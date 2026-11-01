#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cfl {

struct FaceitPlayerReply
{
	std::optional<int> skillLevel;
	std::optional<int> elo;
	std::string playerId;
	std::string region;
};

bool ParseFaceitPlayer(std::string_view body, FaceitPlayerReply &reply);
bool ParseFaceitRankingPosition(std::string_view body, int &position);

std::optional<double> ParseRetryAfter(std::string_view value, int64_t nowUnix);

std::string EscapeDataString(std::string_view text);

uint32_t LevelPin(int level);
bool IsLevelPin(uint32_t pin);

}
