#include "Commands/ChatFormatter.h"

#include <cstdint>

namespace cfl::chat {
namespace {

constexpr char kDefault = '\x01';
constexpr char kLightPurple = '\x03';
constexpr char kLime = '\x06';
constexpr char kRed = '\x07';
constexpr char kGrey = '\x08';
constexpr char kYellow = '\x09';
constexpr char kOrange = '\x10';

constexpr uint32_t kInvalid = 0xFFFFFFFF;

uint32_t Decode(std::string_view text, size_t &i)
{
	const unsigned char lead = static_cast<unsigned char>(text[i]);
	if (lead < 0x80)
	{
		++i;
		return lead;
	}
	size_t length = 0;
	uint32_t value = 0;
	if ((lead & 0xE0) == 0xC0)
	{
		length = 2;
		value = lead & 0x1F;
	}
	else if ((lead & 0xF0) == 0xE0)
	{
		length = 3;
		value = lead & 0x0F;
	}
	else if ((lead & 0xF8) == 0xF0)
	{
		length = 4;
		value = lead & 0x07;
	}
	if (length == 0 || i + length > text.size())
	{
		++i;
		return kInvalid;
	}
	for (size_t k = 1; k < length; ++k)
	{
		const unsigned char next = static_cast<unsigned char>(text[i + k]);
		if ((next & 0xC0) != 0x80)
		{
			++i;
			return kInvalid;
		}
		value = (value << 6) | (next & 0x3F);
	}
	i += length;
	return value;
}

void Encode(uint32_t value, std::string &out)
{
	if (value < 0x80)
		out += static_cast<char>(value);
	else if (value < 0x800)
	{
		out += static_cast<char>(0xC0 | (value >> 6));
		out += static_cast<char>(0x80 | (value & 0x3F));
	}
	else if (value < 0x10000)
	{
		out += static_cast<char>(0xE0 | (value >> 12));
		out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (value & 0x3F));
	}
	else
	{
		out += static_cast<char>(0xF0 | (value >> 18));
		out += static_cast<char>(0x80 | ((value >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (value & 0x3F));
	}
}

uint32_t Fold(uint32_t c)
{
	if (c < 0x80)
		return c >= 'A' && c <= 'Z' ? c + 0x20 : c;
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
		return c + 0x20;
	if (c >= 0x100 && c <= 0x17F)
	{
		if (c == 0x130 || c == 0x131)
			return 'i';
		if (c == 0x178)
			return 0xFF;
		if (c == 0x17F)
			return 's';
		if (c == 0x138 || c == 0x149)
			return c;
		if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
			return (c & 1) ? c + 1 : c;
		return (c & 1) ? c : c + 1;
	}
	if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
		return c + 0x20;
	if (c == 0x386)
		return 0x3AC;
	if (c >= 0x388 && c <= 0x38A)
		return c + 0x25;
	if (c == 0x38C)
		return 0x3CC;
	if (c == 0x38E || c == 0x38F)
		return c + 0x3F;
	if (c == 0x3C2)
		return 0x3C3;
	if (c >= 0x400 && c <= 0x40F)
		return c + 0x50;
	if (c >= 0x410 && c <= 0x42F)
		return c + 0x20;
	if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
		return (c & 1) ? c : c + 1;
	if (c == 0x4C0)
		return 0x4CF;
	if (c >= 0x4C1 && c <= 0x4CE)
		return (c & 1) ? c + 1 : c;
	if (c >= 0x531 && c <= 0x556)
		return c + 0x30;
	if (c >= 0xFF21 && c <= 0xFF3A)
		return c + 0x20;
	return c;
}

bool IsControl(uint32_t c)
{
	return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029;
}

std::string Prefix()
{
	return std::string("[") + kRed + "CS2FaceitLevels" + kDefault + "]";
}

char EloColor(int skillLevel)
{
	switch (skillLevel)
	{
	case 2:
	case 3:
		return kLime;
	case 4:
	case 5:
	case 6:
	case 7:
		return kYellow;
	case 8:
	case 9:
		return kOrange;
	case 10:
		return kRed;
	default:
		return kGrey;
	}
}

}

std::string MissingName()
{
	return Prefix() + " You need player name " + kRed + "< !elo {player name} >";
}

std::string NoMatch(std::string_view search)
{
	return Prefix() + " No player found matching " + kRed + Literal(search);
}

std::string Multiple(std::string_view names)
{
	return Prefix() + " Multiple players found: " + kRed + Literal(names);
}

std::string EloLine(std::string_view playerName, int level, int elo)
{
	const int skillLevel = level <= 0 ? 0 : (level > 10 ? 10 : level);
	const std::string eloText = elo >= 0 ? std::to_string(elo) : "N/A";
	return std::string("- ") + kRed + Literal(playerName) + kDefault + " " + kLightPurple + "Elo" + kDefault + ": " +
		EloColor(skillLevel) + eloText + kDefault;
}

std::string Literal(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size())
	{
		const size_t start = i;
		const uint32_t c = Decode(text, i);
		if (c != kInvalid && IsControl(c))
			continue;
		out.append(text.substr(start, i - start));
	}
	return out;
}

std::string FoldCase(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size())
	{
		const size_t start = i;
		const uint32_t c = Decode(text, i);
		if (c == kInvalid)
			out.append(text.substr(start, i - start));
		else
			Encode(Fold(c), out);
	}
	return out;
}

}
