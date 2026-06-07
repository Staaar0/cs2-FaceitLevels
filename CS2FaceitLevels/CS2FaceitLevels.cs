using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Serialization;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Entities.Constants;
using CounterStrikeSharp.API.Modules.Timers;
using CounterStrikeSharp.API.Modules.Utils;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

public sealed class CS2FaceitLevels : BasePlugin, IPluginConfig<CS2FaceitLevelsConfig>
{
    public override string ModuleName => "CS2FaceitLevels";
    public override string ModuleAuthor => "✪ Stαr";
    public override string ModuleVersion => "1.0.4";
    public override string ModuleDescription => "Shows real FACEIT levels in the CS2 scoreboard.";

    private const string DefaultApiKey = "PUT_YOUR_FACEIT_API_KEY_HERE";

    private static readonly Dictionary<int, int> LevelPins = new()
    {
        [1] = 1017, [2] = 1032, [3] = 1019, [4] = 1005, [5] = 1051, [6] = 1007,
        [7] = 1020, [8] = 1082, [9] = 1035, [10] = 1060, [11] = 1010,
    };

    private static readonly Dictionary<string, char> Colors = new(StringComparer.OrdinalIgnoreCase)
    {
        ["default"] = ChatColors.Default, ["white"] = ChatColors.White,
        ["darkred"] = ChatColors.DarkRed, ["red"] = ChatColors.Red, ["lightred"] = ChatColors.LightRed,
        ["green"] = ChatColors.Green, ["lime"] = ChatColors.Lime, ["olive"] = ChatColors.Olive,
        ["yellow"] = ChatColors.Yellow, ["lightyellow"] = ChatColors.LightYellow, ["gold"] = ChatColors.Gold,
        ["orange"] = ChatColors.Orange, ["blue"] = ChatColors.Blue, ["darkblue"] = ChatColors.DarkBlue,
        ["lightblue"] = ChatColors.LightBlue, ["purple"] = ChatColors.Purple, ["lightpurple"] = ChatColors.LightPurple,
        ["grey"] = ChatColors.Grey, ["gray"] = ChatColors.Grey, ["silver"] = ChatColors.Silver,
        ["magenta"] = ChatColors.Magenta, ["bluegrey"] = ChatColors.BlueGrey,
    };

    private static readonly HttpClient Http = new();
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    private readonly ConcurrentDictionary<ulong, CachedData> _cache = new();
    private readonly ConcurrentDictionary<ulong, byte> _fetching = new();

    private CS2FaceitLevelsLang _lang = new();

    public CS2FaceitLevelsConfig Config { get; set; } = new();

    public void OnConfigParsed(CS2FaceitLevelsConfig config)
    {
        if (config.CacheMinutes < 1) config.CacheMinutes = 1;
        if (config.RequestTimeoutSeconds < 2) config.RequestTimeoutSeconds = 2;
        if (string.IsNullOrWhiteSpace(config.Language)) config.Language = "en";

        Config = config;
        _lang = LoadLanguage(config.Language);
    }

    public override void Load(bool hotReload)
    {
        RegisterEventHandler<EventPlayerConnectFull>(OnPlayerConnectFull);
        RegisterEventHandler<EventPlayerSpawn>(OnPlayerSpawn);
        RegisterEventHandler<EventPlayerTeam>(OnPlayerTeam);
        RegisterEventHandler<EventRoundStart>(OnRoundStart);
        RegisterEventHandler<EventPlayerDisconnect>(OnPlayerDisconnect);

        AddCommand("css_cs2faceitlevels_refresh", "Refresh FACEIT pins for all players.", OnRefreshCommand);

        if (Config.EnableEloCommands)
        {
            AddCommand("css_elo", "Show a player's FACEIT elo.", OnEloCommand);
            AddCommand("css_elos", "Show every player's FACEIT elo.", OnElosCommand);
        }

        if (hotReload)
            AddTimer(2f, () => RefreshAll(force: true));
    }

    private HookResult OnPlayerConnectFull(EventPlayerConnectFull e, GameEventInfo info) => Refresh(e.Userid, 2f);
    private HookResult OnPlayerSpawn(EventPlayerSpawn e, GameEventInfo info) => Refresh(e.Userid, 0.2f);
    private HookResult OnPlayerTeam(EventPlayerTeam e, GameEventInfo info) => Refresh(e.Userid, 0.5f);

    private HookResult OnRoundStart(EventRoundStart e, GameEventInfo info)
    {
        AddTimer(1f, () => RefreshAll(force: false), TimerFlags.STOP_ON_MAPCHANGE);
        return HookResult.Continue;
    }

    private HookResult OnPlayerDisconnect(EventPlayerDisconnect e, GameEventInfo info)
    {
        if (e.Userid is { } player && player.SteamID != 0)
            _fetching.TryRemove(player.SteamID, out _);
        return HookResult.Continue;
    }

    private HookResult Refresh(CCSPlayerController? player, float delay)
    {
        if (IsValid(player))
        {
            var slot = player.Slot;
            AddTimer(delay, () => RefreshSlot(slot, force: false), TimerFlags.STOP_ON_MAPCHANGE);
        }
        return HookResult.Continue;
    }

    private void RefreshAll(bool force)
    {
        foreach (var player in GetPlayers())
            RefreshSlot(player.Slot, force);
    }

    private void RefreshSlot(int slot, bool force)
    {
        var player = Utilities.GetPlayerFromSlot(slot);
        if (!IsValid(player))
            return;

        var steamId = player.SteamID;

        if (!force && _cache.TryGetValue(steamId, out var cached) && cached.ExpiresAt > DateTime.UtcNow)
        {
            Apply(player, cached);
            return;
        }

        if (!_fetching.TryAdd(steamId, 0))
            return;

        var name = player.PlayerName;
        Task.Run(() => FetchAndApply(slot, steamId, name));
    }

    private async Task FetchAndApply(int slot, ulong steamId, string name)
    {
        CachedData data;
        try
        {
            data = await FetchFromFaceit(steamId);
        }
        catch (Exception ex)
        {
            Logger.LogWarning(ex, "[CS2FaceitLevels] FACEIT lookup failed for {Name} ({SteamId}).", name, steamId);
            data = NoFaceit();
        }
        finally
        {
            _fetching.TryRemove(steamId, out _);
        }

        _cache[steamId] = data;

        Server.NextFrame(() =>
        {
            var player = Utilities.GetPlayerFromSlot(slot);
            if (IsValid(player) && player.SteamID == steamId)
                Apply(player, data);
        });
    }

    private void Apply(CCSPlayerController player, CachedData data)
    {
        if (player.InventoryServices == null)
            return;

        if (data.Level >= 1 && LevelPins.TryGetValue(data.Level, out var pin))
            player.InventoryServices.Rank[5] = (MedalRank_t)pin;
        else if (Config.ClearPinWhenNoFaceit)
            player.InventoryServices.Rank[5] = MedalRank_t.MEDAL_RANK_NONE;
        else
            return;

        Utilities.SetStateChanged(player, "CCSPlayerController", "m_pInventoryServices");

        if (Config.Debug)
            Logger.LogInformation("[CS2FaceitLevels] Updated scoreboard pin for {Name} (level {Level}).", player.PlayerName, data.Level);
    }

    private async Task<CachedData> FetchFromFaceit(ulong steamId)
    {
        if (string.IsNullOrWhiteSpace(Config.FaceitApiKey) || Config.FaceitApiKey == DefaultApiKey)
        {
            if (Config.Debug)
                Logger.LogWarning("[CS2FaceitLevels] No FACEIT API key set in the config.");

            return NoFaceit();
        }

        var player = await GetJson<FaceitPlayer>($"https://open.faceit.com/data/v4/players?game=cs2&game_player_id={steamId}");
        var cs2 = player?.Games?.Cs2;

        if (cs2?.SkillLevel is not (>= 1 and <= 10))
            return NoFaceit();

        var level = cs2.SkillLevel.Value;

        if (level == 10 && !string.IsNullOrEmpty(player!.PlayerId) && !string.IsNullOrEmpty(cs2.Region)
            && await IsChallenger(player.PlayerId!, cs2.Region!))
        {
            level = 11;
        }

        return new CachedData(level, cs2.Elo, DateTime.UtcNow.AddMinutes(Config.CacheMinutes));
    }

    private async Task<bool> IsChallenger(string playerId, string region)
    {
        try
        {
            var url = $"https://open.faceit.com/data/v4/rankings/games/cs2/regions/{Uri.EscapeDataString(region)}/players/{Uri.EscapeDataString(playerId)}";
            var ranking = await GetJson<FaceitRanking>(url);
            var position = ranking?.Position ?? ranking?.Items?.FirstOrDefault()?.Position ?? 0;
            return position is > 0 and <= 1000;
        }
        catch (Exception ex)
        {
            if (Config.Debug)
                Logger.LogWarning(ex, "[CS2FaceitLevels] Challenger check failed for {PlayerId}.", playerId);

            return false;
        }
    }

    private async Task<T?> GetJson<T>(string url) where T : class
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", Config.FaceitApiKey);

        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(Config.RequestTimeoutSeconds));
        using var response = await Http.SendAsync(request, cts.Token);

        if (response.StatusCode == HttpStatusCode.NotFound)
            return null;

        if (!response.IsSuccessStatusCode)
        {
            if (Config.Debug)
                Logger.LogWarning("[CS2FaceitLevels] FACEIT API returned status {Status}.", (int)response.StatusCode);

            return null;
        }

        var body = await response.Content.ReadAsStringAsync(cts.Token);
        return JsonSerializer.Deserialize<T>(body, JsonOptions);
    }

    private CachedData NoFaceit() => new(0, null, DateTime.UtcNow.AddMinutes(Config.CacheMinutes));

    private void OnRefreshCommand(CCSPlayerController? player, CommandInfo command)
    {
        if (player != null)
        {
            command.ReplyToCommand("[CS2FaceitLevels] This command can only be run from the server console.");
            return;
        }

        RefreshAll(force: true);
        command.ReplyToCommand("[CS2FaceitLevels] Refreshing FACEIT levels for all players.");
    }

    private void OnEloCommand(CCSPlayerController? caller, CommandInfo command)
    {
        if (!IsValid(caller))
        {
            command.ReplyToCommand(Format(_lang.PlayerOnlyMessage));
            return;
        }

        var search = JoinArgs(command);
        if (search.Length == 0)
        {
            caller.PrintToChat(Format(_lang.MissingPlayerNameMessage));
            return;
        }

        var matches = GetPlayers()
            .Where(p => p.PlayerName.Contains(search, StringComparison.OrdinalIgnoreCase))
            .OrderBy(p => p.PlayerName)
            .ToList();

        if (matches.Count == 0)
        {
            caller.PrintToChat(Format(_lang.NoPlayerFoundMessage, ("SEARCH", search)));
            return;
        }

        if (matches.Count > 1)
        {
            var names = string.Join(", ", matches.Take(5).Select(p => p.PlayerName));
            caller.PrintToChat(Format(_lang.MultiplePlayersFoundMessage, ("PLAYERS", names)));
            return;
        }

        var callerSlot = caller.Slot;
        var targetName = matches[0].PlayerName;
        var targetSteamId = matches[0].SteamID;

        Task.Run(async () =>
        {
            var data = await GetOrFetch(targetSteamId);
            Server.NextFrame(() =>
            {
                var c = Utilities.GetPlayerFromSlot(callerSlot);
                if (IsValid(c))
                    c.PrintToChat(EloLine(_lang.SingleEloChatFormat, targetName, targetSteamId, data));
            });
        });
    }

    private void OnElosCommand(CCSPlayerController? caller, CommandInfo command)
    {
        if (!IsValid(caller))
        {
            command.ReplyToCommand(Format(_lang.PlayerOnlyMessage));
            return;
        }

        var callerSlot = caller.Slot;
        var targets = GetPlayers()
            .OrderBy(p => p.TeamNum)
            .ThenBy(p => p.PlayerName)
            .Select(p => (p.SteamID, p.PlayerName))
            .ToList();

        Task.Run(async () =>
        {
            var lines = new List<string>();
            foreach (var (steamId, name) in targets)
                lines.Add(EloLine(_lang.AllElosChatFormat, name, steamId, await GetOrFetch(steamId)));

            Server.NextFrame(() =>
            {
                var c = Utilities.GetPlayerFromSlot(callerSlot);
                if (!IsValid(c))
                    return;

                foreach (var line in lines)
                    c.PrintToChat(line);
            });
        });
    }

    private async Task<CachedData> GetOrFetch(ulong steamId)
    {
        if (_cache.TryGetValue(steamId, out var cached) && cached.ExpiresAt > DateTime.UtcNow)
            return cached;

        CachedData data;
        try
        {
            data = await FetchFromFaceit(steamId);
        }
        catch (Exception ex)
        {
            Logger.LogWarning(ex, "[CS2FaceitLevels] FACEIT lookup failed for {SteamId}.", steamId);
            data = NoFaceit();
        }

        _cache[steamId] = data;
        return data;
    }

    private CS2FaceitLevelsLang LoadLanguage(string language)
    {
        var name = string.IsNullOrWhiteSpace(language) ? "en" : language.Trim();
        var langDirectory = Path.Combine(ModuleDirectory, "lang");
        var path = Path.Combine(langDirectory, name + ".json");

        if (!File.Exists(path))
        {
            Logger.LogWarning("[CS2FaceitLevels] Language '{Language}' not found in {Directory}, using English.", name, langDirectory);
            path = Path.Combine(langDirectory, "en.json");
        }

        try
        {
            if (File.Exists(path))
            {
                var lang = JsonSerializer.Deserialize<CS2FaceitLevelsLang>(File.ReadAllText(path), JsonOptions);
                if (lang != null)
                    return lang;
            }
        }
        catch (Exception ex)
        {
            Logger.LogWarning(ex, "[CS2FaceitLevels] Failed to read language file {Path}, using built-in English.", path);
        }

        return new CS2FaceitLevelsLang();
    }

    private string Format(string template, params (string Key, string Value)[] replacements)
    {
        var message = template.Replace("{PREFIX}", _lang.ChatPrefix, StringComparison.OrdinalIgnoreCase);

        foreach (var (key, value) in replacements)
            message = message.Replace("{" + key + "}", value, StringComparison.OrdinalIgnoreCase);

        return ApplyColors(message);
    }

    private string EloLine(string template, string playerName, ulong steamId, CachedData data)
    {
        var message = template
            .Replace("{PREFIX}", _lang.ChatPrefix, StringComparison.OrdinalIgnoreCase)
            .Replace("{PLAYER_COLOR}", "{RED}", StringComparison.OrdinalIgnoreCase)
            .Replace("{LABEL_COLOR}", "{LIGHTPURPLE}", StringComparison.OrdinalIgnoreCase)
            .Replace("{ELO_COLOR}", EloColor(data.SkillLevel), StringComparison.OrdinalIgnoreCase)
            .Replace("{PLAYER}", playerName, StringComparison.OrdinalIgnoreCase)
            .Replace("{STEAMID64}", steamId.ToString(), StringComparison.OrdinalIgnoreCase)
            .Replace("{ELO}", data.Elo?.ToString() ?? "N/A", StringComparison.OrdinalIgnoreCase)
            .Replace("{LEVEL}", data.SkillLevel > 0 ? data.SkillLevel.ToString() : "N/A", StringComparison.OrdinalIgnoreCase);

        return ApplyColors(message);
    }

    private static string EloColor(int skillLevel) => skillLevel switch
    {
        1 => "{GREY}",
        2 or 3 => "{LIME}",
        >= 4 and <= 7 => "{YELLOW}",
        8 or 9 => "{ORANGE}",
        10 => "{RED}",
        _ => "{GREY}",
    };

    private static string ApplyColors(string message)
    {
        foreach (var (tag, color) in Colors)
            message = message.Replace("{" + tag + "}", color.ToString(), StringComparison.OrdinalIgnoreCase);

        return message;
    }

    private static string JoinArgs(CommandInfo command)
    {
        var args = new List<string>();
        for (var i = 1; i < command.ArgCount; i++)
        {
            var arg = command.ArgByIndex(i);
            if (!string.IsNullOrWhiteSpace(arg))
                args.Add(arg);
        }

        return string.Join(" ", args);
    }

    private static IEnumerable<CCSPlayerController> GetPlayers() => Utilities.GetPlayers().Where(IsValid);

    private static bool IsValid([NotNullWhen(true)] CCSPlayerController? player) =>
        player is { IsValid: true, IsBot: false, Connected: PlayerConnectedState.PlayerConnected, SteamID: not 0 };

    private sealed record CachedData(int Level, int? Elo, DateTime ExpiresAt)
    {
        public int SkillLevel => Level == 0 ? 0 : Math.Min(Level, 10);
    }

    private sealed class FaceitPlayer
    {
        [JsonPropertyName("player_id")] public string? PlayerId { get; set; }
        [JsonPropertyName("games")] public FaceitGames? Games { get; set; }
    }

    private sealed class FaceitGames
    {
        [JsonPropertyName("cs2")] public FaceitGame? Cs2 { get; set; }
    }

    private sealed class FaceitGame
    {
        [JsonPropertyName("skill_level")] public int? SkillLevel { get; set; }
        [JsonPropertyName("faceit_elo")] public int? Elo { get; set; }
        [JsonPropertyName("region")] public string? Region { get; set; }
    }

    private sealed class FaceitRanking
    {
        [JsonPropertyName("position")] public int? Position { get; set; }
        [JsonPropertyName("items")] public List<FaceitRanking>? Items { get; set; }
    }
}
