using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

// Owns the lifetime of accepted jobs. Work never waits for a game-thread callback.
internal sealed class BackgroundWork(Func<bool> debug, ILogger logger)
{
    private readonly object _gate = new();
    private readonly HashSet<Task> _tasks = [];
    private readonly CancellationTokenSource _stop = new();
    private bool _closed;

    internal bool Stopping => _stop.IsCancellationRequested;

    // Captured by services during Load, before shutdown starts.
    internal CancellationToken LifetimeToken => _stop.Token;

    internal void Run(Func<Task> action, ulong steamId = 0)
    {
        lock (_gate)
        {
            if (_closed) return;
            var task = Task.Run(async () =>
            {
                try { await action().ConfigureAwait(false); }
                catch (OperationCanceledException) when (Stopping) { }
                catch (Exception ex)
                {
                    if (debug())
                        logger.LogWarning(ex, "[CS2FaceitLevels] Background FACEIT work failed for {SteamId}.", steamId);
                }
            });
            _tasks.Add(task);
            _ = task.ContinueWith(completed =>
            {
                lock (_gate) _tasks.Remove(completed);
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }

    internal Task Stop()
    {
        Task[] tasks;
        lock (_gate)
        {
            _closed = true;
            tasks = _tasks.ToArray();
        }
        _stop.Cancel();
        return Task.WhenAll(tasks);
    }

    internal void DisposeAfter(Task completion)
    {
        _ = completion.ContinueWith(_ => _stop.Dispose(), CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }
}
