#include "Game/PinGuard.h"

#include "Faceit/FaceitParser.h"
#include "Game/PlayerController.h"
#include "Runtime/Common.h"

#include <entity2/entityinstance.h>
#include <khook.hpp>

#include <atomic>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace cfl {
namespace {

using StateChangedFn = void (*)(CEntityInstance *, const NetworkStateChangedData &);

void **g_slot = nullptr;
std::atomic<StateChangedFn> g_original{nullptr};
std::atomic<PinGuard *> g_guard{nullptr};

void *LoadEntry(void **slot)
{
	return *reinterpret_cast<void *volatile *>(slot);
}

#ifndef _WIN32
int PageProtection(uintptr_t address)
{
	FILE *maps = std::fopen("/proc/self/maps", "r");
	if (!maps)
		return PROT_READ;
	int protection = PROT_READ;
	char line[512];
	while (std::fgets(line, sizeof(line), maps))
	{
		unsigned long start = 0, end = 0;
		char perms[8] = {};
		if (std::sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3 || address < start || address >= end)
			continue;
		protection = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
		break;
	}
	std::fclose(maps);
	return protection;
}
#endif

bool WriteEntry(void **slot, void *value)
{
#ifdef _WIN32
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return false;
	InterlockedExchangePointer(slot, value);
	VirtualProtect(slot, sizeof(void *), old, &old);
	return true;
#else
	const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
	const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
	const uintptr_t page = address & ~(pageSize - 1);
	const size_t length = address + sizeof(void *) - page;
	const int protection = PageProtection(address);
	const bool writable = (protection & PROT_WRITE) != 0;
	if (!writable && mprotect(reinterpret_cast<void *>(page), length, protection | PROT_READ | PROT_WRITE) != 0)
		return false;
	__atomic_store_n(slot, value, __ATOMIC_SEQ_CST);
	if (!writable)
		mprotect(reinterpret_cast<void *>(page), length, protection);
	return true;
#endif
}

void KeepModuleLoaded()
{
#ifdef _WIN32
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCWSTR>(&KeepModuleLoaded), &module);
#else
	Dl_info info{};
	if (dladdr(reinterpret_cast<void *>(&KeepModuleLoaded), &info) && info.dli_fname)
		dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

}

PinGuard::~PinGuard()
{
	Shutdown();
}

void PinGuard::Start(void *controllerVTable)
{
	if (active_ || !controllerVTable)
		return;
	const int index = KHook::GetVtableIndex(&CEntityInstance::NetworkStateChanged);
	if (index < 0)
		return;
	void **slot = static_cast<void **>(controllerVTable) + index;
	void *replacement = reinterpret_cast<void *>(&PinGuard::OnStateChanged);
	if (g_slot)
	{
		if (g_slot != slot)
			return;
		g_guard.store(this, std::memory_order_release);
		active_ = true;
		return;
	}
	void *current = LoadEntry(slot);
	if (current == replacement)
		return;
	g_original.store(reinterpret_cast<StateChangedFn>(current), std::memory_order_release);
	g_guard.store(this, std::memory_order_release);
	if (!WriteEntry(slot, replacement))
	{
		g_guard.store(nullptr, std::memory_order_release);
		LogWarning("Pin protection could not be enabled (the player controller vtable is not writable).");
		return;
	}
	g_slot = slot;
	active_ = true;
}

void PinGuard::Shutdown()
{
	wanted_ = {};
	if (!active_)
		return;
	active_ = false;
	g_guard.store(nullptr, std::memory_order_release);
	if (!g_slot)
		return;
	if (LoadEntry(g_slot) == reinterpret_cast<void *>(&PinGuard::OnStateChanged) &&
		WriteEntry(g_slot, reinterpret_cast<void *>(g_original.load(std::memory_order_acquire))))
	{
		g_slot = nullptr;
		return;
	}
	KeepModuleLoaded();
}

void PinGuard::Want(int slot, uint64_t steamId, uint32_t pin)
{
	if (slot >= 0 && slot < static_cast<int>(wanted_.size()) && steamId != 0)
		wanted_[slot] = {steamId, pin, true};
}

void PinGuard::Block(int slot, uint64_t steamId)
{
	if (slot >= 0 && slot < static_cast<int>(wanted_.size()) && steamId != 0)
		wanted_[slot] = {steamId, 0, false};
}

void PinGuard::Forget(int slot)
{
	if (slot >= 0 && slot < static_cast<int>(wanted_.size()))
		wanted_[slot] = {};
}

void PinGuard::OnStateChanged(CEntityInstance *controller, const NetworkStateChangedData &data)
{
	g_original.load(std::memory_order_acquire)(controller, data);
	if (PinGuard *guard = g_guard.load(std::memory_order_acquire))
		guard->Check(controller, data);
}

void PinGuard::Check(CEntityInstance *controller, const NetworkStateChangedData &data)
{
	if (applying_ || !OnGameThread())
		return;
	const int slot = game::Slot(controller);
	if (slot < 0 || wanted_[slot].steamId == 0 || game::SteamId(controller) != wanted_[slot].steamId)
		return;
	if (!game::IsInventoryChange(controller, data))
		return;
	if (!wanted_[slot].force)
	{
		uint32_t current = 0;
		if (!game::GetCoin(controller, current) || !IsLevelPin(current))
			return;
	}
	applying_ = true;
	bool changed = false;
	game::SetCoin(controller, wanted_[slot].pin, changed);
	applying_ = false;
}

}
