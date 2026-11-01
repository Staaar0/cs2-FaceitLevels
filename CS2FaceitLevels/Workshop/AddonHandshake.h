#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cfl::workshop {

constexpr const char *kAddonId = "3724637448";

using Slots = std::map<int, uint64_t>;

class WorkshopError : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

std::vector<std::string> Required(std::string_view serverAddons, std::string_view badgeAddon);
std::optional<std::string> FirstAddon(std::string_view addons);

class ConnectionDeadlines
{
public:
	static constexpr double kTimeoutSeconds = 30;

	struct Waiting
	{
		int slot = 0;
		uint64_t steamId = 0;
		double started = 0;
		bool operator==(const Waiting &other) const
		{
			return slot == other.slot && steamId == other.steamId && started == other.started;
		}
	};

	bool Wait(int slot, uint64_t steamId, double now);
	void Remove(int slot) { waiting_.erase(slot); }
	void Clear() { waiting_.clear(); }
	size_t Count() const { return waiting_.size(); }
	std::vector<Waiting> Expired(double now) const;
	bool TakeExpired(const Waiting &pending, uint64_t currentSteamId, double now);

private:
	std::map<int, Waiting> waiting_;
};

class SendHookSchedule
{
public:
	static constexpr double kMapChangeSeconds = 600;
	static constexpr double kLoadingSeconds = 300;

	void Loading(int slot, double now) { loading_.emplace(slot, now); }
	void Loaded(int slot) { loading_.erase(slot); }
	void MapChangeRequested(double now) { mapChangeStarted_ = now; }
	void MapStarted() { mapChangeStarted_.reset(); }
	bool CanRelease(double now, size_t waitingDeadlines);
	void Clear();

private:
	std::map<int, double> loading_;
	std::optional<double> mapChangeStarted_;
};

class AddonHandshake
{
public:
	static constexpr size_t kMaxSessions = 256;

	struct Session
	{
		std::set<std::string> downloaded;
		std::optional<std::string> pending;
		double lastActivity = 0;
		bool active = false;
	};

	size_t Count() const { return sessions_.size(); }

	std::string Reply(uint64_t steamId, const std::vector<std::string> &required, double now);
	void Connected(uint64_t steamId, double now);
	std::optional<std::string> Next(uint64_t steamId, const std::vector<std::string> &required, double now);
	void ChangingMap(uint64_t steamId, const std::optional<std::string> &firstAddon, double now);
	void MapChanging(double now);
	void MapStarted(double now);
	void Active(uint64_t steamId, double now);
	void Disconnected(uint64_t steamId, double now);
	void Prune(double now);
	void Clear() { sessions_.clear(); }
	void Forget(uint64_t steamId) { sessions_.erase(steamId); }

	const Session *Find(uint64_t steamId) const;

private:
	Session &Get(uint64_t steamId, double now);

	std::unordered_map<uint64_t, Session> sessions_;
};

}
