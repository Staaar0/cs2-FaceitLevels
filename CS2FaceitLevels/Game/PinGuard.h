#pragma once

#include <array>
#include <cstdint>

class CEntityInstance;
struct NetworkStateChangedData;

namespace cfl {

class PinGuard
{
public:
	PinGuard() = default;
	~PinGuard();

	void Start(void *controllerVTable);
	void Shutdown();
	bool Active() const { return active_; }

	void Want(int slot, uint64_t steamId, uint32_t pin);
	void Block(int slot, uint64_t steamId);
	void Forget(int slot);

private:
	struct Wanted
	{
		uint64_t steamId = 0;
		uint32_t pin = 0;
		bool force = false;
	};

	static void OnStateChanged(CEntityInstance *controller, const NetworkStateChangedData &data);
	void Check(CEntityInstance *controller, const NetworkStateChangedData &data);

	std::array<Wanted, 64> wanted_{};
	bool active_ = false;
	bool applying_ = false;
};

}
