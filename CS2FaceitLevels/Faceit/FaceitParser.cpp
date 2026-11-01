#include "Faceit/FaceitParser.h"

#include "Runtime/Common.h"
#include "CS2FaceitLevels_Config.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace cfl {
namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

const nlohmann::json *Member(const nlohmann::json &object, std::string_view key)
{
	if (!object.is_object())
		return nullptr;
	auto exact = object.find(std::string(key));
	if (exact != object.end())
		return &*exact;
	for (auto it = object.begin(); it != object.end(); ++it)
		if (EqualsIgnoreCase(it.key(), key))
			return &it.value();
	return nullptr;
}

bool OptionalString(const nlohmann::json &object, std::string_view key, std::string &out)
{
	const nlohmann::json *value = Member(object, key);
	if (!value || value->is_null())
		return true;
	if (!value->is_string())
		return false;
	out = value->get<std::string>();
	return true;
}

bool OptionalInt(const nlohmann::json &object, std::string_view key, std::optional<int> &out)
{
	const nlohmann::json *value = Member(object, key);
	if (!value || value->is_null())
		return true;
	if (!value->is_number_integer() && !value->is_number_unsigned())
		return false;
	const long long number = value->get<long long>();
	if (number < INT32_MIN || number > INT32_MAX)
		return false;
	out = static_cast<int>(number);
	return true;
}

nlohmann::json ParseLenient(std::string_view body)
{
	return nlohmann::json::parse(StripJsonExtras(body), nullptr, false);
}

int MonthIndex(std::string_view name)
{
	static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	for (int i = 0; i < 12; ++i)
		if (EqualsIgnoreCase(name, months[i]))
			return i;
	return -1;
}

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

}

bool ParseFaceitPlayer(std::string_view body, FaceitPlayerReply &reply)
{
	reply = {};
	nlohmann::json root = ParseLenient(body);
	if (root.is_discarded())
		return false;
	if (root.is_null())
		return true;
	if (!root.is_object() || !OptionalString(root, "player_id", reply.playerId))
		return false;

	const nlohmann::json *games = Member(root, "games");
	if (!games || games->is_null())
		return true;
	if (!games->is_object())
		return false;
	const nlohmann::json *cs2 = Member(*games, "cs2");
	if (!cs2 || cs2->is_null())
		return true;
	if (!cs2->is_object())
		return false;
	return OptionalInt(*cs2, "skill_level", reply.skillLevel) && OptionalInt(*cs2, "faceit_elo", reply.elo) &&
		OptionalString(*cs2, "region", reply.region);
}

bool ParseFaceitRankingPosition(std::string_view body, int &position)
{
	position = 0;
	nlohmann::json root = ParseLenient(body);
	if (root.is_discarded())
		return false;
	if (root.is_null())
		return true;
	if (!root.is_object())
		return false;

	std::optional<int> direct;
	if (!OptionalInt(root, "position", direct))
		return false;
	if (direct)
	{
		position = *direct;
		return true;
	}
	const nlohmann::json *items = Member(root, "items");
	if (!items || items->is_null())
		return true;
	if (!items->is_array())
		return false;
	if (items->empty() || (*items)[0].is_null())
		return true;
	if (!(*items)[0].is_object())
		return false;
	std::optional<int> first;
	if (!OptionalInt((*items)[0], "position", first))
		return false;
	position = first.value_or(0);
	return true;
}

std::optional<double> ParseRetryAfter(std::string_view value, int64_t nowUnix)
{
	value = Trim(value);
	uint64_t seconds = 0;
	if (ParseUint64(value, seconds))
		return static_cast<double>(seconds > 86400ull * 365 ? 86400ull * 365 : seconds);

	char weekday[4] = {}, month[4] = {}, zone[4] = {};
	int day = 0, year = 0, hour = 0, minute = 0, second = 0;
	std::string text(value);
	if (std::sscanf(text.c_str(), "%3s, %d %3s %d %d:%d:%d %3s", weekday, &day, month, &year, &hour, &minute, &second, zone) != 8)
		return std::nullopt;
	const int monthIndex = MonthIndex(month);
	if (monthIndex < 0 || day < 1 || day > 31 || year < 1970 || hour > 23 || minute > 59 || second > 60 ||
		!EqualsIgnoreCase(zone, "GMT"))
		return std::nullopt;
	const int64_t when = DaysFromCivil(year, static_cast<unsigned>(monthIndex + 1), static_cast<unsigned>(day)) * 86400 +
		hour * 3600 + minute * 60 + second;
	return static_cast<double>(when - nowUnix);
}

std::string EscapeDataString(std::string_view text)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : text)
	{
		if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
			out += static_cast<char>(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

uint32_t LevelPin(int level)
{
	switch (level)
	{
		case 1: return 1017;
		case 2: return 1032;
		case 3: return 1019;
		case 4: return 1005;
		case 5: return 1051;
		case 6: return 1007;
		case 7: return 1020;
		case 8: return 1082;
		case 9: return 1035;
		case 10: return 1060;
		case 11: return 1010;
		default: return 0;
	}
}

bool IsLevelPin(uint32_t pin)
{
	if (pin == 0)
		return false;
	for (int level = 1; level <= 11; ++level)
		if (LevelPin(level) == pin)
			return true;
	return false;
}

}
