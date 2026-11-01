#include "Workshop/AddonHandshake.h"

#include "Runtime/Common.h"

#include <algorithm>

namespace cfl::workshop {
namespace {

constexpr double kSessionResetSeconds = 120;
constexpr double kSessionPruneSeconds = 600;
constexpr size_t kMaxAddons = 16;

bool IsLetter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool ValidAddonId(std::string_view value)
{
	if (value.empty())
		return false;
	if (value.front() >= '0' && value.front() <= '9')
	{
		uint64_t id = 0;
		return value.size() <= 20 && ParseUint64(value, id) && id != 0;
	}
	if (value.size() > 64 || !IsLetter(value.front()))
		return false;
	for (char c : value)
		if (!IsLetter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.')
			return false;
	return true;
}

void Restart(AddonHandshake::Session &session, double now)
{
	session.downloaded.clear();
	session.pending.reset();
	session.active = false;
	session.lastActivity = now;
}

}

std::vector<std::string> Required(std::string_view serverAddons, std::string_view badgeAddon)
{
	std::vector<std::string> result;
	for (std::string_view raw : SplitNonEmpty(serverAddons, ','))
	{
		std::string_view id = Trim(raw);
		if (!ValidAddonId(id))
			throw WorkshopError("The engine addon list has an unexpected format.");
		if (id != badgeAddon && std::find(result.begin(), result.end(), id) == result.end())
			result.emplace_back(id);
	}
	result.emplace_back(badgeAddon);
	if (result.size() > kMaxAddons)
		throw WorkshopError("Too many addons for the Workshop loader.");
	return result;
}

std::optional<std::string> FirstAddon(std::string_view addons)
{
	for (std::string_view raw : SplitNonEmpty(addons, ','))
	{
		std::string_view id = Trim(raw);
		if (!id.empty())
			return ValidAddonId(id) ? std::optional<std::string>(id) : std::nullopt;
	}
	return std::nullopt;
}

bool ConnectionDeadlines::Wait(int slot, uint64_t steamId, double now)
{
	if (slot < 0 || slot >= 256 || steamId == 0)
		throw WorkshopError("Invalid Workshop connection identity.");
	auto it = waiting_.find(slot);
	if (it == waiting_.end() || it->second.steamId != steamId)
		it = waiting_.insert_or_assign(slot, Waiting{slot, steamId, now}).first;
	return now - it->second.started >= kTimeoutSeconds;
}

std::vector<ConnectionDeadlines::Waiting> ConnectionDeadlines::Expired(double now) const
{
	std::vector<Waiting> result;
	for (const auto &entry : waiting_)
		if (now - entry.second.started >= kTimeoutSeconds)
			result.push_back(entry.second);
	return result;
}

bool ConnectionDeadlines::TakeExpired(const Waiting &pending, uint64_t currentSteamId, double now)
{
	auto it = waiting_.find(pending.slot);
	if (it == waiting_.end() || !(it->second == pending) || now - it->second.started < kTimeoutSeconds)
		return false;
	waiting_.erase(it);
	return currentSteamId == pending.steamId;
}

bool SendHookSchedule::CanRelease(double now, size_t waitingDeadlines)
{
	if (waitingDeadlines != 0)
		return false;
	if (mapChangeStarted_)
	{
		if (now - *mapChangeStarted_ < kMapChangeSeconds)
			return false;
		mapChangeStarted_.reset();
	}
	if (loading_.empty())
		return true;
	for (auto it = loading_.begin(); it != loading_.end();)
		it = now - it->second >= kLoadingSeconds ? loading_.erase(it) : std::next(it);
	return loading_.empty();
}

void SendHookSchedule::Clear()
{
	loading_.clear();
	mapChangeStarted_.reset();
}

AddonHandshake::Session &AddonHandshake::Get(uint64_t steamId, double now)
{
	if (auto it = sessions_.find(steamId); it != sessions_.end())
		return it->second;
	Prune(now);
	if (sessions_.size() >= kMaxSessions)
	{
		auto victim = std::min_element(sessions_.begin(), sessions_.end(), [](const auto &a, const auto &b) {
			if (a.second.active != b.second.active)
				return !a.second.active;
			return a.second.lastActivity < b.second.lastActivity;
		});
		sessions_.erase(victim);
	}
	Session &created = sessions_[steamId];
	created.lastActivity = now;
	return created;
}

const AddonHandshake::Session *AddonHandshake::Find(uint64_t steamId) const
{
	auto it = sessions_.find(steamId);
	return it == sessions_.end() ? nullptr : &it->second;
}

std::string AddonHandshake::Reply(uint64_t steamId, const std::vector<std::string> &required, double now)
{
	Session &session = Get(steamId, now);
	if (session.active || now - session.lastActivity > kSessionResetSeconds)
	{
		session.downloaded.clear();
		session.pending.reset();
		session.active = false;
	}
	session.lastActivity = now;
	session.pending.reset();
	for (const std::string &id : required)
		if (!session.downloaded.count(id))
		{
			session.pending = id;
			break;
		}

	std::vector<std::string> advertised;
	for (const std::string &id : required)
		if (session.downloaded.count(id) || (session.pending && id == *session.pending))
			advertised.push_back(id);
	return Join(advertised, ',');
}

void AddonHandshake::Connected(uint64_t steamId, double now)
{
	auto it = sessions_.find(steamId);
	if (it == sessions_.end())
		return;
	Session &session = it->second;
	if (session.pending && now - session.lastActivity <= kSessionResetSeconds)
		session.downloaded.insert(*session.pending);
	session.pending.reset();
	session.lastActivity = now;
}

std::optional<std::string> AddonHandshake::Next(uint64_t steamId, const std::vector<std::string> &required, double now)
{
	Session &session = Get(steamId, now);
	if (session.active)
		return std::nullopt;
	std::optional<std::string> next;
	for (const std::string &id : required)
		if (!session.downloaded.count(id))
		{
			next = id;
			break;
		}
	session.lastActivity = now;
	session.pending = next;
	return next;
}

void AddonHandshake::ChangingMap(uint64_t steamId, const std::optional<std::string> &firstAddon, double now)
{
	Session &session = Get(steamId, now);
	session.downloaded.clear();
	session.active = false;
	session.lastActivity = now;
	session.pending = firstAddon;
}

void AddonHandshake::MapChanging(double now)
{
	for (auto &entry : sessions_)
		Restart(entry.second, now);
}

void AddonHandshake::MapStarted(double now)
{
	for (auto &entry : sessions_)
		if (entry.second.active)
			Restart(entry.second, now);
}

void AddonHandshake::Active(uint64_t steamId, double now)
{
	Session &session = Get(steamId, now);
	session.active = true;
	session.pending.reset();
	session.lastActivity = now;
}

void AddonHandshake::Disconnected(uint64_t steamId, double now)
{
	auto it = sessions_.find(steamId);
	if (it == sessions_.end())
		return;
	if (it->second.active)
		sessions_.erase(it);
	else
		it->second.lastActivity = now;
}

void AddonHandshake::Prune(double now)
{
	for (auto it = sessions_.begin(); it != sessions_.end();)
		it = !it->second.active && now - it->second.lastActivity > kSessionPruneSeconds ? sessions_.erase(it) : std::next(it);
}

}
