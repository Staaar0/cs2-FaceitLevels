#pragma once

#include <cstdint>
#include <string>

class CEntityInstance;
struct NetworkStateChangedData;

namespace cfl::game {

bool Init(std::string &error);
bool Ready();

CEntityInstance *Controller(int slot);

uint64_t SteamId(CEntityInstance *controller);
bool IsConnected(CEntityInstance *controller);
const char *Name(CEntityInstance *controller);
int Team(CEntityInstance *controller);

bool InventoryReady(CEntityInstance *controller);
bool GetCoin(CEntityInstance *controller, uint32_t &coin);
bool SetCoin(CEntityInstance *controller, uint32_t coin, bool &changed);
int Slot(CEntityInstance *controller);
bool IsInventoryChange(CEntityInstance *controller, const NetworkStateChangedData &data);

}
