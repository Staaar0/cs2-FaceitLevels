namespace CS2FaceitLevels.Workshop;

// Pure timing state for the SendNetMessage hook. The engine calls that function for
// every message to every client, but the loader only acts on SignonState, which is
// sent while a client signs on or the server changes map. The hook is attached for
// those windows and released afterwards. Game thread only; no game natives.
internal sealed class SendHookSchedule
{
    // After the last reply, ClientConnect, SignonState or map start.
    public const double IdleSeconds = 30;
    // An unfinished handshake with recent activity keeps the hook (see AddonHandshake).
    public const double HandshakeSeconds = 120;
    // A host-state request precedes CHANGELEVEL, possibly by a Workshop map download.
    public const double MapChangeSeconds = 600;

    private double _neededUntil;
    private double? _mapChangeStarted;

    public void Activity(double now) => _neededUntil = Math.Max(_neededUntil, now + IdleSeconds);

    public void MapChangeRequested(double now)
    {
        _mapChangeStarted = now;
        Activity(now);
    }

    public void MapStarted(double now)
    {
        _mapChangeStarted = null;
        Activity(now);
    }

    public bool CanRelease(double now, int waitingDeadlines, AddonHandshake handshakes)
    {
        if (now < _neededUntil || waitingDeadlines != 0) return false;
        if (_mapChangeStarted is { } started)
        {
            if (now - started < MapChangeSeconds) return false;
            _mapChangeStarted = null; // A request that never reached a map start.
        }
        return !handshakes.InProgress(now, HandshakeSeconds);
    }

    public void Clear()
    {
        _neededUntil = 0;
        _mapChangeStarted = null;
    }
}
