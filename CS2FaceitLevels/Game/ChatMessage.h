#pragma once

#include <string>

class IGameEventSystem;

namespace cfl::game {

void SetGameEventSystem(IGameEventSystem *system);
bool PrintToChat(int slot, const std::string &message);

}
