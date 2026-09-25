using System.Runtime.InteropServices;

namespace CS2FaceitLevels.Workshop;

// Only slot/server fields use native layout data. The identity comes from the
// engine's GetClientXUID getter, never from a calculated CSteamID field offset.
internal static class ClientIdentityReader
{
    public static (ulong SteamId, int Slot) Read(nint client, nint server, EngineLayout layout,
        Func<int, ulong> getClientXuid)
    {
        if (client == nint.Zero || server == nint.Zero ||
            Marshal.ReadIntPtr(client, layout.ClientServerOffset) != server)
            throw new InvalidOperationException("Client/server memory layout does not match Workshop gamedata.");
        int slot = Marshal.ReadInt32(client, layout.ClientSlotOffset);
        if (slot is < 0 or >= 256)
            throw new InvalidOperationException($"Client slot {slot} is invalid at Workshop gamedata offset {layout.ClientSlotOffset}.");
        ulong steamId = getClientXuid(slot);
        // The engine returns zero when there is no connected account for this slot.
        if (steamId == 0) return (0, slot);
        // Public-universe individual Steam account, desktop instance, nonzero account number.
        if ((steamId >> 32) != 0x01100001UL || (uint)steamId == 0)
            throw new InvalidOperationException($"Engine GetClientXUID returned an invalid Steam ID format for slot {slot}.");
        return (steamId, slot);
    }
}
