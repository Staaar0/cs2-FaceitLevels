using System.Collections.Concurrent;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

internal sealed class FaceitLookup(FaceitClient api, FaceitCache cache, CancellationToken token,
    Func<bool> debug, ILogger logger)
{
    private const int MaxPendingRequests = 128;
    private readonly SemaphoreSlim _pending = new(MaxPendingRequests, MaxPendingRequests);
    private readonly ConcurrentDictionary<ulong, Lazy<Task<FaceitData>>> _fetching = new();

    internal async Task<FaceitData> Get(ulong steamId, bool force = false)
    {
        await cache.Ready.WaitAsync(token).ConfigureAwait(false);
        token.ThrowIfCancellationRequested();
        if (!force && cache.TryGetFresh(steamId, out var cached)) return cached;

        var request = _fetching.GetOrAdd(steamId, id => new Lazy<Task<FaceitData>>(
            () => Fetch(id), LazyThreadSafetyMode.ExecutionAndPublication));
        try { return await request.Value.ConfigureAwait(false); }
        finally { _fetching.TryRemove(new KeyValuePair<ulong, Lazy<Task<FaceitData>>>(steamId, request)); }
    }

    private async Task<FaceitData> Fetch(ulong steamId)
    {
        var acquired = false;
        try
        {
            acquired = await _pending.WaitAsync(0, token).ConfigureAwait(false);
            if (!acquired) return FaceitData.RequestFailed;
            var data = await api.Fetch(steamId, token).ConfigureAwait(false);
            token.ThrowIfCancellationRequested();
            cache.Store(steamId, data);
            return data;
        }
        catch (OperationCanceledException) { return FaceitData.RequestFailed; }
        catch (Exception ex)
        {
            if (token.IsCancellationRequested) return FaceitData.RequestFailed;
            if (debug())
                logger.LogWarning(ex, "[CS2FaceitLevels] FACEIT lookup failed for {SteamId}.", steamId);
            if (cache.TryGetFresh(steamId, out var cached)) return cached;
            var failure = new FaceitData(-1, null, DateTime.UtcNow.AddMinutes(2));
            cache.Store(steamId, failure);
            return failure;
        }
        finally { if (acquired) _pending.Release(); }
    }
}
