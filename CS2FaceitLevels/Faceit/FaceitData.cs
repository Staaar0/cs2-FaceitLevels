using System.Text.Json;

namespace CS2FaceitLevels;

// Levels: -1 = lookup failed, 0 = no FACEIT account, 11 = Challenger.
internal sealed record FaceitData(int Level, int? Elo, DateTime ExpiresAt)
{
    internal int SkillLevel => Level <= 0 ? 0 : Math.Min(Level, 10);
    internal static readonly FaceitData RequestFailed = new(-1, null, DateTime.MinValue);
}

internal static class FaceitJson
{
    internal static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };
}
