using CounterStrikeSharp.API.Modules.Utils;

namespace CS2FaceitLevels;

internal sealed class ChatFormatter
{
    private readonly CS2FaceitLevelsLang _lang;
    private readonly string[] _single = new string[11];
    private readonly string[] _all = new string[11];

    internal ChatFormatter(CS2FaceitLevelsLang lang)
    {
        _lang = lang;
        for (var skill = 0; skill <= 10; skill++)
        {
            _single[skill] = Prepare(lang.SingleEloChatFormat, skill);
            _all[skill] = Prepare(lang.AllElosChatFormat, skill);
        }
    }

    internal string PlayerOnly() => Format(_lang.PlayerOnlyMessage);
    internal string MissingName() => Format(_lang.MissingPlayerNameMessage);
    internal string NoMatch(string search) => Format(_lang.NoPlayerFoundMessage, ("SEARCH", search));
    internal string Multiple(string names) => Format(_lang.MultiplePlayersFoundMessage, ("PLAYERS", names));
    internal string SingleLine(string name, ulong steamId, FaceitData data) => EloLine(_single[data.SkillLevel], name, steamId, data);
    internal string AllLine(string name, ulong steamId, FaceitData data) => EloLine(_all[data.SkillLevel], name, steamId, data);

    private string Prepare(string template, int skill) => template
        .Replace("{PREFIX}", _lang.ChatPrefix, StringComparison.OrdinalIgnoreCase)
        .Replace("{PLAYER_COLOR}", "{RED}", StringComparison.OrdinalIgnoreCase)
        .Replace("{LABEL_COLOR}", "{LIGHTPURPLE}", StringComparison.OrdinalIgnoreCase)
        .Replace("{ELO_COLOR}", EloColor(skill), StringComparison.OrdinalIgnoreCase);

    // Keep dynamic substitution order and color processing after player-name insertion.
    private static string EloLine(string template, string playerName, ulong steamId, FaceitData data)
    {
        var message = template
            .Replace("{PLAYER}", playerName, StringComparison.OrdinalIgnoreCase)
            .Replace("{STEAMID64}", steamId.ToString(), StringComparison.OrdinalIgnoreCase)
            .Replace("{ELO}", data.Elo?.ToString() ?? "N/A", StringComparison.OrdinalIgnoreCase)
            .Replace("{LEVEL}", data.SkillLevel > 0 ? data.SkillLevel.ToString() : "N/A", StringComparison.OrdinalIgnoreCase);
        return ApplyColors(message);
    }

    private static readonly (string Tag, string Value)[] Colors = new Dictionary<string, char>(StringComparer.OrdinalIgnoreCase)
    {
        ["default"] = ChatColors.Default, ["white"] = ChatColors.White,
        ["darkred"] = ChatColors.DarkRed, ["red"] = ChatColors.Red, ["lightred"] = ChatColors.LightRed,
        ["green"] = ChatColors.Green, ["lime"] = ChatColors.Lime, ["olive"] = ChatColors.Olive,
        ["yellow"] = ChatColors.Yellow, ["lightyellow"] = ChatColors.LightYellow, ["gold"] = ChatColors.Gold,
        ["orange"] = ChatColors.Orange, ["blue"] = ChatColors.Blue, ["darkblue"] = ChatColors.DarkBlue,
        ["lightblue"] = ChatColors.LightBlue, ["purple"] = ChatColors.Purple, ["lightpurple"] = ChatColors.LightPurple,
        ["grey"] = ChatColors.Grey, ["gray"] = ChatColors.Grey, ["silver"] = ChatColors.Silver,
        ["magenta"] = ChatColors.Magenta, ["bluegrey"] = ChatColors.BlueGrey,
    }.Select(p => ("{" + p.Key + "}", p.Value.ToString())).ToArray();

    private string Format(string template, params (string Key, string Value)[] replacements)
    {
        var message = template.Replace("{PREFIX}", _lang.ChatPrefix, StringComparison.OrdinalIgnoreCase);

        foreach (var (key, value) in replacements)
            message = message.Replace("{" + key + "}", value, StringComparison.OrdinalIgnoreCase);

        return ApplyColors(message);
    }

    private static string EloColor(int skillLevel) => skillLevel switch
    {
        1 => "{GREY}",
        2 or 3 => "{LIME}",
        >= 4 and <= 7 => "{YELLOW}",
        8 or 9 => "{ORANGE}",
        10 => "{RED}",
        _ => "{GREY}",
    };

    private static string ApplyColors(string message)
    {
        foreach (var (tag, color) in Colors)
            message = message.Replace(tag, color, StringComparison.OrdinalIgnoreCase);

        return message;
    }

}
