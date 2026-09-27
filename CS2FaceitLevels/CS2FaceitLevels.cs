using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes;
using CounterStrikeSharp.API.Modules.Timers;
using Microsoft.Extensions.Logging;
using CS2FaceitLevels.Workshop;

namespace CS2FaceitLevels;

[MinimumApiVersion(371)]
public sealed class CS2FaceitLevels : BasePlugin, IPluginConfig<CS2FaceitLevelsConfig>
{
    public override string ModuleName => "CS2FaceitLevels";
    public override string ModuleAuthor => "✪ Stαr";
    public override string ModuleVersion => "1.1.0";
    public override string ModuleDescription => "Shows real FACEIT levels in the CS2 scoreboard.";

    private readonly PlayerSessions _sessions = new();
    private BackgroundWork _work = null!;
    private FaceitCache _cache = null!;
    private FaceitLookup _lookup = null!;
    private PinEnforcer _pins = null!;
    private EloCommands _commands = null!;
    private ChatFormatter _chat = new(new CS2FaceitLevelsLang());
    private bool _reloadOnFirstConnect;
    private long _mapGeneration;
    private bool _unloading;
    private static readonly float[] InventoryRetryDelays = [0.5f, 1f, 2f];
    private WorkshopLoader? _workshop;
    private bool _ownsMamBadge;
    private bool _mamRegistrationPending;

    public CS2FaceitLevelsConfig Config { get; set; } = new();

    public void OnConfigParsed(CS2FaceitLevelsConfig config)
    {
        config.CacheMinutes = Math.Clamp(config.CacheMinutes, 1, 1440);
        config.RequestTimeoutSeconds = Math.Clamp(config.RequestTimeoutSeconds, 2, 60);
        if (string.IsNullOrWhiteSpace(config.Language)) config.Language = "en";
        Config = config;
        _chat = new ChatFormatter(LanguageReader.Load(ModuleDirectory, config.Language, Logger));
    }

    public override void Load(bool hotReload)
    {
        _work = new BackgroundWork(() => Config.Debug, Logger);
        _cache = new FaceitCache(Path.Combine(ModuleDirectory, "cache.json"), _work.LifetimeToken,
            () => Config.Debug, Logger);
        _lookup = new FaceitLookup(new FaceitClient(() => Config, Logger), _cache, _work.LifetimeToken,
            () => Config.Debug, Logger);
        _pins = new PinEnforcer(_sessions, () => Config, Logger);
        _commands = new EloCommands(_sessions, _lookup, _work, () => _chat, () => Config.Debug, Logger);
        var workshopReloadState = WorkshopReloadBridge.Take(ModuleDirectory, hotReload);
        _ownsMamBadge = WorkshopReloadBridge.TakeMamOwnership(ModuleDirectory, hotReload);
        _workshop = new WorkshopLoader(ModuleDirectory, Logger, () => Config.Debug);
        _workshop.RestoreReload(workshopReloadState);

        RegisterEventHandler<EventPlayerConnectFull>(OnPlayerConnectFull);
        RegisterEventHandler<EventPlayerSpawn>(OnPlayerSpawn);
        RegisterEventHandler<EventPlayerDisconnect>(OnPlayerDisconnect);
        RegisterListener<Listeners.OnTick>(EnforcePins);
        RegisterListener<Listeners.OnClientPutInServer>(OnClientPutInServer);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        RegisterListener<Listeners.OnMapStart>(OnMapStart);
        RegisterListener<Listeners.OnClientConnect>((slot, name, ip) =>
        {
            _workshop?.ClientConnect(slot);
        });
        AddCommand("css_faceit_workshop_status", "Show the Workshop loader status (server console).", (player, command) =>
        {
            if (player == null) command.ReplyToCommand(_workshop?.Summary ?? "Workshop loader has not started.");
        });

        if (hotReload)
        {
            foreach (var player in Utilities.GetPlayers())
                if (PlayerAccess.TryIdentity(player, out var steamId))
                {
                    _sessions.GetOrAdd(player.Slot, steamId).EnforcePin = true;
                    if (PlayerAccess.TryIdentity(player, out _, connected: true))
                        _workshop?.ClientActive(player.Slot, steamId);
                }
        }
        // Preserve the original first-player reload required by pin protection.
        // Workshop handshakes are handed to the new instance during hot reload.
        _reloadOnFirstConnect = !hotReload;
        AddTimer(60f, _cache.RequestMaintenance, TimerFlags.REPEAT);
        if (_workshop != null)
        {
            AddTimer(30f, _workshop.Maintenance, TimerFlags.REPEAT);
            AddTimer(1f, _workshop.ReleaseIdleSendHook, TimerFlags.REPEAT);
        }
        if (Config.EnableEloCommands)
        {
            AddCommand("css_elo", "Show a player's FACEIT elo.", _commands.Single);
            AddCommand("css_elos", "Show every player's FACEIT elo.", _commands.All);
            // Compile the two chat command paths off-thread, without accessing players
            // or sending requests to FACEIT during startup or the first-player reload.
            _work.Run(() =>
            {
                _commands.Prewarm(_chat);
                return Task.CompletedTask;
            });
        }
        AddTimer(2f, () => RefreshAll(force: hotReload), TimerFlags.STOP_ON_MAPCHANGE);
        // On hot reload the network message system is already initialized. Reattach
        // before returning from Load so concurrent connections have no frame-long gap.
        if (hotReload) StartWorkshop();
    }

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        // The first connection can arrive before a queued world update runs.
        // Attach the loader as soon as CSS has finished loading plugins.
        StartWorkshop();
    }

    private void StartWorkshop()
    {
        if (_unloading || _workshop == null) return;
        if (WorkshopLoader.OtherLoaderPresent() && !WorkshopLoader.MamHasBadge())
        {
            if (_mamRegistrationPending) return;
            // Let MAM own the complete download and reconnect sequence when it is
            // installed. This changes MAM's in-memory client list, not its cfg file.
            _mamRegistrationPending = true;
            Server.ExecuteCommand($"mm_add_client_addon {WorkshopLoader.AddonId}");
            ConfirmMamRegistration(0);
            return;
        }
        _workshop.Start();
    }

    private void ConfirmMamRegistration(int attempt)
    {
        if (_unloading || !_mamRegistrationPending) return;
        if (WorkshopLoader.MamHasClientBadge())
        {
            _ownsMamBadge = true;
            _mamRegistrationPending = false;
            StartWorkshop();
        }
        else if (attempt < 4)
        {
            // Server commands may run after the current plugin callback. A pre-world
            // update also runs on hibernating servers before their first player joins.
            Server.NextWorldUpdate(() => ConfirmMamRegistration(attempt + 1));
        }
        else
        {
            _mamRegistrationPending = false;
            Logger.LogError("[CS2FaceitLevels] MultiAddonManager did not register the badge addon. Check that mm_add_client_addon is available.");
        }
    }

    public override void Unload(bool hotReload)
    {
        if (_unloading) return;
        _unloading = true;
        _workshop?.PreserveReload(hotReload);
        _workshop?.Dispose();
        WorkshopReloadBridge.SaveMamOwnership(ModuleDirectory, hotReload && _ownsMamBadge);
        if (!hotReload && _ownsMamBadge && WorkshopLoader.MamHasClientBadge())
            Server.ExecuteCommand($"mm_remove_client_addon {WorkshopLoader.AddonId}");
        _sessions.Clear();
        var pending = _work.Stop();
        var shutdown = Task.Run(() => _cache.Stop(pending, TimeSpan.FromSeconds(2.5)));
        _work.DisposeAfter(Task.WhenAll(shutdown, pending, _cache.Ready));
        try { shutdown.WaitAsync(TimeSpan.FromSeconds(3)).GetAwaiter().GetResult(); }
        catch (TimeoutException)
        {
            Logger.LogWarning("[CS2FaceitLevels] Background shutdown is completing after the unload deadline.");
        }
        catch (Exception ex)
        {
            Logger.LogWarning(ex, "[CS2FaceitLevels] Background shutdown failed.");
        }
        // Observe late completion too; no task in this drain depends on a frame callback.
        _ = shutdown.ContinueWith(task =>
        {
            if (task.IsFaulted && Config.Debug)
                Logger.LogWarning(task.Exception, "[CS2FaceitLevels] Background shutdown failed.");
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    private bool Stopping => _unloading || _work.Stopping;

    private void OnMapStart(string mapName)
    {
        _workshop?.MapStarted();
        Server.NextWorldUpdate(StartWorkshop);
        _mapGeneration++;
        foreach (var session in _sessions.Active)
        {
            session.RefreshPending = false;
            session.RefreshRequest++;
            session.InventoryRetryPending = false;
            session.InventoryRetryAttempts = 0;
        }
        AddTimer(2f, () => RefreshAll(force: false), TimerFlags.STOP_ON_MAPCHANGE);
    }

    private HookResult OnPlayerConnectFull(EventPlayerConnectFull e, GameEventInfo info)
    {
        if (PlayerAccess.TryIdentity(e.Userid, out var linkedSteamId))
            _workshop?.ClientActive(e.Userid.Slot, linkedSteamId);
        if (_reloadOnFirstConnect && PlayerAccess.TryIdentity(e.Userid, out _))
        {
            _reloadOnFirstConnect = false;
            Server.NextFrame(() =>
            {
                if (!Stopping) Server.ExecuteCommand("css_plugins reload CS2FaceitLevels");
            });
        }
        return Refresh(e.Userid, 2f);
    }

    private HookResult OnPlayerSpawn(EventPlayerSpawn e, GameEventInfo info) => Refresh(e.Userid, 0.2f);

    private void EnforcePins()
    {
        if (!Stopping) _pins.Enforce();
    }

    private void OnClientPutInServer(int slot)
    {
        var version = _sessions.ReservePut(slot);
        Server.NextFrame(() =>
        {
            if (Stopping || !_sessions.IsPendingPut(slot, version)) return;
            var player = Utilities.GetPlayerFromSlot(slot);
            // The player may not report "connected" on this frame yet. Arm protection anyway:
            // PinEnforcer itself waits until the player is connected with an inventory.
            if (PlayerAccess.TryIdentity(player, out var steamId))
                _sessions.GetOrAdd(slot, steamId).EnforcePin = true;
        });
    }

    private void OnClientDisconnect(int slot)
    {
        _workshop?.ClientDisconnect(slot);
        _sessions.Remove(slot);
    }

    private HookResult OnPlayerDisconnect(EventPlayerDisconnect e, GameEventInfo info)
    {
        if (PlayerAccess.TryIdentity(e.Userid, out var steamId)) _sessions.Remove(e.Userid.Slot, steamId);
        return HookResult.Continue;
    }

    private HookResult Refresh(CCSPlayerController? player, float delay)
    {
        if (!Stopping && PlayerAccess.TryIdentity(player, out var steamId))
        {
            var session = _sessions.GetOrAdd(player.Slot, steamId);
            // In-game events re-arm pin protection, so it never depends on the timing of
            // OnClientPutInServer or the first-player reload (Workshop downloads, reconnects).
            session.EnforcePin = true;
            session.InventoryRetryAttempts = 0;
            var map = _mapGeneration;
            // Preserve the short spawn/connect delay; missing inventories get bounded retries.
            AddTimer(delay, () =>
            {
                if (map == _mapGeneration) RefreshSlot(session, force: false);
            }, TimerFlags.STOP_ON_MAPCHANGE);
        }
        return HookResult.Continue;
    }

    private void RefreshAll(bool force)
    {
        if (Stopping) return;
        foreach (var player in Utilities.GetPlayers())
            if (PlayerAccess.TryIdentity(player, out var steamId))
            {
                var session = _sessions.GetOrAdd(player.Slot, steamId);
                session.EnforcePin = true;
                RefreshSlot(session, force);
            }
    }

    private void RefreshSlot(PlayerSession session, bool force)
    {
        if (Stopping || !_sessions.TryResolve(session, out var player)) return;
        if (!PinEnforcer.InventoryReady(player)) ScheduleInventoryRetry(session);
        else
        {
            session.InventoryRetryPending = false;
            session.InventoryRetryAttempts = 0;
        }
        if (!force && _cache.TryGetFresh(session.SteamId, out var cached))
        {
            _pins.Apply(session, player, cached);
            return;
        }
        // Pending refreshes already perform a fresh lookup. Keep one completion
        // per session; delayed events still reapply cached results when needed.
        if (session.RefreshPending) return;
        session.RefreshPending = true;
        var request = ++session.RefreshRequest;
        var map = _mapGeneration;
        _work.Run(async () =>
        {
            FaceitData? data = null;
            try
            {
                if (session.Active) data = await _lookup.Get(session.SteamId, force).ConfigureAwait(false);
            }
            finally
            {
                if (!_work.Stopping && session.Active)
                    Server.NextFrame(() =>
                    {
                        if (Stopping || map != _mapGeneration || !_sessions.IsCurrent(session) ||
                            request != session.RefreshRequest) return;
                        session.RefreshPending = false;
                        if (data != null && _sessions.TryResolve(session, out var current))
                        {
                            _pins.Apply(session, current, data);
                            if (data.Level >= 0 && !PinEnforcer.InventoryReady(current))
                                ScheduleInventoryRetry(session);
                        }
                    });
            }
        }, session.SteamId);
    }

    private void ScheduleInventoryRetry(PlayerSession session)
    {
        if (session.InventoryRetryPending || session.InventoryRetryAttempts >= InventoryRetryDelays.Length) return;
        var delay = InventoryRetryDelays[session.InventoryRetryAttempts++];
        var map = _mapGeneration;
        session.InventoryRetryPending = true;
        AddTimer(delay, () =>
        {
            if (Stopping || map != _mapGeneration || !_sessions.IsCurrent(session) ||
                !session.InventoryRetryPending) return;
            session.InventoryRetryPending = false;
            RefreshSlot(session, force: false);
        }, TimerFlags.STOP_ON_MAPCHANGE);
    }
}
