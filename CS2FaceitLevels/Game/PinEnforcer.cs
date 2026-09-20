using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Entities.Constants;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

internal sealed class PinEnforcer(PlayerSessions sessions, Func<CS2FaceitLevelsConfig> getConfig, ILogger logger)
{
    private const int RankIndex = 5;

    // Called at the original OnTick frequency. The controller/inventory is
    // resolved afresh; no native handle survives a frame or reconnect.
    internal void Enforce()
    {
        var players = sessions.Active;
        for (var i = 0; i < players.Count; i++)
        {
            var session = players[i];
            if (!session.EnforcePin) continue;
            var player = Utilities.GetPlayerFromSlot(session.Slot);
            if (!PlayerAccess.TryIdentity(player, out var steamId, connected: true) || steamId != session.SteamId ||
                player.InventoryServices is not { } inventory) continue;
            var ranks = inventory.Rank;
            if (ranks.Length <= RankIndex) continue;
            var current = ranks[RankIndex];
            if (session.DesiredPin is { } desired)
            {
                if (current == desired) continue;
                ranks[RankIndex] = desired;
            }
            else
            {
                // Retain cleanup of FACEIT-mapped pins on players with no assignment.
                if (!IsFaceitPin(current)) continue;
                ranks[RankIndex] = MedalRank_t.MEDAL_RANK_NONE;
            }
            Utilities.SetStateChanged(player, "CCSPlayerController", "m_pInventoryServices");
        }
    }

    internal void Apply(PlayerSession session, CCSPlayerController player, FaceitData data)
    {
        if (data.Level < 0 || player.InventoryServices is not { } inventory) return;
        var ranks = inventory.Rank;
        if (ranks.Length <= RankIndex) return;
        var pin = LevelPin(data.Level);
        if (pin == null && getConfig().ClearPinWhenNoFaceit) pin = MedalRank_t.MEDAL_RANK_NONE;
        if (pin is not { } desired) return;
        session.DesiredPin = desired;
        if (ranks[RankIndex] != desired)
        {
            ranks[RankIndex] = desired;
            Utilities.SetStateChanged(player, "CCSPlayerController", "m_pInventoryServices");
        }
        if (getConfig().Debug)
            logger.LogInformation("[CS2FaceitLevels] Updated scoreboard pin for {Name} (level {Level}).", player.PlayerName, data.Level);
    }

    private static MedalRank_t? LevelPin(int level) => level switch
    {
        1 => (MedalRank_t)1017, 2 => (MedalRank_t)1032, 3 => (MedalRank_t)1019,
        4 => (MedalRank_t)1005, 5 => (MedalRank_t)1051, 6 => (MedalRank_t)1007,
        7 => (MedalRank_t)1020, 8 => (MedalRank_t)1082, 9 => (MedalRank_t)1035,
        10 => (MedalRank_t)1060, 11 => (MedalRank_t)1010, _ => null,
    };

    private static bool IsFaceitPin(MedalRank_t rank) => (int)rank is
        1017 or 1032 or 1019 or 1005 or 1051 or 1007 or 1020 or 1082 or 1035 or 1060 or 1010;
}
