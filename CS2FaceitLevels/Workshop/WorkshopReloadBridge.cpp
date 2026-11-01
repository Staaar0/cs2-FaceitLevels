#include "Workshop/WorkshopReloadBridge.h"

#include "Runtime/Common.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cfl {
namespace {

constexpr size_t kMaxBridgeBytes = 2 * 1024 * 1024;

}

std::string ProcessIdentity()
{
#ifdef _WIN32
	FILETIME created{}, exited{}, kernel{}, user{};
	GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
	const unsigned long long start = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
	return std::to_string(GetCurrentProcessId()) + ":" + std::to_string(start);
#else
	std::string stat;
	std::string start = "0";
	if (ReadFile("/proc/self/stat", stat, 64 * 1024))
	{
		const size_t close = stat.rfind(')');
		if (close != std::string::npos)
		{
			auto fields = SplitNonEmpty(std::string_view(stat).substr(close + 1), ' ');
			if (fields.size() > 19)
				start = std::string(fields[19]);
		}
	}
	return std::to_string(getpid()) + ":" + start;
#endif
}

BridgeState TakeBridge(const std::string &path)
{
	BridgeState state;
	std::string text;
	if (!FileExists(path) || !ReadFile(path, text, kMaxBridgeBytes))
		return state;

	nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
	auto process = root.is_object() ? root.find("process") : root.end();
	const bool sameProcess = root.is_object() && process != root.end() && process->is_string() &&
		process->get<std::string>() == ProcessIdentity();
	if (!sameProcess)
	{
		RemoveFile(path);
		return state;
	}

	if (auto it = root.find("runtime_gamedata"); it != root.end() && it->is_string())
		state.runtimeGamedata = it->get<std::string>();
	return state;
}

void SaveBridge(const std::string &path, const BridgeState &state)
{
	if (!state.runtimeGamedata)
	{
		if (FileExists(path))
			RemoveFile(path);
		return;
	}
	nlohmann::json root;
	root["process"] = ProcessIdentity();
	root["runtime_gamedata"] = *state.runtimeGamedata;
	if (!WriteFileAtomic(path, root.dump()))
		LogWarning("Could not write %s.", path.c_str());
}

}
