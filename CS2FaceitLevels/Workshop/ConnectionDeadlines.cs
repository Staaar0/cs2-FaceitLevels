namespace CS2FaceitLevels.Workshop;

// Only unfinished Workshop handshakes are tracked. Retransmissions do not extend
// the deadline. All state is owned by the game thread, including timer callbacks.
internal sealed class ConnectionDeadlines
{
    public const double TimeoutSeconds = 30;
    internal sealed record Waiting(int Slot, ulong SteamId, double Started);
    private readonly Dictionary<int, Waiting> _waiting = new();
    public int Count => _waiting.Count;

    public bool Wait(int slot, ulong steamId, double now)
    {
        if (slot is < 0 or >= 256 || steamId == 0)
            throw new InvalidOperationException("Invalid Workshop connection identity.");
        if (!_waiting.TryGetValue(slot, out var pending) || pending.SteamId != steamId)
            _waiting[slot] = pending = new Waiting(slot, steamId, now);
        return now - pending.Started >= TimeoutSeconds;
    }

    public void Remove(int slot) => _waiting.Remove(slot);
    public void Clear() => _waiting.Clear();
    public Waiting[] Snapshot() => _waiting.Values.ToArray();
    public Waiting[] Expired(double now) => _waiting.Values
        .Where(x => now - x.Started >= TimeoutSeconds).ToArray();

    public bool TakeExpired(Waiting pending, ulong currentSteamId, double now)
    {
        // A callback may run after a disconnect/reconnect or after this slot was
        // reused. Never disconnect a replacement connection using an old deadline.
        if (!_waiting.TryGetValue(pending.Slot, out var current) ||
            !ReferenceEquals(current, pending) || now - current.Started < TimeoutSeconds)
            return false;
        _waiting.Remove(pending.Slot);
        return currentSteamId == pending.SteamId;
    }

    public void Restore(Waiting[] waiting, Dictionary<int, ulong> slots, double now)
    {
        if (waiting.Length > 256) throw new InvalidOperationException("Too many Workshop deadlines.");
        var restored = new Dictionary<int, Waiting>();
        foreach (var item in waiting)
        {
            if (item == null || item.Slot is < 0 or >= 256 || item.SteamId == 0 ||
                !double.IsFinite(item.Started) || item.Started > now ||
                !slots.TryGetValue(item.Slot, out var steamId) || steamId != item.SteamId ||
                !restored.TryAdd(item.Slot, item))
                throw new InvalidOperationException("Invalid Workshop reload deadline.");
        }
        _waiting.Clear();
        foreach (var pair in restored) _waiting.Add(pair.Key, pair.Value);
    }
}
