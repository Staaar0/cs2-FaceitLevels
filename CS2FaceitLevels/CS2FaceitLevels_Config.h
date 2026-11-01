#pragma once

#include <string>
#include <string_view>

namespace cfl {

struct Config
{
	static constexpr int kCurrentVersion = 2;
	static constexpr const char *kDefaultApiKey = "PUT_YOUR_FACEIT_API_KEY_HERE";

	std::string faceitApiKey = kDefaultApiKey;
	bool debug = false;
	int cacheMinutes = 30;
	int requestTimeoutSeconds = 10;
	bool clearPinWhenNoFaceit = false;
	bool enableEloCommands = true;
	bool autoUpdateSignatures = true;
	int version = kCurrentVersion;

	bool HasApiKey() const;
	void Clamp();
};

Config LoadConfig(const std::string &path, const std::string &legacyPath);
bool ReloadConfig(const std::string &path, Config &config);

std::string SerializeConfig(const Config &config);
bool ParseConfig(std::string_view text, Config &config, bool &hadVersion, std::string &error);
std::string StripJsonExtras(std::string_view text);

}
