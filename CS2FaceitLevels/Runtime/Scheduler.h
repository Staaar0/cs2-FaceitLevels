#pragma once

#include "Runtime/Hooks.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>

class ISource2Server;

namespace cfl {

class Scheduler
{
public:
	using TaskId = uint64_t;
	static constexpr TaskId kInvalidTask = 0;

	Scheduler(ISource2Server *server, HookKeeper &hooks);
	~Scheduler();

	TaskId After(double seconds, std::function<void()> task);
	TaskId NextFrame(std::function<void()> task) { return After(0.0, std::move(task)); }
	bool Cancel(TaskId id);

	void Shutdown();

	size_t Pending() const;

private:
	using FrameHook = VirtualHook<Scheduler, ISource2Server, void, bool, bool, bool>;

	KHook::Return<void> OnFrame(FrameHook &hook, ISource2Server *server, bool simulating, bool firstTick, bool lastTick);

	using Deadline = std::pair<double, TaskId>;
	using Tasks = std::map<Deadline, std::function<void()>>;
	void UpdateNextDue();

	FrameHook *hook_ = nullptr;
	mutable std::mutex mutex_;
	Tasks tasks_;
	std::unordered_map<TaskId, Tasks::iterator> queued_;
	TaskId order_ = 0;
	std::atomic<double> nextDue_{std::numeric_limits<double>::infinity()};
	bool running_ = false;
	std::atomic<bool> stopped_{false};
};

}
