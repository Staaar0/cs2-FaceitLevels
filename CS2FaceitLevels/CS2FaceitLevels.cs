using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes;
using CounterStrikeSharp.API.Modules.Timers;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

[MinimumApiVersion(371)]
public sealed class CS2FaceitLevels : BasePlugin, IPluginConfig<CS2FaceitLevelsConfig>
{
    public override string ModuleName => "CS2FaceitLevels";
    public override string ModuleAuthor => "✪ Stαr";
    public override string ModuleVersion => "1.0.9";
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

        RegisterEventHandler<EventPlayerConnectFull>(OnPlayerConnectFull);
        RegisterEventHandler<EventPlayerSpawn>(OnPlayerSpawn);
        RegisterEventHandler<EventPlayerTeam>(OnPlayerTeam);
        RegisterEventHandler<EventRoundStart>(OnRoundStart);
        RegisterEventHandler<EventPlayerDisconnect>(OnPlayerDisconnect);
        RegisterListener<Listeners.OnTick>(EnforcePins);
        RegisterListener<Listeners.OnClientPutInServer>(OnClientPutInServer);
        RegisterListener<Listeners.OnClientDisconnect>(OnClientDisconnect);
        RegisterListener<Listeners.OnMapStart>(OnMapStart);

        if (hotReload)
        {
            foreach (var player in Utilities.GetPlayers())
                if (PlayerAccess.TryIdentity(player, out var steamId))
                    _sessions.GetOrAdd(player.Slot, steamId).EnforcePin = true;
        }
        _reloadOnFirstConnect = !hotReload;
        AddTimer(60f, _cache.RequestMaintenance, TimerFlags.REPEAT);
        if (Config.EnableEloCommands)
        {
            AddCommand("css_elo", "Show a player's FACEIT elo.", _commands.Single);
            AddCommand("css_elos", "Show every player's FACEIT elo.", _commands.All);
        }
        AddTimer(2f, () => RefreshAll(force: hotReload), TimerFlags.STOP_ON_MAPCHANGE);
    }

    public override void Unload(bool hotReload)
    {
        if (_unloading) return;
        _unloading = true;
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
        _mapGeneration++;
        foreach (var session in _sessions.Active)
        {
            session.RefreshPending = false;
            session.RefreshRequest++;
        }
        AddTimer(2f, () => RefreshAll(force: false), TimerFlags.STOP_ON_MAPCHANGE);
    }

    private HookResult OnPlayerConnectFull(EventPlayerConnectFull e, GameEventInfo info)
    {
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
    private HookResult OnPlayerTeam(EventPlayerTeam e, GameEventInfo info) => Refresh(e.Userid, 0.5f);
    private HookResult OnRoundStart(EventRoundStart e, GameEventInfo info)
    {
        AddTimer(1f, () => RefreshAll(force: false), TimerFlags.STOP_ON_MAPCHANGE);
        return HookResult.Continue;
    }

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
            if (PlayerAccess.TryIdentity(player, out var steamId, connected: true))
                _sessions.GetOrAdd(slot, steamId).EnforcePin = true;
        });
    }

    private void OnClientDisconnect(int slot) => _sessions.Remove(slot);

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
            var map = _mapGeneration;
            // Preserve event delays: these retries cover temporarily missing inventory services.
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
                RefreshSlot(_sessions.GetOrAdd(player.Slot, steamId), force);
    }

    private void RefreshSlot(PlayerSession session, bool force)
    {
        if (Stopping || !_sessions.TryResolve(session, out var player)) return;
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
                        if (data != null && _sessions.TryResolve(session, out var current)) _pins.Apply(session, current, data);
                    });
            }
        }, session.SteamId);
    }
}
