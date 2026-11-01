#pragma once

#include <optional>
#include <string>

namespace cfl {

struct BridgeState
{
	std::optional<std::string> runtimeGamedata;
};

std::string ProcessIdentity();

BridgeState TakeBridge(const std::string &path);
void SaveBridge(const std::string &path, const BridgeState &state);

}
