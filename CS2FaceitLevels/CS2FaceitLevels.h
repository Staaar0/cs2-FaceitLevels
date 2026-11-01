#pragma once

#include <ISmmPlugin.h>

#define CFL_VERSION "1.1.4"

class Runtime;

class CS2FaceitLevelsPlugin final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlength, bool late) override;
	bool Unload(char *error, size_t maxlen) override;
	void AllPluginsLoaded() override;

	void OnLevelInit(char const *mapName, char const *mapEntities, char const *oldLevel, char const *landmarkName,
		bool loadGame, bool background) override;
	void OnLevelShutdown() override;

	const char *GetAuthor() override { return "✪ Stαr"; }
	const char *GetName() override { return "CS2FaceitLevels"; }
	const char *GetDescription() override { return "Shows real FACEIT levels in the CS2 scoreboard."; }
	const char *GetURL() override { return "https://github.com/Staaar0/cs2-FaceitLevels"; }
	const char *GetLicense() override { return "GPL-3.0"; }
	const char *GetVersion() override { return CFL_VERSION; }
	const char *GetDate() override { return __DATE__; }
	const char *GetLogTag() override { return "CS2FaceitLevels"; }

private:
	Runtime *runtime_ = nullptr;
};

PLUGIN_GLOBALVARS();
