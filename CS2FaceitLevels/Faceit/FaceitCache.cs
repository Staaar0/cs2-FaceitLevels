using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Text.Json;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

// The game thread only performs TryGetFresh and requests maintenance.
// Disk I/O, JSON and eviction run on owned background jobs.
internal sealed class FaceitCache
{
    private const int MaxEntries = 10_000;
    private const long MaxFileBytes = 8 * 1024 * 1024;
    private static readonly SemaphoreSlim FileLock = new(1, 1);
    private readonly ConcurrentDictionary<ulong, FaceitData> _entries = new();
    private readonly object _dataGate = new();
    private readonly object _maintenanceGate = new();
    private readonly CancellationTokenSource _ioStop = new();
    private readonly string _path;
    private readonly string _tempPath;
    private readonly Func<bool> _debug;
    private readonly ILogger _logger;
    private Task _maintenance = Task.CompletedTask;
    private long _version;
    private long _savedVersion;
    private bool _closed;
    private bool _acceptWrites = true;

    internal Task Ready { get; }

    internal FaceitCache(string path, CancellationToken lifetime, Func<bool> debug, ILogger logger)
    {
        _path = path;
        _tempPath = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        _debug = debug;
        _logger = logger;
        Ready = Task.Run(() => Load(lifetime));
    }

    internal bool TryGetFresh(ulong steamId, [NotNullWhen(true)] out FaceitData? data)
    {
        if (_entries.TryGetValue(steamId, out data) && data.ExpiresAt > DateTime.UtcNow) return true;
        data = null;
        return false;
    }

    internal void Store(ulong steamId, FaceitData data)
    {
        lock (_dataGate)
        {
            if (!_acceptWrites) return;
            _entries[steamId] = data;
            if (data.Level >= 0) _version++;
            if (_entries.Count > MaxEntries) Compact();
        }
    }

    // Called only under _dataGate, on a worker. All cache writes use this gate.
    // One pass removes expired entries and finds the oldest survivor. Each store
    // adds at most one entry, so overflow needs at most one further removal.
    private void Compact()
    {
        var now = DateTime.UtcNow;
        KeyValuePair<ulong, FaceitData>? oldest = null;
        foreach (var entry in _entries)
        {
            if (entry.Value.ExpiresAt <= now)
            {
                Remove(entry);
                continue;
            }
            if (oldest == null || entry.Value.ExpiresAt < oldest.Value.Value.ExpiresAt)
                oldest = entry;
        }
        if (_entries.Count > MaxEntries && oldest is { } candidate) Remove(candidate);
    }

    private void Remove(KeyValuePair<ulong, FaceitData> entry)
    {
        // Conditional removal cannot evict a different, newly refreshed value.
        if (_entries.TryRemove(entry) && entry.Value.Level >= 0) _version++;
    }

    internal void RequestMaintenance()
    {
        lock (_maintenanceGate)
        {
            if (_closed || !_maintenance.IsCompleted) return;
            _maintenance = Task.Run(async () =>
            {
                try
                {
                    await Ready.ConfigureAwait(false);
                    _ioStop.Token.ThrowIfCancellationRequested();
                    lock (_dataGate) Compact();
                    await Flush(_ioStop.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (_ioStop.IsCancellationRequested) { }
                catch (Exception ex)
                {
                    if (_debug()) _logger.LogWarning(ex, "[CS2FaceitLevels] Cache maintenance failed.");
                }
            });
        }
    }

    private async Task Load(CancellationToken token)
    {
        try
        {
            if (!File.Exists(_path)) return;
            await using var stream = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.Read,
                16 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
            if (stream.Length > MaxFileBytes)
            {
                _logger.LogWarning("[CS2FaceitLevels] Cache file {Path} is unexpectedly large ({Bytes} bytes), ignoring it.",
                    _path, stream.Length);
                return;
            }
            var entries = await JsonSerializer.DeserializeAsync<List<PersistentEntry>>(stream, FaceitJson.Options, token)
                .ConfigureAwait(false);
            if (entries == null) return;
            var now = DateTime.UtcNow;
            var valid = entries.Where(e => e.SteamId != 0 && e.Level >= 0 && e.ExpiresAt > now)
                .OrderByDescending(e => e.ExpiresAt).Take(MaxEntries).ToList();
            token.ThrowIfCancellationRequested();
            lock (_dataGate)
            {
                if (!_acceptWrites) return;
                // Lookups await Ready, so loading cannot overwrite an HTTP result.
                foreach (var e in valid) _entries[e.SteamId] = new FaceitData(e.Level, e.Elo, e.ExpiresAt);
                if (valid.Count != entries.Count || _entries.Count != valid.Count) _version++;
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "[CS2FaceitLevels] Failed to read cache file {Path}.", _path);
        }
    }

    private async Task Flush(CancellationToken token)
    {
        var acquired = false;
        try
        {
            await FileLock.WaitAsync(token).ConfigureAwait(false);
            acquired = true;
            KeyValuePair<ulong, FaceitData>[] snapshot;
            long version;
            lock (_dataGate)
            {
                if (_savedVersion == _version) return;
                version = _version;
                snapshot = _entries.ToArray();
            }
            var now = DateTime.UtcNow;
            var entries = snapshot.Where(e => e.Value.Level >= 0 && e.Value.ExpiresAt > now)
                .OrderByDescending(e => e.Value.ExpiresAt).Take(MaxEntries)
                .Select(e => new PersistentEntry(e.Key, e.Value.Level, e.Value.Elo, e.Value.ExpiresAt))
                .OrderBy(e => e.SteamId).ToList();
            await using (var stream = new FileStream(_tempPath, FileMode.Create, FileAccess.Write, FileShare.None,
                16 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                await JsonSerializer.SerializeAsync(stream, entries, FaceitJson.Options, token).ConfigureAwait(false);
                await stream.FlushAsync(token).ConfigureAwait(false);
            }
            token.ThrowIfCancellationRequested();
            File.Move(_tempPath, _path, true);
            lock (_dataGate) _savedVersion = version;
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { throw; }
        catch (Exception ex)
        {
            // Do not acknowledge this version: the next maintenance/save retries it.
            if (_debug())
                _logger.LogWarning(ex, "[CS2FaceitLevels] Failed to write cache file {Path}.", _path);
        }
        finally
        {
            if (acquired)
            {
                try { File.Delete(_tempPath); } catch (IOException) { } catch (UnauthorizedAccessException) { }
                FileLock.Release();
            }
        }
    }

    internal async Task Stop(Task pendingWork, TimeSpan timeout)
    {
        Task maintenance;
        lock (_maintenanceGate)
        {
            _closed = true;
            maintenance = _maintenance;
        }
        lock (_dataGate) _acceptWrites = false;
        _ioStop.CancelAfter(timeout);
        var drain = Task.WhenAll(pendingWork, Ready, maintenance);
        try
        {
            await drain.WaitAsync(_ioStop.Token).ConfigureAwait(false);
            // A clean unload saves changes accepted before cancellation, even if
            // the 60-second maintenance timer has not fired yet.
            await Flush(_ioStop.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_ioStop.IsCancellationRequested)
        {
            _logger.LogWarning("[CS2FaceitLevels] Shutdown cache save exceeded its time limit; the previous cache file is retained.");
        }
        finally
        {
            _ioStop.Cancel();
            _ = drain.ContinueWith(_ => _ioStop.Dispose(), CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }

    // Keep cache.json field names, supported levels and UTC expiry semantics.
    private sealed record PersistentEntry(ulong SteamId, int Level, int? Elo, DateTime ExpiresAt);
}
