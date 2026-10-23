namespace CS2FaceitLevels.Workshop;

internal sealed class SendHookSchedule
{
    public const double MapChangeSeconds = 600;
    public const double LoadingSeconds = 300;

    private readonly Dictionary<int, double> _loading = new();
    private double? _mapChangeStarted;
    public double? MapChangeStarted => _mapChangeStarted;

    public void Loading(int slot, double now) => _loading.TryAdd(slot, now);

    public void Loaded(int slot) => _loading.Remove(slot);

    public void MapChangeRequested(double now) => _mapChangeStarted = now;

    public void MapStarted() => _mapChangeStarted = null;

    public void RestoreMapChange(double? started, double now)
    {
        _mapChangeStarted = started is { } value && now - value < MapChangeSeconds ? value : null;
    }

    public bool CanRelease(double now, int waitingDeadlines)
    {
        if (waitingDeadlines != 0) return false;
        if (_mapChangeStarted is { } started)
        {
            if (now - started < MapChangeSeconds) return false;
            _mapChangeStarted = null;
        }
        if (_loading.Count == 0) return true;
        foreach (var slot in _loading.Where(x => now - x.Value >= LoadingSeconds).Select(x => x.Key).ToArray())
            _loading.Remove(slot);
        return _loading.Count == 0;
    }

    public void Clear()
    {
        _loading.Clear();
        _mapChangeStarted = null;
    }
}
