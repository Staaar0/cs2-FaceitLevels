#pragma once

#include "Runtime/Hooks.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class ISource2Server;

namespace cfl {

class Scheduler
{
public:
	Scheduler(ISource2Server *server, HookKeeper &hooks);
	~Scheduler();

	void After(double seconds, std::function<void()> task);
	void NextFrame(std::function<void()> task) { After(0.0, std::move(task)); }

	void Shutdown();

	size_t Pending() const;

private:
	using FrameHook = VirtualHook<Scheduler, ISource2Server, void, bool, bool, bool>;

	KHook::Return<void> OnFrame(FrameHook &hook, ISource2Server *server, bool simulating, bool firstTick, bool lastTick);

	struct Task
	{
		double due;
		uint64_t order;
		std::function<void()> run;
	};

	FrameHook *hook_ = nullptr;
	mutable std::mutex mutex_;
	std::vector<Task> tasks_;
	uint64_t order_ = 0;
	bool running_ = false;
	std::atomic<bool> stopped_{false};
};

}
