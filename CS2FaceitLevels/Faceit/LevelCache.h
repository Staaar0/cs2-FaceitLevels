#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cfl {

struct LevelEntry
{
	int level;
	int elo;
	int64_t expiresAt;
};

class LevelCache
{
public:
	static constexpr size_t kMaxEntries = 10000;

	explicit LevelCache(std::string path) : path_(std::move(path)) {}

	bool TryGetFresh(uint64_t steamId, LevelEntry &entry) const;
	void Store(uint64_t steamId, LevelEntry entry);
	void DropFailures();

	void Load();
	void SaveIfChanged();

	size_t Size() const { return entries_.size(); }

	static std::string Serialize(const std::unordered_map<uint64_t, LevelEntry> &entries, int64_t now);
	static std::unordered_map<uint64_t, LevelEntry> Parse(std::string_view text, int64_t now);

private:
	void Compact();
	void Remove(std::unordered_map<uint64_t, LevelEntry>::iterator it);

	std::string path_;
	std::unordered_map<uint64_t, LevelEntry> entries_;
	uint64_t version_ = 0;
	uint64_t savedVersion_ = 0;
};

}
