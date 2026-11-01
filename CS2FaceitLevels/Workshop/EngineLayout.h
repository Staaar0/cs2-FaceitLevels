#pragma once

#include <string>
#include <string_view>

namespace cfl::workshop {

struct EngineLayout
{
	std::string platform;
	std::string replyConnectionSignature;
	std::string clientVTable;
	int sendNetMessageIndex = 0;
	std::string sendServerInfoSignature;
	int sendServerInfoIndex = 0;
	std::string serverVTable;
	int fillServerInfoIndex = 0;
	int serverAddonsOffset = 0;
	std::string hostStateRequestSignature;
	int hostStateRequestAddonsOffset = 0;
	int clientSlotOffset = 0;
	std::string engineInterface;
	int getClientXuidIndex = 0;
	int getAppIdIndex = 0;
	int clientServerOffset = 0;
	int userMessagePayloadOffset = 0;
	std::string sourceJson;
};

const char *CurrentPlatform();

EngineLayout ParseLayout(std::string_view json, std::string_view platform);

std::string NormalizeSignature(std::string_view signature);

}
