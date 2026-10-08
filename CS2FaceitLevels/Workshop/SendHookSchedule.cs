namespace CS2FaceitLevels.Workshop;

// Game-thread timing for a hook used only during connections and map changes.
internal sealed class SendHookSchedule
{
    // Keep the hook briefly after connection or map activity.
    public const double IdleSeconds = 30;
    // Keep the hook while a recent download is unfinished.
    public const double HandshakeSeconds = 120;
    // Allow time for a Workshop map download before CHANGELEVEL.
    public const double MapChangeSeconds = 600;

    private double _neededUntil;
    private double? _mapChangeStarted;
    public double? MapChangeStarted => _mapChangeStarted;

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

    public void RestoreMapChange(double? started, double now)
    {
        _mapChangeStarted = started is { } value && now - value < MapChangeSeconds ? value : null;
    }

    public bool CanRelease(double now, int waitingDeadlines, AddonHandshake handshakes)
    {
        if (now < _neededUntil || waitingDeadlines != 0) return false;
        if (_mapChangeStarted is { } started)
        {
            if (now - started < MapChangeSeconds) return false;
            _mapChangeStarted = null; // The map-change request timed out.
        }
        return !handshakes.InProgress(now, HandshakeSeconds);
    }

    public void Clear()
    {
        _neededUntil = 0;
        _mapChangeStarted = null;
    }
}
