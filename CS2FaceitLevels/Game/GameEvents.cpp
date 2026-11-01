#include "Game/GameEvents.h"

#include "Runtime/Common.h"

#include <playerslot.h>

#include <cstring>

namespace cfl {

GameEvents::GameEvents(HookKeeper &hooks, Handler handler)
	: hooks_(hooks), handler_(std::move(handler))
{
}

GameEvents::~GameEvents()
{
	Shutdown();
}

void GameEvents::Start(void *managerVTable)
{
	if (manager_ || capture_ || !managerVTable)
		return;
	capture_ = hooks_.Make<CaptureHook>(&IGameEventManager2::FireEvent, this, &GameEvents::OnFireEvent, nullptr);
	capture_->AttachVTable(managerVTable);
}

void GameEvents::Shutdown()
{
	if (manager_)
	{
		manager_->RemoveListener(this);
		manager_ = nullptr;
	}
	if (capture_)
		capture_->Deactivate();
}

KHook::Return<bool> GameEvents::OnFireEvent(CaptureHook &, IGameEventManager2 *manager, IGameEvent *, bool)
{
	if (manager_ || !manager || !OnGameThread())
		return {KHook::Action::Ignore};

	manager_ = manager;
	manager_->AddListener(this, "player_spawn", true);
	manager_->AddListener(this, "player_connect_full", true);
	capture_->Deactivate();
	return {KHook::Action::Ignore};
}

void GameEvents::FireGameEvent(IGameEvent *event)
{
	if (!event || !manager_ || !OnGameThread())
		return;
	const char *name = event->GetName();
	if (!name)
		return;

	Event kind;
	if (std::strcmp(name, "player_spawn") == 0)
		kind = Event::PlayerSpawn;
	else if (std::strcmp(name, "player_connect_full") == 0)
		kind = Event::PlayerConnectFull;
	else
		return;

	const CPlayerSlot slot = event->GetPlayerSlot("userid");
	if (slot.IsValid())
		handler_(kind, slot.Get());
}

}
