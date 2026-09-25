namespace CS2FaceitLevels.Workshop;

// AppDomain data holds only a CoreLib string, never an object from the unloadable
// plugin assembly. This survives CSS's assembly reload without retaining old hooks.
// It is process-local and consumed once; no per-connection disk I/O is needed.
internal static class WorkshopReloadBridge
{
    private static string Key(string directory) => "CS2FaceitLevels.Workshop.Reload:" + Path.GetFullPath(directory);

    public static void Save(string directory, string? state) => AppDomain.CurrentDomain.SetData(Key(directory), state);

    public static string? Take(string directory, bool hotReload)
    {
        string? state = AppDomain.CurrentDomain.GetData(Key(directory)) as string;
        AppDomain.CurrentDomain.SetData(Key(directory), null);
        return hotReload ? state : null;
    }
}
