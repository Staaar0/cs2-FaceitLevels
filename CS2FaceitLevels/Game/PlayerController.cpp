#include "Game/PlayerController.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include <entity2/entityidentity.h>
#include <entity2/entityinstance.h>
#include <gameentitysystem.h>
#include <interfaces/interfaces.h>
#include <networksystem/networkvar.h>
#include <schemalib/schemaclassinfo.h>
#include <schemasystem/schemasystem.h>

namespace cfl::game {
namespace {

std::ptrdiff_t g_steamIdOffset = -1;
std::ptrdiff_t g_connectedOffset = -1;
std::ptrdiff_t g_nameOffset = -1;
std::ptrdiff_t g_teamOffset = -1;
std::ptrdiff_t g_inventoryOffset = -1;
std::ptrdiff_t g_rankOffset = -1;
std::ptrdiff_t g_chainOffset = -1;

constexpr int kCoinRank = 5;
constexpr int kPlayerConnected = 0;

std::ptrdiff_t FieldOffset(CSchemaSystemTypeScope *scope, const char *className, const char *fieldName)
{
	const CSchemaClassInfo *info = scope ? scope->FindDeclaredClass(className).Get() : nullptr;
	for (int i = 0; info && i < info->m_nNumFields; ++i)
	{
		const SchemaClassFieldData_t &field = info->m_pFields[i];
		if (field.m_pName && std::strcmp(field.m_pName, fieldName) == 0)
			return field.m_nSingleInheritanceOffset;
	}
	return -1;
}

template <typename T>
T &Field(void *object, std::ptrdiff_t offset)
{
	return *reinterpret_cast<T *>(static_cast<std::byte *>(object) + offset);
}

void *Inventory(CEntityInstance *controller)
{
	return controller ? Field<void *>(controller, g_inventoryOffset) : nullptr;
}

}

bool Init(std::string &error)
{
	if (!g_pSchemaSystem)
	{
		error = "the schema system interface is missing";
		return false;
	}
#ifdef _WIN32
	CSchemaSystemTypeScope *scope = g_pSchemaSystem->FindTypeScopeForModule("server.dll");
#else
	CSchemaSystemTypeScope *scope = g_pSchemaSystem->FindTypeScopeForModule("libserver.so");
#endif
	g_steamIdOffset = FieldOffset(scope, "CBasePlayerController", "m_steamID");
	g_connectedOffset = FieldOffset(scope, "CBasePlayerController", "m_iConnected");
	g_nameOffset = FieldOffset(scope, "CBasePlayerController", "m_iszPlayerName");
	g_teamOffset = FieldOffset(scope, "CBaseEntity", "m_iTeamNum");
	g_inventoryOffset = FieldOffset(scope, "CCSPlayerController", "m_pInventoryServices");
	g_rankOffset = FieldOffset(scope, "CCSPlayerController_InventoryServices", "m_rank");
	g_chainOffset = FieldOffset(scope, "CPlayerControllerComponent", "__m_pChainEntity");
	if (Ready())
		return true;

	char message[256];
	std::snprintf(message, sizeof(message),
		"CS2's schema is missing a player field (m_steamID %d, m_pInventoryServices %d, m_rank %d, __m_pChainEntity %d)",
		static_cast<int>(g_steamIdOffset), static_cast<int>(g_inventoryOffset), static_cast<int>(g_rankOffset),
		static_cast<int>(g_chainOffset));
	error = message;
	return false;
}

bool Ready()
{
	return g_steamIdOffset >= 0 && g_inventoryOffset >= 0 && g_rankOffset >= 0 && g_chainOffset >= 0;
}

CEntityInstance *Controller(int slot)
{
	if (slot < 0 || slot >= 64 || !Ready())
		return nullptr;
	CGameEntitySystem *entities = GameEntitySystem();
	CEntityInstance *controller = entities ? entities->GetPlayerController(CPlayerSlot(slot)) : nullptr;
	if (!controller || !controller->m_pEntity)
		return nullptr;
	const char *classname = controller->GetClassname();
	return classname && std::strcmp(classname, "cs_player_controller") == 0 ? controller : nullptr;
}

uint64_t SteamId(CEntityInstance *controller)
{
	return controller ? Field<uint64_t>(controller, g_steamIdOffset) : 0;
}

bool IsConnected(CEntityInstance *controller)
{
	if (!controller)
		return false;
	return g_connectedOffset < 0 || Field<int32_t>(controller, g_connectedOffset) == kPlayerConnected;
}

const char *Name(CEntityInstance *controller)
{
	if (!controller || g_nameOffset < 0)
		return "?";
	return &Field<char>(controller, g_nameOffset);
}

int Team(CEntityInstance *controller)
{
	if (!controller || g_teamOffset < 0)
		return 0;
	return Field<uint8_t>(controller, g_teamOffset);
}

bool InventoryReady(CEntityInstance *controller)
{
	return Inventory(controller) != nullptr;
}

bool GetCoin(CEntityInstance *controller, uint32_t &coin)
{
	void *inventory = Inventory(controller);
	if (!inventory)
		return false;
	coin = (&Field<uint32_t>(inventory, g_rankOffset))[kCoinRank];
	return true;
}

bool SetCoin(CEntityInstance *controller, uint32_t coin, bool &changed)
{
	changed = false;
	void *inventory = Inventory(controller);
	if (!inventory)
		return false;
	uint32_t &current = (&Field<uint32_t>(inventory, g_rankOffset))[kCoinRank];
	if (current == coin)
		return true;
	current = coin;
	changed = true;

	auto &chainer = Field<CNetworkVarChainer>(inventory, g_chainOffset);
	if (static_cast<CEntityInstance *>(chainer) == controller)
	{
		NetworkStateChangedData data(static_cast<uint32>(g_rankOffset), kCoinRank);
		chainer.NetworkStateChanged(data);
	}
	controller->NetworkStateChanged(NetworkStateChangedData(static_cast<uint32>(g_inventoryOffset)));
	return true;
}

int Slot(CEntityInstance *controller)
{
	if (!controller || !controller->m_pEntity)
		return -1;
	const int slot = controller->GetEntityIndex().Get() - 1;
	return slot >= 0 && slot < 64 ? slot : -1;
}

bool IsInventoryChange(CEntityInstance *controller, const NetworkStateChangedData &data)
{
	if (!Ready() || !controller)
		return false;
	if (data.m_bPartialChange == 0)
		return true;
	if (data.m_nPathIndex.m_Value < 0)
	{
		for (int i = 0; i < data.m_LocalOffsets.Count(); ++i)
		{
			if (data.m_LocalOffsets[i] == static_cast<uint32>(g_inventoryOffset))
				return true;
		}
		return false;
	}
	void *inventory = Inventory(controller);
	if (!inventory)
		return false;
	return Field<CNetworkVarChainer>(inventory, g_chainOffset).m_PathIndex.m_Value == data.m_nPathIndex.m_Value;
}

}
