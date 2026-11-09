#include "Workshop/WorkshopReloadBridge.h"

#include "Runtime/Common.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>

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

#ifndef _WIN32
std::string ReadProcessStartTime()
{
	FILE *file = std::fopen("/proc/self/stat", "rb");
	if (!file)
		return {};
	std::array<char, 64 * 1024> buffer{};
	const size_t size = std::fread(buffer.data(), 1, buffer.size(), file);
	const bool complete = std::feof(file) && !std::ferror(file);
	std::fclose(file);
	if (!complete)
		return {};
	const std::string_view stat(buffer.data(), size);
	const size_t close = stat.rfind(')');
	if (close == std::string_view::npos)
		return {};
	const auto fields = SplitNonEmpty(stat.substr(close + 1), ' ');
	uint64_t start = 0;
	if (fields.size() <= 19 || !ParseUint64(fields[19], start) || start == 0)
		return {};
	return std::to_string(start);
}
#endif

}

std::string ProcessIdentity()
{
#ifdef _WIN32
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
		return {};
	const unsigned long long start = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
	if (start == 0)
		return {};
	return std::to_string(GetCurrentProcessId()) + ":" + std::to_string(start);
#else
	const std::string start = ReadProcessStartTime();
	if (start.empty())
		return {};
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
	const std::string identity = ProcessIdentity();
	const bool sameProcess = !identity.empty() && root.is_object() && process != root.end() && process->is_string() &&
		process->get<std::string>() == identity;
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
	const std::string identity = ProcessIdentity();
	if (!state.runtimeGamedata || identity.empty())
	{
		if (FileExists(path))
			RemoveFile(path);
		return;
	}
	nlohmann::json root;
	root["process"] = identity;
	root["runtime_gamedata"] = *state.runtimeGamedata;
	if (!WriteFileAtomic(path, root.dump()))
		LogWarning("Could not write %s.", path.c_str());
}

}
