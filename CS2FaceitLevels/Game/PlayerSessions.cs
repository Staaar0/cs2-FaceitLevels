using System.Diagnostics.CodeAnalysis;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Entities.Constants;

namespace CS2FaceitLevels;

internal sealed class PlayerSession(int slot, ulong steamId, long generation, int index)
{
    internal readonly int Slot = slot;
    internal readonly ulong SteamId = steamId;
    internal readonly long Generation = generation;
    internal int Index = index;
    internal volatile bool Active = true;
    internal bool EnforcePin;
    internal MedalRank_t? DesiredPin;
    internal bool RefreshPending;
    internal long RefreshRequest;
    internal long? LastCommandTime;
    internal bool CommandPending;
}

// Main-thread collection: compact iteration and no dictionary lookup per tick.
// Workers receive a session identity and read Active, never game entity handles.
internal sealed class PlayerSessions
{
    private readonly Dictionary<int, PlayerSession> _bySlot = new();
    private readonly Dictionary<int, long> _pendingPuts = new();
    internal readonly List<PlayerSession> Active = new(64);
    private long _generation;

    internal long ReservePut(int slot)
    {
        var version = ++_generation;
        _pendingPuts[slot] = version;
        return version;
    }

    internal bool IsPendingPut(int slot, long version) =>
        _pendingPuts.TryGetValue(slot, out var current) && version == current;

    internal PlayerSession GetOrAdd(int slot, ulong steamId)
    {
        if (_bySlot.TryGetValue(slot, out var current))
        {
            if (current.SteamId == steamId) return current;
            Remove(slot);
        }
        var session = new PlayerSession(slot, steamId, ++_generation, Active.Count);
        _bySlot.Add(slot, session);
        Active.Add(session);
        return session;
    }

    internal bool IsCurrent(PlayerSession session) => session.Active &&
        _bySlot.TryGetValue(session.Slot, out var current) && session.Generation == current.Generation;

    internal bool TryResolve(PlayerSession session, [NotNullWhen(true)] out CCSPlayerController? player)
    {
        player = null;
        if (!IsCurrent(session)) return false;
        player = Utilities.GetPlayerFromSlot(session.Slot);
        return PlayerAccess.TryIdentity(player, out var steamId) && steamId == session.SteamId;
    }

    internal void Remove(int slot, ulong? expectedSteamId = null)
    {
        if (expectedSteamId.HasValue && _bySlot.TryGetValue(slot, out var existing) &&
            existing.SteamId != expectedSteamId.Value) return;
        _pendingPuts.Remove(slot);
        if (!_bySlot.TryGetValue(slot, out var session)) return;
        session.Active = false;
        _bySlot.Remove(slot);
        var last = Active[^1];
        Active[session.Index] = last;
        last.Index = session.Index;
        Active.RemoveAt(Active.Count - 1);
    }

    internal void Clear()
    {
        foreach (var session in Active) session.Active = false;
        Active.Clear();
        _bySlot.Clear();
        _pendingPuts.Clear();
    }
}

internal static class PlayerAccess
{
    internal static bool TryIdentity([NotNullWhen(true)] CCSPlayerController? player, out ulong steamId,
        bool connected = false)
    {
        steamId = 0;
        if (player is not { IsValid: true, IsBot: false }) return false;
        steamId = player.SteamID;
        return steamId != 0 && (!connected || player.Connected == PlayerConnectedState.Connected);
    }
}
