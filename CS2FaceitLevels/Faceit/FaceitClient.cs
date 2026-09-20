using System.Buffers;
using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

// HTTP-only: never reads game entities or schedules game-native work.
internal sealed class FaceitClient(Func<CS2FaceitLevelsConfig> getConfig, ILogger logger)
{
    private CS2FaceitLevelsConfig Config => getConfig();
    private ILogger Logger => logger;
    private const string DefaultApiKey = "PUT_YOUR_FACEIT_API_KEY_HERE";
    private const int MaxResponseBytes = 1024 * 1024;
    private const int MaxRequestAttempts = 3;
    private const int MinTimeoutSeconds = 2, MaxTimeoutSeconds = 60;
    private const int MinCacheMinutes = 1, MaxCacheMinutes = 1440;
    private static readonly HttpClient Http = new() { MaxResponseContentBufferSize = MaxResponseBytes };
    private static readonly SemaphoreSlim HttpSlots = new(4, 4);
    private long _requestBlockedUntilTicks;

    internal async Task<FaceitData> Fetch(ulong steamId, CancellationToken token)
    {
        if (string.IsNullOrWhiteSpace(Config.FaceitApiKey) || Config.FaceitApiKey == DefaultApiKey)
        {
            if (Config.Debug)
                Logger.LogWarning("[CS2FaceitLevels] No FACEIT API key set in the config.");

            return NoFaceit();
        }

        var player = await GetJson<FaceitPlayer>(
            $"https://open.faceit.com/data/v4/players?game=cs2&game_player_id={steamId}", token);
        var cs2 = player?.Games?.Cs2;

        if (cs2?.SkillLevel is not (>= 1 and <= 10))
            return NoFaceit();

        var level = cs2.SkillLevel.Value;

        if (level == 10 && !string.IsNullOrEmpty(player!.PlayerId) && !string.IsNullOrEmpty(cs2.Region)
            && await IsChallenger(player.PlayerId!, cs2.Region!, token))
        {
            level = 11;
        }

        return new FaceitData(level, cs2.Elo, DateTime.UtcNow.AddMinutes(CacheMinutes()));
    }

    private async Task<bool> IsChallenger(string playerId, string region, CancellationToken token)
    {
        var url = $"https://open.faceit.com/data/v4/rankings/games/cs2/regions/{Uri.EscapeDataString(region)}/players/{Uri.EscapeDataString(playerId)}";
        var ranking = await GetJson<FaceitRanking>(url, token);
        var position = ranking?.Position ?? ranking?.Items?.FirstOrDefault()?.Position ?? 0;
        return position is > 0 and <= 1000;
    }

    private async Task<T?> GetJson<T>(string url, CancellationToken token) where T : class
    {
        ThrowIfRequestBackedOff();

        for (var attempt = 1; attempt <= MaxRequestAttempts; attempt++)
        {
            try
            {
                await HttpSlots.WaitAsync(token);
                try
                {
                    ThrowIfRequestBackedOff();

                    using var request = new HttpRequestMessage(HttpMethod.Get, url);
                    request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", Config.FaceitApiKey);

                    using var cts = CancellationTokenSource.CreateLinkedTokenSource(token);
                    cts.CancelAfter(TimeSpan.FromSeconds(TimeoutSeconds()));

                    using var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cts.Token);

                    if (response.StatusCode == HttpStatusCode.NotFound)
                        return null;

                    if (response.StatusCode == HttpStatusCode.TooManyRequests)
                    {
                        var retryAfter = response.Headers.RetryAfter?.Delta;
                        if (retryAfter == null && response.Headers.RetryAfter?.Date is { } retryDate)
                            retryAfter = retryDate - DateTimeOffset.UtcNow;

                        var delay = retryAfter ?? TimeSpan.FromMinutes(5);
                        SetRequestBackoff(delay < TimeSpan.FromSeconds(30) ? TimeSpan.FromSeconds(30) : delay);
                        throw new FaceitApiException("FACEIT API rate limit reached.");
                    }

                    if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden)
                    {
                        SetRequestBackoff(TimeSpan.FromMinutes(10));
                        throw new FaceitApiException($"FACEIT API authorization failed with status {(int)response.StatusCode}.");
                    }

                    if ((int)response.StatusCode >= 500)
                    {
                        if (attempt == MaxRequestAttempts)
                        {
                            SetRequestBackoff(TimeSpan.FromSeconds(60));
                            throw new FaceitApiException($"FACEIT API returned status {(int)response.StatusCode} after retries.");
                        }
                    }
                    else
                    {
                        if (!response.IsSuccessStatusCode)
                            throw new FaceitApiException($"FACEIT API returned status {(int)response.StatusCode}.");

                        try
                        {
                            return await ReadJsonAsync<T>(response, cts.Token);
                        }
                        catch (JsonException ex)
                        {
                            throw new FaceitApiException("FACEIT API returned malformed JSON.", ex);
                        }
                    }
                }
                finally
                {
                    HttpSlots.Release();
                }
            }
            catch (FaceitApiException)
            {
                throw;
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested)
            {
                throw new FaceitApiException("FACEIT API request cancelled.");
            }
            catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or OperationCanceledException or IOException)
            {
                if (attempt == MaxRequestAttempts)
                {
                    SetRequestBackoff(TimeSpan.FromSeconds(60));
                    throw new FaceitApiException("FACEIT API request failed after retries.", ex);
                }
            }

            await Task.Delay(TimeSpan.FromSeconds(attempt), token);
        }

        throw new FaceitApiException("FACEIT API request failed.");
    }

    private int CacheMinutes() => Math.Clamp(Config.CacheMinutes, MinCacheMinutes, MaxCacheMinutes);

    private int TimeoutSeconds() => Math.Clamp(Config.RequestTimeoutSeconds, MinTimeoutSeconds, MaxTimeoutSeconds);

    private void ThrowIfRequestBackedOff()
    {
        var blockedUntil = new DateTime(Volatile.Read(ref _requestBlockedUntilTicks), DateTimeKind.Utc);
        if (blockedUntil > DateTime.UtcNow)
            throw new FaceitApiException("FACEIT API requests are temporarily backed off.");
    }

    private void SetRequestBackoff(TimeSpan duration)
    {
        var until = DateTime.UtcNow.Add(duration).Ticks;
        var current = Volatile.Read(ref _requestBlockedUntilTicks);

        while (until > current)
        {
            var observed = Interlocked.CompareExchange(ref _requestBlockedUntilTicks, until, current);
            if (observed == current)
                break;

            current = observed;
        }
    }

    private FaceitData NoFaceit() => new(0, null, DateTime.UtcNow.AddMinutes(CacheMinutes()));

    private static async Task<T?> ReadJsonAsync<T>(HttpResponseMessage response, CancellationToken token)
    {
        if (response.Content.Headers.ContentLength is > MaxResponseBytes)
            throw new FaceitApiException("FACEIT API response exceeded the size limit.");

        await using var stream = await response.Content.ReadAsStreamAsync(token);
        var buffer = ArrayPool<byte>.Shared.Rent(8192);
        var length = 0;
        try
        {
            while (true)
            {
                if (length == MaxResponseBytes)
                {
                    // Probe EOF without allocating or accepting a byte past the limit.
                    if (await stream.ReadAsync(buffer.AsMemory(0, 1), token) != 0)
                        throw new FaceitApiException("FACEIT API response exceeded the size limit.");
                    break;
                }
                if (length == buffer.Length)
                {
                    var larger = ArrayPool<byte>.Shared.Rent(Math.Min(buffer.Length * 2, MaxResponseBytes));
                    buffer.AsSpan(0, length).CopyTo(larger);
                    ArrayPool<byte>.Shared.Return(buffer);
                    buffer = larger;
                }
                var read = await stream.ReadAsync(buffer.AsMemory(length,
                    Math.Min(buffer.Length - length, MaxResponseBytes - length)), token);
                if (read == 0) break;
                length += read;
            }
            return JsonSerializer.Deserialize<T>(buffer.AsSpan(0, length), FaceitJson.Options);
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    private sealed class FaceitApiException : Exception
    {
        public FaceitApiException(string message) : base(message) { }
        public FaceitApiException(string message, Exception innerException) : base(message, innerException) { }
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
