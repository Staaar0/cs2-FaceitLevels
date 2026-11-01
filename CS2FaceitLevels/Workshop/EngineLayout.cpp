#include "Workshop/EngineLayout.h"

#include "Runtime/Common.h"
#include "Workshop/AddonHandshake.h"

#include <nlohmann/json.hpp>

#include <climits>

namespace cfl::workshop {
namespace {

constexpr const char *kIncomplete =
	"Incomplete or invalid Workshop gamedata. Replace workshop.gamedata.json together with the plugin.";
constexpr const char *kUnsupported =
	"Unsupported Workshop gamedata layout. Replace workshop.gamedata.json together with the plugin.";

const nlohmann::json &Member(const nlohmann::json &object, const char *key)
{
	auto it = object.find(key);
	if (it == object.end())
		throw WorkshopError(kIncomplete);
	return *it;
}

std::string RequiredString(const nlohmann::json &object, const char *key)
{
	const nlohmann::json &value = Member(object, key);
	if (value.is_null())
		return {};
	if (!value.is_string())
		throw WorkshopError(kIncomplete);
	return value.get<std::string>();
}

int RequiredInt(const nlohmann::json &object, const char *key)
{
	const nlohmann::json &value = Member(object, key);
	if (!value.is_number_integer() && !value.is_number_unsigned())
		throw WorkshopError(kIncomplete);
	if (value.is_number_unsigned() ? value.get<uint64_t>() > static_cast<uint64_t>(INT_MAX)
	                               : (value.get<int64_t>() < INT_MIN || value.get<int64_t>() > INT_MAX))
		throw WorkshopError(kIncomplete);
	return static_cast<int>(value.get<int64_t>());
}

EngineLayout ReadLayout(const nlohmann::json &object)
{
	if (!object.is_object())
		throw WorkshopError(kIncomplete);
	EngineLayout layout;
	layout.replyConnectionSignature = RequiredString(object, "ReplyConnectionSignature");
	layout.clientVTable = RequiredString(object, "ClientVTable");
	layout.sendNetMessageIndex = RequiredInt(object, "SendNetMessageIndex");
	layout.sendServerInfoSignature = RequiredString(object, "SendServerInfoSignature");
	layout.sendServerInfoIndex = RequiredInt(object, "SendServerInfoIndex");
	layout.serverVTable = RequiredString(object, "ServerVTable");
	layout.fillServerInfoIndex = RequiredInt(object, "FillServerInfoIndex");
	layout.serverAddonsOffset = RequiredInt(object, "ServerAddonsOffset");
	layout.hostStateRequestSignature = RequiredString(object, "HostStateRequestSignature");
	layout.hostStateRequestAddonsOffset = RequiredInt(object, "HostStateRequestAddonsOffset");
	layout.clientSlotOffset = RequiredInt(object, "ClientSlotOffset");
	layout.engineInterface = RequiredString(object, "EngineInterface");
	layout.getClientXuidIndex = RequiredInt(object, "GetClientXuidIndex");
	layout.getAppIdIndex = RequiredInt(object, "GetAppIdIndex");
	layout.clientServerOffset = RequiredInt(object, "ClientServerOffset");
	layout.userMessagePayloadOffset = RequiredInt(object, "UserMessagePayloadOffset");
	return layout;
}

bool ValidOffset(int offset)
{
	return offset >= 0 && offset <= 4096 && offset % 4 == 0;
}

bool IsHex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool ValidSignature(std::string_view value)
{
	if (Trim(value).empty())
		return false;
	bool anyByte = false;
	for (std::string_view token : SplitNonEmpty(value, ' '))
	{
		if (token == "?" || token == "??")
			continue;
		if (token.size() != 2 || !IsHex(token[0]) || !IsHex(token[1]))
			return false;
		anyByte = true;
	}
	return anyByte;
}

bool Blank(const std::string &value)
{
	return Trim(value).empty();
}

}

const char *CurrentPlatform()
{
#ifdef _WIN32
	return "windows";
#else
	return "linux";
#endif
}

std::string NormalizeSignature(std::string_view signature)
{
	std::string normalized;
	for (std::string_view token : SplitNonEmpty(signature, ' '))
	{
		if (!normalized.empty())
			normalized += ' ';
		normalized += token;
	}
	return normalized;
}

EngineLayout ParseLayout(std::string_view json, std::string_view platform)
{
	if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF && static_cast<unsigned char>(json[1]) == 0xBB &&
		static_cast<unsigned char>(json[2]) == 0xBF)
		json.remove_prefix(3);

	nlohmann::json root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
	if (root.is_discarded() || !root.is_object())
		throw WorkshopError(kIncomplete);
	const nlohmann::json &platforms = Member(root, "Platforms");
	if (!platforms.is_object())
		throw WorkshopError(platforms.is_null() ? kUnsupported : kIncomplete);

	EngineLayout selected;
	bool found = false;
	for (auto it = platforms.begin(); it != platforms.end(); ++it)
	{
		if (it.value().is_null())
			continue;
		EngineLayout layout = ReadLayout(it.value());
		if (it.key() == platform)
		{
			selected = std::move(layout);
			found = true;
		}
	}

	const EngineLayout &d = selected;
	if ((platform != "linux" && platform != "windows") || !found || !ValidSignature(d.replyConnectionSignature) ||
		!ValidSignature(d.sendServerInfoSignature) || !ValidSignature(d.hostStateRequestSignature) || Blank(d.clientVTable) ||
		d.sendServerInfoIndex < 1 || d.sendServerInfoIndex > 128 || Blank(d.serverVTable) || d.fillServerInfoIndex < 1 ||
		d.fillServerInfoIndex > 128 || d.sendNetMessageIndex < 1 || d.sendNetMessageIndex > 64 ||
		d.userMessagePayloadOffset != 0 || !ValidOffset(d.serverAddonsOffset) || !ValidOffset(d.hostStateRequestAddonsOffset) ||
		d.hostStateRequestAddonsOffset != 88 || !ValidOffset(d.clientSlotOffset) || !ValidOffset(d.clientServerOffset) ||
		d.engineInterface != "Source2EngineToServer001" || d.getClientXuidIndex < 1 || d.getClientXuidIndex > 200 ||
		d.getAppIdIndex < 1 || d.getAppIdIndex > 200)
		throw WorkshopError(kUnsupported);

	selected.platform = std::string(platform);
	selected.sourceJson = std::string(json);
	return selected;
}

}
