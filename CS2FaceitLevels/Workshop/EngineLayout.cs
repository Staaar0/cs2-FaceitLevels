using System.Text.Json;
using System.Text.Json.Serialization;

namespace CS2FaceitLevels.Workshop;

internal sealed class EngineLayout
{
    [JsonIgnore]
    public string Platform { get; set; } = "";
    [JsonRequired]
    public string ReplyConnectionSignature { get; set; } = "";
    [JsonRequired]
    public string ClientVTable { get; set; } = "";
    [JsonRequired]
    public int SendNetMessageIndex { get; set; }
    [JsonRequired]
    public string SendServerInfoSignature { get; set; } = "";
    [JsonRequired]
    public int SendServerInfoIndex { get; set; }
    [JsonRequired]
    public string ServerVTable { get; set; } = "";
    [JsonRequired]
    public int FillServerInfoIndex { get; set; }
    [JsonRequired]
    public int ServerAddonsOffset { get; set; }
    [JsonRequired]
    public string HostStateRequestSignature { get; set; } = "";
    [JsonRequired]
    public int HostStateRequestAddonsOffset { get; set; }
    [JsonRequired]
    public int ClientSlotOffset { get; set; }
    [JsonRequired]
    public string EngineInterface { get; set; } = "";
    [JsonRequired]
    public int GetClientXuidIndex { get; set; }
    [JsonRequired]
    public int GetAppIdIndex { get; set; }
    [JsonRequired]
    public int ClientServerOffset { get; set; }
    [JsonRequired]
    public int UserMessagePayloadOffset { get; set; }

    private sealed class Gamedata
    {
        [JsonRequired]
        public Dictionary<string, EngineLayout> Platforms { get; set; } = new();
    }

    public static EngineLayout Read(string directory)
    {
        if ((!OperatingSystem.IsLinux() && !OperatingSystem.IsWindows()) || System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture !=
            System.Runtime.InteropServices.Architecture.X64)
            throw new PlatformNotSupportedException("The Workshop loader requires Linux x64 or Windows x64.");
        return Parse(File.ReadAllText(Path.Combine(directory, "workshop.gamedata.json")),
            OperatingSystem.IsWindows() ? "windows" : "linux");
    }

    internal static EngineLayout Parse(string json, string platform)
    {
        Gamedata gamedata;
        try
        {
            gamedata = JsonSerializer.Deserialize<Gamedata>(json)
                ?? throw new InvalidOperationException("Empty Workshop gamedata.");
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("Incomplete or invalid Workshop gamedata. Replace workshop.gamedata.json together with the plugin DLL.", ex);
        }
        if (platform is not ("linux" or "windows") ||
            gamedata.Platforms == null || !gamedata.Platforms.TryGetValue(platform, out var data) || data == null ||
            !ValidSignature(data.ReplyConnectionSignature) || !ValidSignature(data.SendServerInfoSignature) ||
            !ValidSignature(data.HostStateRequestSignature) ||
            string.IsNullOrWhiteSpace(data.ClientVTable) || data.SendServerInfoIndex is < 1 or > 128 ||
            string.IsNullOrWhiteSpace(data.ServerVTable) || data.FillServerInfoIndex is < 1 or > 128 ||
            data.SendNetMessageIndex is < 1 or > 64 || data.UserMessagePayloadOffset != 0 ||
            !ValidOffset(data.ServerAddonsOffset) || !ValidOffset(data.HostStateRequestAddonsOffset) ||
            data.HostStateRequestAddonsOffset != 88 || !ValidOffset(data.ClientSlotOffset) ||
            !ValidOffset(data.ClientServerOffset) || data.EngineInterface != "Source2EngineToServer001" ||
            data.GetClientXuidIndex is < 1 or > 200 || data.GetAppIdIndex is < 1 or > 200)
            throw new InvalidOperationException("Unsupported Workshop gamedata layout. Replace workshop.gamedata.json together with the plugin DLL.");
        data.Platform = platform;
        return data;
    }

    private static bool ValidOffset(int offset) => offset is >= 0 and <= 4096 && offset % 4 == 0;
    private static bool ValidSignature(string value) => !string.IsNullOrWhiteSpace(value) &&
        value.Split(' ', StringSplitOptions.RemoveEmptyEntries).All(token => token is "?" or "??" ||
            (token.Length == 2 && byte.TryParse(token, System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture, out _)));
}
