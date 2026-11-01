#include "CS2FaceitLevels.h"

#include "CS2FaceitLevels_Config.h"
#include "Commands/EloCommands.h"
#include "Faceit/FaceitClient.h"
#include "Faceit/FaceitParser.h"
#include "Faceit/LevelCache.h"
#include "Game/ChatMessage.h"
#include "Game/PinGuard.h"
#include "Game/PlayerController.h"
#include "Runtime/Common.h"
#include "Runtime/Hooks.h"
#include "Runtime/HttpClient.h"
#include "Runtime/ModuleImage.h"
#include "Runtime/Scheduler.h"
#include "Workshop/SignatureUpdater.h"
#include "Workshop/WorkshopLoader.h"
#include "Workshop/WorkshopReloadBridge.h"

#include <IPluginManager.h>
#include <engine2/igameeventsystem.h>
#include <engine2/igameresourceservice.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>
#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>
#include <schemasystem/schemasystem.h>
#include <tier0/convar.h>
#include <tier0/icvar.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cfl;

namespace {

constexpr std::array<double, 3> kInventoryRetryDelays = {0.5, 1.0, 2.0};

#ifdef _WIN32
constexpr const char *kServerModule = "server.dll";
#else
constexpr const char *kServerModule = "libserver.so";
#endif

void ConsoleSink(const char *line)
{
	if (g_SMAPI)
		g_SMAPI->ConPrint(line);
}

std::string ParentDirectory(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool IsSayCommand(const char *name)
{
	if (!name)
		return false;
	std::string lowered(name);
	for (char &c : lowered)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return lowered == "say" || lowered == "say_team";
}

std::string_view ChatText(const CCommand &args)
{
	std::string_view text = args.ArgS() ? args.ArgS() : "";
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
		text.remove_suffix(1);
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
		text = text.substr(1, text.size() - 2);
	return text;
}

}

class Runtime
{
public:
	Runtime(ISource2Server *server, ISource2GameClients *gameClients, IVEngineServer2 *engine, std::string directory,
		std::string baseDirectory, bool late);
	~Runtime();

	bool Init(std::string &error);
	void EnsureGameFields();
	void AllPluginsLoaded();
	void LevelInit();
	void LevelShutdown();
	void Shutdown();
	void StatusCommand();
	void ReloadCommand();

private:
	struct Session
	{
		bool active = false;
		uint64_t steamId = 0;
		uint64_t generation = 0;
		bool refreshPending = false;
		uint64_t refreshRequest = 0;
		bool inventoryRetryPending = false;
		int inventoryRetryAttempts = 0;
		int level = -1;
	};

	using ConnectHook = VirtualHook<Runtime, ISource2GameClients, bool, CPlayerSlot, const char *, uint64, const char *, bool, CBufferString *>;
	using DisconnectHook = VirtualHook<Runtime, ISource2GameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *>;
	using FullyConnectHook = VirtualHook<Runtime, ISource2GameClients, void, CPlayerSlot>;
	using SteamActivatedHook = VirtualHook<Runtime, ISource2Server, void>;
	using SayHook = VirtualHook<Runtime, ICvar, void, ConCommandRef, const CCommandContext &, const CCommand &>;

	KHook::Return<bool> OnClientConnect(ConnectHook &, ISource2GameClients *, CPlayerSlot slot, const char *, uint64, const char *, bool, CBufferString *);
	KHook::Return<void> OnClientDisconnect(DisconnectHook &, ISource2GameClients *, CPlayerSlot slot, ENetworkDisconnectionReason, const char *, uint64, const char *);
	KHook::Return<void> OnClientFullyConnect(FullyConnectHook &, ISource2GameClients *, CPlayerSlot slot);
	KHook::Return<void> OnSteamActivated(SteamActivatedHook &, ISource2Server *);
	KHook::Return<void> OnDispatchConCommand(SayHook &, ICvar *, ConCommandRef, const CCommandContext &context, const CCommand &args);
	void SyncSayHook();

	void StartWorkshop();
	void TryUpdateSignatures();
	std::vector<EloCommands::Player> HumanPlayers() const;

	Session &GetOrAdd(int slot, uint64_t steamId);
	bool IsCurrent(int slot, uint64_t generation) const;
	CEntityInstance *Resolve(int slot, uint64_t generation) const;
	void Refresh(int slot, double delay);
	void RefreshAll(bool force);
	void RefreshSlot(int slot, uint64_t generation, bool force);
	void ScheduleInventoryRetry(int slot);
	void ApplyCachedPin(int slot, uint64_t steamId);
	void ApplyPin(int slot, CEntityInstance *controller, int level);

	ISource2Server *server_;
	ISource2GameClients *gameClients_;
	IVEngineServer2 *engine_;
	std::string directory_;
	std::string baseDirectory_;
	bool late_;

	Config config_;
	HookKeeper hooks_;
	std::unique_ptr<Scheduler> scheduler_;
	std::unique_ptr<HttpClient> http_;
	std::unique_ptr<LevelCache> cache_;
	std::unique_ptr<FaceitService> faceit_;
	std::unique_ptr<PinGuard> pinGuard_;
	std::unique_ptr<EloCommands> eloCommands_;
	std::unique_ptr<workshop::Loader> workshop_;
	ConnectHook *connectHook_ = nullptr;
	DisconnectHook *disconnectHook_ = nullptr;
	FullyConnectHook *fullyConnectHook_ = nullptr;
	SteamActivatedHook *steamHook_ = nullptr;
	SayHook *sayHook_ = nullptr;

	std::array<Session, 64> sessions_{};
	uint64_t generation_ = 0;
	uint64_t mapGeneration_ = 0;
	bool stopping_ = false;
	bool workshopCanStart_ = false;
	bool signaturesUpdated_ = false;
	bool signatureUpdatePending_ = false;
	bool ownsMamBadge_ = false;
	bool mamAddSent_ = false;
};

namespace {

CS2FaceitLevelsPlugin g_Plugin;
Runtime *g_runtime = nullptr;

}

PLUGIN_EXPOSE(CS2FaceitLevelsPlugin, g_Plugin);

namespace {

void WorkshopStatusCommand(const CCommandContext &context, const CCommand &)
{
	if (context.GetPlayerSlot().Get() != -1)
		return;
	if (g_runtime)
		g_runtime->StatusCommand();
	else
		META_CONPRINT("Workshop loader has not started.\n");
}

void ReloadConfigCommand(const CCommandContext &context, const CCommand &)
{
	if (context.GetPlayerSlot().Get() != -1)
		return;
	if (g_runtime)
		g_runtime->ReloadCommand();
}

void RegisterConsoleCommands()
{
	new ConCommand("faceit_workshop_status", WorkshopStatusCommand,
		"Show the CS2FaceitLevels Workshop loader status (server console).", FCVAR_NONE);
	new ConCommand("faceit_reload", ReloadConfigCommand, "Reload the CS2FaceitLevels config (server console).", FCVAR_NONE);
}

}

bool CS2FaceitLevelsPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	SetLogSink(&ConsoleSink);
	g_detourAwareScan = &KHook::LookupSignature;

	IVEngineServer2 *engine = nullptr;
	ISource2Server *server = nullptr;
	ISource2GameClients *gameClients = nullptr;
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService,
		GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, server, ISource2Server, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, gameClients, ISource2GameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	auto *gameEventSystem = static_cast<IGameEventSystem *>(ismm->VInterfaceMatch(ismm->GetEngineFactory(), GAMEEVENTSYSTEM_INTERFACE_VERSION));
	if (!gameEventSystem)
		LogWarning("%s was not found; !elo and !elos cannot reply in chat.", GAMEEVENTSYSTEM_INTERFACE_VERSION);
	game::SetGameEventSystem(gameEventSystem);

	const std::string baseDirectory = ismm->GetBaseDir();
	std::string directory = JoinPath(baseDirectory, "addons/CS2FaceitLevels");
	auto *manager = static_cast<SourceMM::ISmmPluginManager *>(ismm->MetaFactory(MMIFACE_PLMANAGER, nullptr, nullptr));
	const char *file = nullptr;
	if (manager && manager->Query(id, &file, nullptr, nullptr) && file && *file)
	{
		const std::string bin = ParentDirectory(file);
		if (!bin.empty() && !ParentDirectory(bin).empty())
			directory = ParentDirectory(bin);
	}

	auto *runtime = new Runtime(server, gameClients, engine, directory, baseDirectory, late);
	std::string reason;
	if (!runtime->Init(reason))
	{
		runtime->Shutdown();
		delete runtime;
		ismm->Format(error, maxlen, "%s", reason.c_str());
		return false;
	}
	runtime_ = runtime;
	g_runtime = runtime;
	g_SMAPI->AddListener(this, this);
	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);
	RegisterConsoleCommands();
	Log("CS2FaceitLevels %s loaded%s.", CFL_VERSION, late ? " late" : "");
	return true;
}

bool CS2FaceitLevelsPlugin::Unload(char *, size_t)
{
	g_runtime = nullptr;
	if (runtime_)
	{
		runtime_->Shutdown();
		delete runtime_;
		runtime_ = nullptr;
	}
	SetLogSink(nullptr);
	return true;
}

void CS2FaceitLevelsPlugin::AllPluginsLoaded()
{
	if (runtime_)
		runtime_->AllPluginsLoaded();
}

void CS2FaceitLevelsPlugin::OnLevelInit(char const *, char const *, char const *, char const *, bool, bool)
{
	if (runtime_)
		runtime_->LevelInit();
}

void CS2FaceitLevelsPlugin::OnLevelShutdown()
{
	if (runtime_)
		runtime_->LevelShutdown();
}

Runtime::Runtime(ISource2Server *server, ISource2GameClients *gameClients, IVEngineServer2 *engine, std::string directory,
	std::string baseDirectory, bool late)
	: server_(server), gameClients_(gameClients), engine_(engine), directory_(std::move(directory)),
	  baseDirectory_(std::move(baseDirectory)), late_(late)
{
}

Runtime::~Runtime()
{
	Shutdown();
}

bool Runtime::Init(std::string &error)
{
	std::string schemaError;
	if (!game::Init(schemaError))
		LogWarning("%s; trying again when the map starts.", schemaError.c_str());

	config_ = LoadConfig(JoinPath(directory_, "CS2FaceitLevels.json"),
		JoinPath(baseDirectory_, "addons/counterstrikesharp/configs/plugins/CS2FaceitLevels/CS2FaceitLevels.json"));

	scheduler_ = std::make_unique<Scheduler>(server_, hooks_);
	http_ = std::make_unique<HttpClient>();
	cache_ = std::make_unique<LevelCache>(JoinPath(directory_, "cache.txt"));
	cache_->Load();
	faceit_ = std::make_unique<FaceitService>(*cache_, *http_, *scheduler_, config_);

	BridgeState bridge = TakeBridge(JoinPath(directory_, ".reload-bridge.json"));
	workshop_ = std::make_unique<workshop::Loader>(directory_, *scheduler_, hooks_, engine_, bridge.runtimeGamedata, config_.debug);
	workshopCanStart_ = false;
	if (config_.autoUpdateSignatures)
	{
		signatureUpdatePending_ = true;
		TryUpdateSignatures();
	}

	ModuleImage serverModule;
	std::string moduleError;
	const bool serverOpen = serverModule.Open(kServerModule, moduleError);

	pinGuard_ = std::make_unique<PinGuard>();
	void *controllerVTable = serverOpen ? serverModule.FindVirtualTable("CCSPlayerController") : nullptr;
	if (controllerVTable)
		pinGuard_->Start(controllerVTable);

	connectHook_ = hooks_.Make<ConnectHook>(&ISource2GameClients::ClientConnect, this, &Runtime::OnClientConnect, nullptr);
	connectHook_->AttachInstance(gameClients_);
	disconnectHook_ = hooks_.Make<DisconnectHook>(&ISource2GameClients::ClientDisconnect, this, nullptr, &Runtime::OnClientDisconnect);
	disconnectHook_->AttachInstance(gameClients_);
	fullyConnectHook_ = hooks_.Make<FullyConnectHook>(&ISource2GameClients::ClientFullyConnect, this, nullptr, &Runtime::OnClientFullyConnect);
	fullyConnectHook_->AttachInstance(gameClients_);
	steamHook_ = hooks_.Make<SteamActivatedHook>(&ISource2Server::GameServerSteamAPIActivated, this, nullptr, &Runtime::OnSteamActivated);
	steamHook_->AttachInstance(server_);

	eloCommands_ = std::make_unique<EloCommands>(
		[this] { return HumanPlayers(); },
		[this](uint64_t steamId, EloCommands::LookupDone done) {
			if (!stopping_ && faceit_)
				faceit_->Lookup(steamId, false, std::move(done));
		},
		[](int slot, uint64_t steamId, const std::string &message) {
			if (game::SteamId(game::Controller(slot)) == steamId)
				game::PrintToChat(slot, message);
		});
	SyncSayHook();

	if (late_)
	{
		for (int slot = 0; slot < 64; ++slot)
		{
			CEntityInstance *controller = game::Controller(slot);
			const uint64_t steamId = game::SteamId(controller);
			if (steamId == 0)
				continue;
			GetOrAdd(slot, steamId);
			if (game::IsConnected(controller))
				workshop_->ClientActive(slot, steamId);
		}
		scheduler_->NextFrame([this] {
			for (int slot = 0; slot < 64; ++slot)
			{
				CEntityInstance *controller = game::Controller(slot);
				const uint64_t steamId = game::SteamId(controller);
				if (steamId != 0 && game::IsConnected(controller))
					ApplyCachedPin(slot, steamId);
			}
		});
	}
	const uint64_t map = mapGeneration_;
	const bool force = late_;
	scheduler_->After(2.0, [this, map, force] {
		if (map == mapGeneration_)
			RefreshAll(force);
	});
	return true;
}

void Runtime::AllPluginsLoaded()
{
	workshopCanStart_ = true;
	StartWorkshop();
}

void Runtime::Shutdown()
{
	if (stopping_)
		return;
	stopping_ = true;
	hooks_.DeactivateAll();

	if (faceit_)
		faceit_->Shutdown();
	if (http_)
		http_->CancelAll();

	if (workshop_)
	{
		BridgeState bridge;
		bridge.runtimeGamedata = workshop_->RuntimeGamedata();
		SaveBridge(JoinPath(directory_, ".reload-bridge.json"), bridge);
		workshop_->Dispose();
		if (ownsMamBadge_ && workshop::Loader::MamHasClientBadge() && engine_)
			engine_->ServerCommand("mm_remove_client_addon 3724637448\n");
	}

	if (pinGuard_)
		pinGuard_->Shutdown();
	if (scheduler_)
		scheduler_->Shutdown();
	if (cache_)
		cache_->SaveIfChanged();

	sessions_ = {};
	workshop_.reset();
	(void)pinGuard_.release();
	eloCommands_.reset();
	faceit_.reset();
	cache_.reset();
	http_.reset();
	scheduler_.reset();
}

void Runtime::StatusCommand()
{
	if (!stopping_ && workshop::Loader::OtherLoaderPresent())
		workshop_->Start();
	META_CONPRINTF("%s\n", workshop_ ? workshop_->Summary().c_str() : "Workshop loader has not started.");
}

void Runtime::ReloadCommand()
{
	if (stopping_)
		return;
	const std::string previousKey = config_.faceitApiKey;
	const bool previousAutoUpdate = config_.autoUpdateSignatures;
	if (!ReloadConfig(JoinPath(directory_, "CS2FaceitLevels.json"), config_))
		return;
	if (config_.faceitApiKey != previousKey)
	{
		faceit_->ResetErrors();
		cache_->DropFailures();
	}
	if (config_.autoUpdateSignatures && !previousAutoUpdate)
	{
		signatureUpdatePending_ = true;
		TryUpdateSignatures();
	}
	SyncSayHook();
	Log("Reloaded CS2FaceitLevels.json.");
	RefreshAll(false);
}

void Runtime::EnsureGameFields()
{
	if (game::Ready())
		return;
	std::string error;
	if (!game::Init(error))
		LogError("%s. Scoreboard pins are disabled until this is fixed.", error.c_str());
}

void Runtime::LevelInit()
{
	if (stopping_)
		return;
	EnsureGameFields();
	workshop_->MapStarted();
	scheduler_->NextFrame([this] { StartWorkshop(); });
	++mapGeneration_;
	for (Session &session : sessions_)
	{
		session.refreshPending = false;
		++session.refreshRequest;
		session.inventoryRetryPending = false;
		session.inventoryRetryAttempts = 0;
	}
	if (signatureUpdatePending_)
		TryUpdateSignatures();
	const uint64_t map = mapGeneration_;
	scheduler_->After(2.0, [this, map] {
		if (map == mapGeneration_)
			RefreshAll(false);
	});
}

void Runtime::LevelShutdown()
{
	if (!stopping_)
		cache_->SaveIfChanged();
}

void Runtime::StartWorkshop()
{
	if (stopping_ || !workshopCanStart_ || !workshop_)
		return;
	if (workshop::Loader::OtherLoaderPresent())
	{
		if (!mamAddSent_ && !workshop::Loader::MamHasBadge())
		{
			mamAddSent_ = true;
			ownsMamBadge_ = true;
			engine_->ServerCommand("mm_add_client_addon 3724637448\n");
			scheduler_->After(1.0, [this] {
				if (!stopping_ && workshop::Loader::OtherLoaderPresent())
					workshop_->Start();
			});
		}
		workshop_->Start();
		return;
	}
	const bool updated = signaturesUpdated_;
	signaturesUpdated_ = false;
	if (updated && workshop_->CanRetryStartup())
		workshop_->RetryStartup();
	else
		workshop_->Start();
}

void Runtime::TryUpdateSignatures()
{
	if (stopping_ || !signatureUpdatePending_)
		return;
	const bool started = workshop::UpdateSignatures(*http_, directory_, [this](bool changed) {
		if (stopping_ || !changed)
			return;
		scheduler_->NextFrame([this] {
			if (stopping_)
				return;
			signaturesUpdated_ = true;
			if (workshopCanStart_)
				StartWorkshop();
		});
	});
	if (started)
		signatureUpdatePending_ = false;
}

KHook::Return<void> Runtime::OnClientFullyConnect(FullyConnectHook &, ISource2GameClients *, CPlayerSlot slot)
{
	const int index = slot.Get();
	if (stopping_ || index < 0 || index >= 64)
		return {KHook::Action::Ignore};
	const uint64_t steamId = game::SteamId(game::Controller(index));
	if (steamId != 0)
	{
		workshop_->ClientActive(index, steamId);
		ApplyCachedPin(index, steamId);
	}
	Refresh(index, 2.0);
	return {KHook::Action::Ignore};
}

KHook::Return<void> Runtime::OnSteamActivated(SteamActivatedHook &, ISource2Server *)
{
	if (!stopping_ && signatureUpdatePending_)
		scheduler_->NextFrame([this] { TryUpdateSignatures(); });
	return {KHook::Action::Ignore};
}

std::vector<EloCommands::Player> Runtime::HumanPlayers() const
{
	std::vector<EloCommands::Player> players;
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *controller = game::Controller(slot);
		const uint64_t steamId = game::SteamId(controller);
		if (steamId != 0)
			players.push_back({slot, steamId, game::Name(controller), game::Team(controller)});
	}
	return players;
}

void Runtime::SyncSayHook()
{
	if (!g_pCVar)
		return;
	if (!config_.enableEloCommands)
	{
		if (sayHook_)
			sayHook_->Deactivate();
		return;
	}
	if (sayHook_)
	{
		sayHook_->Activate();
		return;
	}
	sayHook_ = hooks_.Make<SayHook>(&ICvar::DispatchConCommand, this, &Runtime::OnDispatchConCommand, nullptr);
	sayHook_->AttachInstance(g_pCVar);
}

KHook::Return<void> Runtime::OnDispatchConCommand(SayHook &, ICvar *, ConCommandRef, const CCommandContext &context,
	const CCommand &args)
{
	if (stopping_ || !eloCommands_ || !config_.enableEloCommands || args.ArgC() < 2)
		return {KHook::Action::Ignore};
	const int slot = context.GetPlayerSlot().Get();
	if (slot < 0 || slot >= 64 || !IsSayCommand(args.Arg(0)))
		return {KHook::Action::Ignore};
	const uint64_t steamId = game::SteamId(game::Controller(slot));
	bool silent = false;
	if (!eloCommands_->OnSay(slot, steamId, ChatText(args), silent) || !silent)
		return {KHook::Action::Ignore};
	return {KHook::Action::Supersede};
}

KHook::Return<bool> Runtime::OnClientConnect(ConnectHook &, ISource2GameClients *, CPlayerSlot slot, const char *, uint64,
	const char *, bool, CBufferString *)
{
	if (!stopping_)
		workshop_->ClientConnect(slot.Get());
	return {KHook::Action::Ignore};
}

KHook::Return<void> Runtime::OnClientDisconnect(DisconnectHook &, ISource2GameClients *, CPlayerSlot slot,
	ENetworkDisconnectionReason, const char *, uint64, const char *)
{
	if (stopping_)
		return {KHook::Action::Ignore};
	workshop_->ClientDisconnect(slot.Get());
	if (slot.Get() >= 0 && slot.Get() < 64)
	{
		sessions_[slot.Get()] = Session{};
		pinGuard_->Forget(slot.Get());
		if (eloCommands_)
			eloCommands_->Forget(slot.Get());
	}
	return {KHook::Action::Ignore};
}

Runtime::Session &Runtime::GetOrAdd(int slot, uint64_t steamId)
{
	Session &session = sessions_[slot];
	if (session.active && session.steamId == steamId)
		return session;
	session = Session{};
	session.active = true;
	session.steamId = steamId;
	session.generation = ++generation_;
	pinGuard_->Forget(slot);
	return session;
}

bool Runtime::IsCurrent(int slot, uint64_t generation) const
{
	return slot >= 0 && slot < 64 && sessions_[slot].active && sessions_[slot].generation == generation;
}

CEntityInstance *Runtime::Resolve(int slot, uint64_t generation) const
{
	if (!IsCurrent(slot, generation))
		return nullptr;
	CEntityInstance *controller = game::Controller(slot);
	const uint64_t steamId = game::SteamId(controller);
	return steamId != 0 && steamId == sessions_[slot].steamId ? controller : nullptr;
}

void Runtime::Refresh(int slot, double delay)
{
	if (stopping_)
		return;
	const uint64_t steamId = game::SteamId(game::Controller(slot));
	if (steamId == 0)
		return;
	Session &session = GetOrAdd(slot, steamId);
	session.inventoryRetryAttempts = 0;
	const uint64_t generation = session.generation;
	const uint64_t map = mapGeneration_;
	scheduler_->After(delay, [this, slot, generation, map] {
		if (map == mapGeneration_)
			RefreshSlot(slot, generation, false);
	});
}

void Runtime::RefreshAll(bool force)
{
	if (stopping_)
		return;
	for (int slot = 0; slot < 64; ++slot)
	{
		const uint64_t steamId = game::SteamId(game::Controller(slot));
		if (steamId != 0)
			RefreshSlot(slot, GetOrAdd(slot, steamId).generation, force);
	}
}

void Runtime::RefreshSlot(int slot, uint64_t generation, bool force)
{
	if (stopping_)
		return;
	CEntityInstance *controller = Resolve(slot, generation);
	if (!controller)
		return;
	Session &session = sessions_[slot];
	if (!game::InventoryReady(controller))
		ScheduleInventoryRetry(slot);
	else
	{
		session.inventoryRetryPending = false;
		session.inventoryRetryAttempts = 0;
	}

	LevelEntry cached;
	if (!force && cache_->TryGetFresh(session.steamId, cached))
	{
		ApplyPin(slot, controller, cached.level);
		return;
	}
	if (session.refreshPending)
		return;
	session.refreshPending = true;
	const uint64_t request = ++session.refreshRequest;
	const uint64_t map = mapGeneration_;
	faceit_->Lookup(session.steamId, force, [this, slot, generation, request, map](int level, int) {
		if (stopping_ || map != mapGeneration_ || !IsCurrent(slot, generation) || request != sessions_[slot].refreshRequest)
			return;
		sessions_[slot].refreshPending = false;
		if (CEntityInstance *current = Resolve(slot, generation))
		{
			ApplyPin(slot, current, level);
			if (level >= 0 && !game::InventoryReady(current))
				ScheduleInventoryRetry(slot);
		}
	});
}

void Runtime::ScheduleInventoryRetry(int slot)
{
	Session &session = sessions_[slot];
	if (session.inventoryRetryPending || session.inventoryRetryAttempts >= static_cast<int>(kInventoryRetryDelays.size()))
		return;
	const double delay = kInventoryRetryDelays[session.inventoryRetryAttempts++];
	const uint64_t map = mapGeneration_;
	const uint64_t generation = session.generation;
	session.inventoryRetryPending = true;
	scheduler_->After(delay, [this, slot, generation, map] {
		if (stopping_ || map != mapGeneration_ || !IsCurrent(slot, generation) || !sessions_[slot].inventoryRetryPending)
			return;
		sessions_[slot].inventoryRetryPending = false;
		RefreshSlot(slot, generation, false);
	});
}

void Runtime::ApplyCachedPin(int slot, uint64_t steamId)
{
	if (stopping_ || slot < 0 || slot >= 64)
		return;
	CEntityInstance *controller = game::Controller(slot);
	if (!controller || game::SteamId(controller) != steamId)
		return;
	GetOrAdd(slot, steamId);
	LevelEntry cached;
	ApplyPin(slot, controller, cache_->TryGetFresh(steamId, cached) ? cached.level : -1);
}

void Runtime::ApplyPin(int slot, CEntityInstance *controller, int level)
{
	Session &session = sessions_[slot];
	if (level >= 0)
		session.level = level;
	if (!pinGuard_->Active())
		pinGuard_->Start(*reinterpret_cast<void **>(controller));
	const uint32_t levelPin = LevelPin(session.level);
	bool changed = false;
	if (levelPin != 0 || (session.level == 0 && config_.clearPinWhenNoFaceit))
	{
		pinGuard_->Want(slot, session.steamId, levelPin);
		if (game::SetCoin(controller, levelPin, changed) && config_.debug)
			Log("Updated scoreboard pin for %s (level %d).", game::Name(controller), session.level);
		return;
	}
	pinGuard_->Block(slot, session.steamId);
	uint32_t current = 0;
	if (game::GetCoin(controller, current) && IsLevelPin(current) && game::SetCoin(controller, 0, changed) && config_.debug)
		Log("Removed a FACEIT level pin from %s (no FACEIT level).", game::Name(controller));
}
