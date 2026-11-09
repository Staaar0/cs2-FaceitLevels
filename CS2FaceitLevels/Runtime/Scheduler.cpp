#include "Runtime/Scheduler.h"

#include "Runtime/Common.h"

#include <engine2/isource2server.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

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

Scheduler::TaskId Scheduler::After(double seconds, std::function<void()> task)
{
	if (!task || !std::isfinite(seconds))
		return kInvalidTask;
	std::lock_guard<std::mutex> lock(mutex_);
	if (stopped_)
		return kInvalidTask;

	const TaskId id = ++order_;
	auto entry = tasks_.emplace(Deadline{Now() + (std::max)(seconds, kMinimumDelay), id}, std::move(task)).first;
	queued_.emplace(id, entry);
	UpdateNextDue();
	return id;
}

bool Scheduler::Cancel(TaskId id)
{
	std::function<void()> dropped;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = queued_.find(id);
		if (it == queued_.end())
			return false;
		dropped = std::move(it->second->second);
		tasks_.erase(it->second);
		queued_.erase(it);
		UpdateNextDue();
	}
	return true;
}

void Scheduler::UpdateNextDue()
{
	nextDue_.store(tasks_.empty() ? std::numeric_limits<double>::infinity() : tasks_.begin()->first.first,
		std::memory_order_release);
}

size_t Scheduler::Pending() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return tasks_.size();
}

void Scheduler::Shutdown()
{
	Tasks dropped;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = true;
		dropped.swap(tasks_);
		queued_.clear();
		UpdateNextDue();
	}
	if (hook_)
		hook_->Deactivate();
}

KHook::Return<void> Scheduler::OnFrame(FrameHook &, ISource2Server *, bool, bool, bool)
{
	MarkGameThread();
	if (running_ || stopped_)
		return {KHook::Action::Ignore};

	const double nextDue = nextDue_.load(std::memory_order_acquire);
	if (!std::isfinite(nextDue))
		return {KHook::Action::Ignore};
	const double now = Now();
	if (now < nextDue)
		return {KHook::Action::Ignore};

	std::vector<std::function<void()>> due;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		while (!tasks_.empty() && tasks_.begin()->first.first <= now)
		{
			auto it = tasks_.begin();
			due.push_back(std::move(it->second));
			queued_.erase(it->first.second);
			tasks_.erase(it);
		}
		UpdateNextDue();
	}
	if (due.empty())
		return {KHook::Action::Ignore};

	running_ = true;
	for (auto &task : due)
	{
		if (stopped_)
			break;
		try
		{
			task();
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
