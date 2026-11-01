#include "Commands/EloCommands.h"

#include "Commands/ChatFormatter.h"
#include "Runtime/Common.h"

#include <algorithm>
#include <utility>

namespace cfl {
namespace {

constexpr double kCooldownSeconds = 1.0;
constexpr size_t kMaxLines = 32;
constexpr size_t kMaxListedNames = 5;

bool EqualsAscii(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
	{
		char x = a[i];
		char y = b[i];
		if (x >= 'A' && x <= 'Z')
			x = static_cast<char>(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = static_cast<char>(y - 'A' + 'a');
		if (x != y)
			return false;
	}
	return true;
}

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string JoinArgs(std::string_view text)
{
	std::string result;
	std::string word;
	auto flush = [&] {
		if (word.empty())
			return;
		if (!result.empty())
			result += ' ';
		result += word;
		word.clear();
	};
	for (char c : text)
	{
		if (c == '"')
			continue;
		if (IsSpace(c))
			flush();
		else
			word += c;
	}
	flush();
	return result;
}

bool NameLess(const std::string &a, const std::string &b)
{
	const std::string foldedA = chat::FoldCase(a);
	const std::string foldedB = chat::FoldCase(b);
	return foldedA != foldedB ? foldedA < foldedB : a < b;
}

}

EloCommands::EloCommands(Players players, Lookup lookup, Print print)
	: players_(std::move(players)), lookup_(std::move(lookup)), print_(std::move(print)), alive_(std::make_shared<bool>(true))
{
}

EloCommands::~EloCommands()
{
	*alive_ = false;
}

bool EloCommands::OnSay(int slot, uint64_t steamId, std::string_view message, bool &silent)
{
	silent = false;
	if (slot < 0 || slot >= static_cast<int>(callers_.size()) || steamId == 0)
		return false;
	while (!message.empty() && IsSpace(message.front()))
		message.remove_prefix(1);
	if (message.size() < 2 || (message.front() != '!' && message.front() != '/'))
		return false;
	const bool hidden = message.front() == '/';
	message.remove_prefix(1);
	size_t end = 0;
	while (end < message.size() && !IsSpace(message[end]))
		++end;
	const std::string_view word = message.substr(0, end);
	const bool single = EqualsAscii(word, "elo");
	if (!single && !EqualsAscii(word, "elos"))
		return false;

	silent = hidden;
	Caller &caller = Get(slot, steamId);
	if (!CanUse(caller))
		return true;
	if (single)
		Single(slot, JoinArgs(message.substr(end)));
	else
		All(slot);
	return true;
}

void EloCommands::Forget(int slot)
{
	if (slot >= 0 && slot < static_cast<int>(callers_.size()))
		callers_[slot] = Caller{};
}

EloCommands::Caller &EloCommands::Get(int slot, uint64_t steamId)
{
	Caller &caller = callers_[slot];
	if (caller.steamId != steamId || caller.generation == 0)
	{
		caller = Caller{};
		caller.steamId = steamId;
		caller.generation = ++generation_;
	}
	return caller;
}

bool EloCommands::CanUse(Caller &caller)
{
	if (caller.pending)
		return false;
	const double now = Now();
	if (caller.lastUse && now - *caller.lastUse < kCooldownSeconds)
		return false;
	caller.lastUse = now;
	return true;
}

bool EloCommands::IsCurrent(int slot, uint64_t steamId, uint64_t generation) const
{
	return slot >= 0 && slot < static_cast<int>(callers_.size()) && callers_[slot].steamId == steamId &&
		callers_[slot].generation == generation;
}

void EloCommands::Single(int slot, std::string_view search)
{
	Caller &caller = callers_[slot];
	if (search.empty())
	{
		print_(slot, caller.steamId, chat::MissingName());
		return;
	}

	const std::string needle = chat::FoldCase(search);
	std::vector<Player> matches;
	for (Player &player : players_())
		if (chat::FoldCase(player.name).find(needle) != std::string::npos)
			matches.push_back(std::move(player));
	std::sort(matches.begin(), matches.end(), [](const Player &a, const Player &b) { return NameLess(a.name, b.name); });

	if (matches.empty())
	{
		print_(slot, caller.steamId, chat::NoMatch(search));
		return;
	}
	if (matches.size() > 1)
	{
		std::string names;
		for (size_t i = 0; i < matches.size() && i < kMaxListedNames; ++i)
		{
			if (!names.empty())
				names += ", ";
			names += matches[i].name;
		}
		print_(slot, caller.steamId, chat::Multiple(names));
		return;
	}

	caller.pending = true;
	const uint64_t steamId = caller.steamId;
	const uint64_t generation = caller.generation;
	std::weak_ptr<bool> alive = alive_;
	lookup_(matches.front().steamId, [this, alive, slot, steamId, generation, name = matches.front().name](int level, int elo) {
		auto flag = alive.lock();
		if (!flag || !*flag || !IsCurrent(slot, steamId, generation))
			return;
		callers_[slot].pending = false;
		print_(slot, steamId, chat::EloLine(name, level, elo));
	});
}

void EloCommands::All(int slot)
{
	std::vector<Player> targets = players_();
	std::sort(targets.begin(), targets.end(), [](const Player &a, const Player &b) {
		return a.team != b.team ? a.team < b.team : NameLess(a.name, b.name);
	});
	if (targets.size() > kMaxLines)
		targets.resize(kMaxLines);
	if (targets.empty())
		return;

	Caller &caller = callers_[slot];
	caller.pending = true;
	const uint64_t steamId = caller.steamId;
	const uint64_t generation = caller.generation;

	struct Collected
	{
		std::vector<std::string> lines;
		size_t remaining = 0;
	};
	auto collected = std::make_shared<Collected>();
	collected->lines.resize(targets.size());
	collected->remaining = targets.size();

	std::weak_ptr<bool> alive = alive_;
	for (size_t i = 0; i < targets.size(); ++i)
	{
		lookup_(targets[i].steamId, [this, alive, collected, i, slot, steamId, generation, name = targets[i].name](int level, int elo) {
			auto flag = alive.lock();
			if (!flag || !*flag)
				return;
			collected->lines[i] = chat::EloLine(name, level, elo);
			if (--collected->remaining != 0 || !IsCurrent(slot, steamId, generation))
				return;
			callers_[slot].pending = false;
			for (const std::string &line : collected->lines)
				print_(slot, steamId, line);
		});
	}
}

}
