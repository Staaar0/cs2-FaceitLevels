#include "Faceit/LevelCache.h"

#include "Runtime/Common.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace cfl {
namespace {

constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;
constexpr std::string_view kHeader = "CS2FaceitLevels cache 2";
constexpr std::string_view kHeaderWithoutElo = "CS2FaceitLevels cache 1";

bool ParseInt64(std::string_view text, int64_t &value)
{
	bool negative = !text.empty() && text.front() == '-';
	if (negative)
		text.remove_prefix(1);
	uint64_t magnitude = 0;
	if (!ParseUint64(text, magnitude) || magnitude > static_cast<uint64_t>(INT64_MAX))
		return false;
	value = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
	return true;
}

}

bool LevelCache::TryGetFresh(uint64_t steamId, LevelEntry &entry) const
{
	auto it = entries_.find(steamId);
	if (it == entries_.end() || it->second.expiresAt <= UnixNow())
		return false;
	entry = it->second;
	return true;
}

void LevelCache::Store(uint64_t steamId, LevelEntry entry)
{
	entries_[steamId] = entry;
	if (entry.level >= 0)
		++version_;
	if (entries_.size() > kMaxEntries)
		Compact();
}

void LevelCache::DropFailures()
{
	for (auto it = entries_.begin(); it != entries_.end();)
	{
		auto current = it++;
		if (current->second.level < 0)
			Remove(current);
	}
}

void LevelCache::Remove(std::unordered_map<uint64_t, LevelEntry>::iterator it)
{
	if (it->second.level >= 0)
		++version_;
	entries_.erase(it);
}

void LevelCache::Compact()
{
	const int64_t now = UnixNow();
	auto oldest = entries_.end();
	for (auto it = entries_.begin(); it != entries_.end();)
	{
		if (it->second.expiresAt <= now)
		{
			auto expired = it++;
			if (oldest == expired)
				oldest = entries_.end();
			Remove(expired);
			continue;
		}
		if (oldest == entries_.end() || it->second.expiresAt < oldest->second.expiresAt)
			oldest = it;
		++it;
	}
	if (entries_.size() > kMaxEntries && oldest != entries_.end())
		Remove(oldest);
}

std::unordered_map<uint64_t, LevelEntry> LevelCache::Parse(std::string_view text, int64_t now)
{
	std::unordered_map<uint64_t, LevelEntry> result;
	auto lines = SplitNonEmpty(text, '\n');
	if (lines.empty())
		return result;
	const std::string_view header = Trim(lines.front());
	const bool withElo = header == kHeader;
	if (!withElo && header != kHeaderWithoutElo)
		return result;

	std::vector<std::pair<uint64_t, LevelEntry>> valid;
	for (size_t i = 1; i < lines.size(); ++i)
	{
		auto fields = SplitNonEmpty(Trim(lines[i]), ' ');
		uint64_t steamId = 0;
		int64_t level = 0;
		int64_t elo = -1;
		int64_t expires = 0;
		if (fields.size() != (withElo ? 4u : 3u) || !ParseUint64(fields[0], steamId) || !ParseInt64(fields[1], level) ||
			(withElo && !ParseInt64(fields[2], elo)) || !ParseInt64(fields.back(), expires))
			continue;
		if (steamId == 0 || level < 0 || level > 11 || elo < -1 || elo > 100000 || expires <= now)
			continue;
		valid.push_back({steamId, {static_cast<int>(level), static_cast<int>(elo), expires}});
	}
	std::stable_sort(valid.begin(), valid.end(),
		[](const auto &a, const auto &b) { return a.second.expiresAt > b.second.expiresAt; });
	for (const auto &entry : valid)
	{
		if (result.size() >= kMaxEntries)
			break;
		result.emplace(entry.first, entry.second);
	}
	return result;
}

std::string LevelCache::Serialize(const std::unordered_map<uint64_t, LevelEntry> &entries, int64_t now)
{
	std::vector<std::pair<uint64_t, LevelEntry>> keep;
	keep.reserve(entries.size());
	for (const auto &entry : entries)
		if (entry.second.level >= 0 && entry.second.expiresAt > now)
			keep.push_back(entry);
	std::sort(keep.begin(), keep.end(), [](const auto &a, const auto &b) {
		return a.second.expiresAt != b.second.expiresAt ? a.second.expiresAt > b.second.expiresAt : a.first < b.first;
	});
	if (keep.size() > kMaxEntries)
		keep.resize(kMaxEntries);
	std::sort(keep.begin(), keep.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

	std::string text(kHeader);
	text += '\n';
	char line[96];
	for (const auto &entry : keep)
	{
		std::snprintf(line, sizeof(line), "%llu %d %d %lld\n", static_cast<unsigned long long>(entry.first),
			entry.second.level, entry.second.elo, static_cast<long long>(entry.second.expiresAt));
		text += line;
	}
	return text;
}

void LevelCache::Load()
{
	std::string text;
	if (!FileExists(path_))
		return;
	if (!ReadFile(path_, text, kMaxFileBytes))
	{
		LogWarning("Cache file %s is unreadable or larger than 8 MB; ignoring it.", path_.c_str());
		return;
	}
	const int64_t now = UnixNow();
	auto loaded = Parse(text, now);
	const size_t lines = SplitNonEmpty(text, '\n').size();
	for (auto &entry : loaded)
		entries_[entry.first] = entry.second;
	if (lines != loaded.size() + 1)
		++version_;
}

void LevelCache::SaveIfChanged()
{
	if (version_ == savedVersion_)
		return;
	const uint64_t version = version_;
	if (WriteFileAtomic(path_, Serialize(entries_, UnixNow())))
		savedVersion_ = version;
	else
		LogWarning("Could not write cache file %s.", path_.c_str());
}

}
