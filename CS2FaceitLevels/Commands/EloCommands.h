#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cfl {

class EloCommands
{
public:
	struct Player
	{
		int slot = -1;
		uint64_t steamId = 0;
		std::string name;
		int team = 0;
	};

	using Players = std::function<std::vector<Player>()>;
	using LookupDone = std::function<void(int level, int elo)>;
	using Lookup = std::function<void(uint64_t steamId, LookupDone done)>;
	using Print = std::function<void(int slot, uint64_t steamId, const std::string &message)>;

	EloCommands(Players players, Lookup lookup, Print print);
	~EloCommands();

	bool OnSay(int slot, uint64_t steamId, std::string_view message, bool &silent);
	void Forget(int slot);

private:
	struct Caller
	{
		uint64_t steamId = 0;
		uint64_t generation = 0;
		bool pending = false;
		std::optional<double> lastUse;
	};

	Caller &Get(int slot, uint64_t steamId);
	bool CanUse(Caller &caller);
	bool IsCurrent(int slot, uint64_t steamId, uint64_t generation) const;
	void Single(int slot, std::string_view search);
	void All(int slot);

	Players players_;
	Lookup lookup_;
	Print print_;
	std::array<Caller, 64> callers_{};
	uint64_t generation_ = 0;
	std::shared_ptr<bool> alive_;
};

}
