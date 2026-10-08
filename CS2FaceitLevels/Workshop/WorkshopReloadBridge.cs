namespace CS2FaceitLevels.Workshop;

// Save strings in AppDomain so reload state survives without keeping the old assembly alive.
internal static class WorkshopReloadBridge
{
    private static string Key(string directory) => "CS2FaceitLevels.Workshop.Reload:" + Path.GetFullPath(directory);
    private static string MamKey(string directory) => "CS2FaceitLevels.Workshop.MamOwned:" + Path.GetFullPath(directory);

    public static void Save(string directory, string? state) => AppDomain.CurrentDomain.SetData(Key(directory), state);

    public static string? Take(string directory, bool hotReload)
    {
        string? state = AppDomain.CurrentDomain.GetData(Key(directory)) as string;
        AppDomain.CurrentDomain.SetData(Key(directory), null);
        return hotReload ? state : null;
    }

    public static void SaveMamOwnership(string directory, bool owned) =>
        AppDomain.CurrentDomain.SetData(MamKey(directory), owned ? "1" : null);

    public static bool TakeMamOwnership(string directory, bool hotReload)
    {
        bool owned = AppDomain.CurrentDomain.GetData(MamKey(directory)) is "1";
        AppDomain.CurrentDomain.SetData(MamKey(directory), null);
        return hotReload && owned;
    }
}
