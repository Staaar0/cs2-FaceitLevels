#include "CS2FaceitLevels_Config.h"

#include "Runtime/Common.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace cfl {
namespace {

constexpr size_t kMaxConfigBytes = 1024 * 1024;

size_t SkipComment(std::string_view text, size_t i)
{
	if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/')
	{
		size_t end = text.find('\n', i);
		return end == std::string_view::npos ? text.size() : end;
	}
	if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*')
	{
		size_t end = text.find("*/", i + 2);
		return end == std::string_view::npos ? text.size() : end + 2;
	}
	return i;
}

char NextSignificant(std::string_view text, size_t i)
{
	while (i < text.size())
	{
		const char c = text[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
		{
			++i;
			continue;
		}
		size_t skipped = SkipComment(text, i);
		if (skipped != i)
		{
			i = skipped;
			continue;
		}
		return c;
	}
	return '\0';
}

bool ReadInt(const nlohmann::json &value, int &out)
{
	if (value.is_number_integer() || value.is_number_unsigned())
	{
		const auto number = value.get<long long>();
		out = static_cast<int>(std::clamp<long long>(number, INT32_MIN, INT32_MAX));
		return true;
	}
	if (value.is_number_float())
	{
		const double number = value.get<double>();
		if (!std::isfinite(number) || std::floor(number) != number)
			return false;
		out = static_cast<int>(std::clamp(number, static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX)));
		return true;
	}
	return false;
}

template <typename Apply>
void Field(const nlohmann::json &root, const char *key, const char *type, Apply apply)
{
	auto it = root.find(key);
	if (it == root.end())
		return;
	if (!apply(*it))
		LogWarning("Config key \"%s\" must be %s; using its default value.", key, type);
}

}

bool Config::HasApiKey() const
{
	std::string_view key = Trim(faceitApiKey);
	return !key.empty() && key != kDefaultApiKey;
}

void Config::Clamp()
{
	cacheMinutes = std::clamp(cacheMinutes, 1, 1440);
	requestTimeoutSeconds = std::clamp(requestTimeoutSeconds, 2, 60);
	faceitApiKey = std::string(Trim(faceitApiKey));
}

std::string StripJsonExtras(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
		static_cast<unsigned char>(text[2]) == 0xBF)
		i = 3;
	while (i < text.size())
	{
		const char c = text[i];
		if (c == '"')
		{
			out += c;
			++i;
			while (i < text.size())
			{
				const char s = text[i++];
				out += s;
				if (s == '\\' && i < text.size())
					out += text[i++];
				else if (s == '"')
					break;
			}
			continue;
		}
		size_t skipped = SkipComment(text, i);
		if (skipped != i)
		{
			i = skipped;
			continue;
		}
		if (c == ',')
		{
			const char next = NextSignificant(text, i + 1);
			if (next == '}' || next == ']')
			{
				++i;
				continue;
			}
		}
		out += c;
		++i;
	}
	return out;
}

bool ParseConfig(std::string_view text, Config &config, bool &hadVersion, std::string &error)
{
	hadVersion = false;
	nlohmann::json root = nlohmann::json::parse(StripJsonExtras(text), nullptr, false);
	if (root.is_discarded() || !root.is_object())
	{
		error = "the file is not a valid JSON object";
		return false;
	}

	Field(root, "faceit_api_key", "a string", [&](const nlohmann::json &v) {
		if (!v.is_string())
			return false;
		config.faceitApiKey = v.get<std::string>();
		return true;
	});
	Field(root, "debug", "true or false", [&](const nlohmann::json &v) {
		if (!v.is_boolean())
			return false;
		config.debug = v.get<bool>();
		return true;
	});
	Field(root, "cache_minutes", "a whole number", [&](const nlohmann::json &v) { return ReadInt(v, config.cacheMinutes); });
	Field(root, "request_timeout_seconds", "a whole number",
		[&](const nlohmann::json &v) { return ReadInt(v, config.requestTimeoutSeconds); });
	Field(root, "clear_pin_when_no_faceit", "true or false", [&](const nlohmann::json &v) {
		if (!v.is_boolean())
			return false;
		config.clearPinWhenNoFaceit = v.get<bool>();
		return true;
	});
	Field(root, "enable_elo_commands", "true or false", [&](const nlohmann::json &v) {
		if (!v.is_boolean())
			return false;
		config.enableEloCommands = v.get<bool>();
		return true;
	});
	Field(root, "AutoUpdateSignatures", "true or false", [&](const nlohmann::json &v) {
		if (!v.is_boolean())
			return false;
		config.autoUpdateSignatures = v.get<bool>();
		return true;
	});

	config.version = 0;
	if (auto it = root.find("ConfigVersion"); it != root.end())
		hadVersion = ReadInt(*it, config.version);
	return true;
}

std::string SerializeConfig(const Config &config)
{
	nlohmann::ordered_json root;
	root["faceit_api_key"] = config.faceitApiKey;
	root["debug"] = config.debug;
	root["cache_minutes"] = config.cacheMinutes;
	root["request_timeout_seconds"] = config.requestTimeoutSeconds;
	root["clear_pin_when_no_faceit"] = config.clearPinWhenNoFaceit;
	root["enable_elo_commands"] = config.enableEloCommands;
	root["AutoUpdateSignatures"] = config.autoUpdateSignatures;
	root["ConfigVersion"] = Config::kCurrentVersion;
	return root.dump(2) + "\n";
}

Config LoadConfig(const std::string &path, const std::string &legacyPath)
{
	Config config;
	std::string text;
	std::string error;
	bool hadVersion = false;

	if (FileExists(path))
	{
		if (!ReadFile(path, text, kMaxConfigBytes))
		{
			LogError("Could not read %s; using the default settings.", path.c_str());
			Config defaults;
			defaults.Clamp();
			return defaults;
		}
		if (!ParseConfig(text, config, hadVersion, error))
		{
			LogError("Could not parse %s (%s); using the default settings until it is fixed.", path.c_str(), error.c_str());
			Config defaults;
			defaults.Clamp();
			return defaults;
		}
		if (config.version < Config::kCurrentVersion)
		{
			if (WriteFileAtomic(path, SerializeConfig(config)))
				Log("Updated %s to ConfigVersion %d.", path.c_str(), Config::kCurrentVersion);
			else
				LogWarning("Could not update %s to ConfigVersion %d.", path.c_str(), Config::kCurrentVersion);
		}
	}
	else
	{
		if (!legacyPath.empty() && ReadFile(legacyPath, text, kMaxConfigBytes) && ParseConfig(text, config, hadVersion, error))
			Log("Imported the settings of %s.", legacyPath.c_str());
		else
			config = Config();
		if (WriteFileAtomic(path, SerializeConfig(config)))
			Log("Created %s.", path.c_str());
		else
			LogWarning("Could not create %s.", path.c_str());
	}

	config.version = Config::kCurrentVersion;
	config.Clamp();
	return config;
}

bool ReloadConfig(const std::string &path, Config &config)
{
	std::string text;
	std::string error;
	bool hadVersion = false;
	Config loaded;
	if (!ReadFile(path, text, kMaxConfigBytes))
	{
		LogError("Could not read %s; keeping the current settings.", path.c_str());
		return false;
	}
	if (!ParseConfig(text, loaded, hadVersion, error))
	{
		LogError("Could not parse %s (%s); keeping the current settings.", path.c_str(), error.c_str());
		return false;
	}
	if (loaded.version < Config::kCurrentVersion && !WriteFileAtomic(path, SerializeConfig(loaded)))
		LogWarning("Could not update %s to ConfigVersion %d.", path.c_str(), Config::kCurrentVersion);
	loaded.version = Config::kCurrentVersion;
	loaded.Clamp();
	config = loaded;
	return true;
}

}
