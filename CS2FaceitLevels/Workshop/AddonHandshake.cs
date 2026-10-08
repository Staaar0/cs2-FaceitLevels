using System.Text.Json;

namespace CS2FaceitLevels.Workshop;

// Game-thread state. Track downloads by Steam ID because player slots get reused.
internal sealed class AddonHandshake
{
    private const int MaxSessions = 256;
    private readonly Dictionary<ulong, Session> _sessions = new();
    public int Count => _sessions.Count;

    internal sealed class Session
    {
        public HashSet<string> Downloaded { get; } = new(StringComparer.Ordinal);
        public string? Pending { get; set; }
        public double LastActivity { get; set; }
        public bool Active { get; set; }
    }

    public static string[] Required(string serverAddons, string badgeAddon)
    {
        var result = new List<string>();
        foreach (var raw in serverAddons.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var id = raw.Trim();
            if (id.Length > 20 || id.Any(c => c < '0' || c > '9') ||
                !ulong.TryParse(id, out var value) || value == 0)
                throw new InvalidOperationException("The engine addon list has an unexpected format.");
            if (id != badgeAddon && !result.Contains(id, StringComparer.Ordinal)) result.Add(id);
        }
        // The badge must load last so its images override the map's assets.
        result.Add(badgeAddon);
        if (result.Count > 16) throw new InvalidOperationException("Too many addons for the Workshop loader.");
        return result.ToArray();
    }

    // Use the destination map's addons. Stock maps still need the badge.
    public static string MapChangeAddon(string destinationAddons, string badgeAddon)
        => Required(destinationAddons, badgeAddon)[0];

    private Session Get(ulong steamId, double now)
    {
        if (_sessions.TryGetValue(steamId, out var found)) return found;
        Prune(now);
        // Dropping old progress only asks Steam to check the addon again.
        if (_sessions.Count >= MaxSessions)
            _sessions.Remove(_sessions.OrderBy(x => x.Value.Active)
                .ThenBy(x => x.Value.LastActivity).First().Key);
        var created = new Session { LastActivity = now };
        _sessions.Add(steamId, created);
        return created;
    }

    public string Reply(ulong steamId, string[] required, double now)
    {
        var session = Get(steamId, now);
        // Ask Steam to check the files again on a later visit.
        if (session.Active || now - session.LastActivity > 120)
        {
            session.Downloaded.Clear();
            session.Pending = null;
            session.Active = false;
        }
        // Repeated replies are normal while the Workshop popup is open.
        session.LastActivity = now;
        session.Pending = required.FirstOrDefault(id => !session.Downloaded.Contains(id));
        // CS2 downloads one missing addon at a time.
        return string.Join(',', required.Where(id => session.Downloaded.Contains(id) || id == session.Pending));
    }

    public void Connected(ulong steamId, double now)
    {
        if (!_sessions.TryGetValue(steamId, out var session)) return;
        if (session.Pending != null && now - session.LastActivity <= 120)
            session.Downloaded.Add(session.Pending);
        session.Pending = null;
        session.LastActivity = now;
    }

    public string? Next(ulong steamId, string[] required, double now)
    {
        var session = Get(steamId, now);
        if (session.Active) return null;
        var next = required.FirstOrDefault(id => !session.Downloaded.Contains(id));
        session.LastActivity = now;
        session.Pending = next;
        return next;
    }

    public void ChangingMap(ulong steamId, string? firstAddon, double now)
    {
        var session = Get(steamId, now);
        session.Downloaded.Clear();
        session.Active = false;
        session.LastActivity = now;
        session.Pending = firstAddon;
    }

    public void Active(ulong steamId, double now)
    {
        var session = Get(steamId, now);
        session.Active = true;
        session.Pending = null;
        session.LastActivity = now;
    }

    public void Disconnected(ulong steamId, double now)
    {
        if (!_sessions.TryGetValue(steamId, out var session)) return;
        if (session.Active) _sessions.Remove(steamId);
        else session.LastActivity = now; // Keep download progress for the reconnect.
    }

    // Downloads are in progress until player_connect_full.
    public bool InProgress(double now, double window)
    {
        foreach (var session in _sessions.Values)
            if (!session.Active && now - session.LastActivity <= window) return true;
        return false;
    }

    public void Prune(double now)
    {
        foreach (var id in _sessions.Where(x => !x.Value.Active && now - x.Value.LastActivity > 600)
                     .Select(x => x.Key).ToArray())
            _sessions.Remove(id);
    }

    public void Clear() => _sessions.Clear();
    public void Forget(ulong steamId) => _sessions.Remove(steamId);

    private sealed record SavedSession(ulong SteamId, string[] Downloaded, string? Pending,
        double LastActivity, bool Active);
    private sealed record ReloadState(int Version, double SavedAt, SavedSession[] Sessions,
        Dictionary<int, ulong> Slots, ConnectionDeadlines.Waiting[] Waiting, double? MapChangeStarted = null);

    public string ExportReload(Dictionary<int, ulong> slots, double now, ConnectionDeadlines? deadlines = null,
        SendHookSchedule? sendSchedule = null)
    {
        Prune(now);
        var sessions = _sessions.Select(pair => new SavedSession(pair.Key, pair.Value.Downloaded.ToArray(),
            pair.Value.Pending, pair.Value.LastActivity, pair.Value.Active)).ToArray();
        return JsonSerializer.Serialize(new ReloadState(2, now, sessions, slots,
            deadlines?.Snapshot() ?? Array.Empty<ConnectionDeadlines.Waiting>(), sendSchedule?.MapChangeStarted));
    }

    public Dictionary<int, ulong> ImportReload(string json, double now, ConnectionDeadlines? deadlines = null,
        SendHookSchedule? sendSchedule = null)
    {
        if (json.Length > 256_000) throw new InvalidOperationException("Workshop reload state exceeds its size limit.");
        var state = JsonSerializer.Deserialize<ReloadState>(json);
        if (state is not { Version: 2 } || state.Sessions == null || state.Slots == null || state.Waiting == null ||
            state.Sessions.Length > MaxSessions || state.Slots.Count > MaxSessions ||
            !double.IsFinite(state.SavedAt) || now < state.SavedAt || now - state.SavedAt > 120)
            throw new InvalidOperationException("Workshop reload state is unsupported or expired.");
        if (state.MapChangeStarted is { } started && (!double.IsFinite(started) || started > state.SavedAt))
            throw new InvalidOperationException("Workshop reload state contains an invalid map-change time.");
        var restored = new Dictionary<ulong, Session>();
        foreach (var saved in state.Sessions)
        {
            if (saved.SteamId == 0 || saved.Downloaded == null || saved.Downloaded.Length > 16 ||
                saved.Downloaded.Any(id => !ValidAddonId(id)) ||
                (saved.Pending != null && !ValidAddonId(saved.Pending)) ||
                !double.IsFinite(saved.LastActivity) || saved.LastActivity > now)
                throw new InvalidOperationException("Workshop reload state contains an invalid session.");
            if (!saved.Active && now - saved.LastActivity > 600) continue;
            var session = new Session { Pending = saved.Pending, LastActivity = saved.LastActivity,
                Active = saved.Active };
            session.Downloaded.UnionWith(saved.Downloaded);
            if (!restored.TryAdd(saved.SteamId, session))
                throw new InvalidOperationException("Duplicate player in Workshop reload state.");
        }
        var bindings = new Dictionary<int, ulong>();
        foreach (var pair in state.Slots)
        {
            if (pair.Key is < 0 or >= 256 || pair.Value == 0)
                throw new InvalidOperationException("Invalid slot in Workshop reload state.");
            bindings.Add(pair.Key, pair.Value);
        }
        (deadlines ?? new ConnectionDeadlines()).Restore(state.Waiting, bindings, now);
        // Apply the snapshot only after all validation passes.
        sendSchedule?.RestoreMapChange(state.MapChangeStarted, now);
        _sessions.Clear();
        foreach (var pair in restored) _sessions.Add(pair.Key, pair.Value);
        return bindings;
    }

    private static bool ValidAddonId(string? value) => value is { Length: > 0 and <= 20 } &&
        value.All(c => c is >= '0' and <= '9') && ulong.TryParse(value, out var id) && id != 0;
}
