#include "Runtime/Scheduler.h"

#include "Runtime/Common.h"

#include <engine2/isource2server.h>

#include <algorithm>
#include <exception>

namespace cfl {
namespace {

constexpr double kMinimumDelay = 0.001;

}

Scheduler::Scheduler(ISource2Server *server, HookKeeper &hooks)
{
	if (!server)
		return;
	hook_ = hooks.Make<FrameHook>(&ISource2Server::GameFrame, this, nullptr, &Scheduler::OnFrame);
	hook_->AttachInstance(server);
}

Scheduler::~Scheduler()
{
	Shutdown();
}

void Scheduler::After(double seconds, std::function<void()> task)
{
	if (!task)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	if (stopped_)
		return;
	tasks_.push_back({Now() + (std::max)(seconds, kMinimumDelay), ++order_, std::move(task)});
}

size_t Scheduler::Pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return tasks_.size();
}

void Scheduler::Shutdown()
{
	std::vector<Task> dropped;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = true;
		dropped.swap(tasks_);
	}
	if (hook_)
		hook_->Deactivate();
}

KHook::Return<void> Scheduler::OnFrame(FrameHook &, ISource2Server *, bool, bool, bool)
{
	MarkGameThread();
	if (running_ || stopped_)
		return {KHook::Action::Ignore};

	std::vector<Task> due;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (tasks_.empty())
			return {KHook::Action::Ignore};
		const double now = Now();
		auto ready = std::stable_partition(tasks_.begin(), tasks_.end(), [now](const Task &task) { return task.due > now; });
		due.assign(std::make_move_iterator(ready), std::make_move_iterator(tasks_.end()));
		tasks_.erase(ready, tasks_.end());
	}
	if (due.empty())
		return {KHook::Action::Ignore};
	std::sort(due.begin(), due.end(), [](const Task &a, const Task &b) {
		return a.due != b.due ? a.due < b.due : a.order < b.order;
	});

	running_ = true;
	for (Task &task : due)
	{
		if (stopped_)
			break;
		try
		{
			task.run();
		}
		catch (const std::exception &ex)
		{
			LogError("Scheduled task failed: %s", ex.what());
		}
		catch (...)
		{
			LogError("Scheduled task failed.");
		}
	}
	running_ = false;
	return {KHook::Action::Ignore};
}

}
