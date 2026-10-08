using System.Runtime.InteropServices;

namespace CS2FaceitLevels.Workshop;

// Read slot/server offsets from gamedata; get the Steam ID through GetClientXUID.
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
        // Zero means this slot has no connected account yet.
        if (steamId == 0) return (0, slot);
        // Accept a public desktop Steam account with a nonzero account number.
        if ((steamId >> 32) != 0x01100001UL || (uint)steamId == 0)
            throw new InvalidOperationException($"Engine GetClientXUID returned an invalid Steam ID format for slot {slot}.");
        return (steamId, slot);
    }
}
