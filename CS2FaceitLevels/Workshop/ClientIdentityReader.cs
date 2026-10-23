using System.Runtime.InteropServices;

namespace CS2FaceitLevels.Workshop;

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
        if (steamId == 0) return (0, slot);
        if ((steamId >> 32) != 0x01100001UL || (uint)steamId == 0)
            throw new InvalidOperationException($"Engine GetClientXUID returned an invalid Steam ID format for slot {slot}.");
        return (steamId, slot);
    }
}
