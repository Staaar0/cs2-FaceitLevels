#pragma once

#include "Runtime/Hooks.h"

#include <igameevents.h>

#include <functional>

namespace cfl {

class GameEvents final : public IGameEventListener2
{
public:
	enum class Event { PlayerSpawn, PlayerConnectFull };
	using Handler = std::function<void(Event event, int slot)>;

	GameEvents(HookKeeper &hooks, Handler handler);
	~GameEvents() override;

	void Start(void *managerVTable);
	void Shutdown();
	bool Listening() const { return manager_ != nullptr; }

	void FireGameEvent(IGameEvent *event) override;

private:
	using CaptureHook = VirtualHook<GameEvents, IGameEventManager2, bool, IGameEvent *, bool>;
	KHook::Return<bool> OnFireEvent(CaptureHook &hook, IGameEventManager2 *manager, IGameEvent *event, bool dontBroadcast);

	HookKeeper &hooks_;
	Handler handler_;
	CaptureHook *capture_ = nullptr;
	IGameEventManager2 *manager_ = nullptr;
};

}
