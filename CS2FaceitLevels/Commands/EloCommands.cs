using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

internal sealed class EloCommands(PlayerSessions sessions, FaceitLookup lookup, BackgroundWork work,
    Func<ChatFormatter> formatter, Func<bool> debug, ILogger logger)
{
    private const long CooldownMs = 10_000;
    private const int MaxLines = 32;

    // Preparing methods compiles their managed code without calling game natives.
    // The sample below also initializes the formatting paths used by both commands.
    internal void Prewarm(ChatFormatter chat)
    {
        const BindingFlags instance = BindingFlags.Instance | BindingFlags.NonPublic;
        const BindingFlags statics = BindingFlags.Static | BindingFlags.NonPublic;
        foreach (var name in new[] { nameof(Single), nameof(All) })
            RuntimeHelpers.PrepareMethod(typeof(EloCommands).GetMethod(name, instance)!.MethodHandle);
        foreach (var name in new[] { nameof(CanUse), nameof(SnapshotPlayers), nameof(JoinArgs) })
            RuntimeHelpers.PrepareMethod(typeof(EloCommands).GetMethod(name, statics)!.MethodHandle);

        var sample = new FaceitData(1, 100, DateTime.UtcNow);
        _ = chat.SingleLine("Player", 0, sample);
        _ = chat.AllLine("Player", 0, sample);
        _ = chat.PlayerOnly();
        _ = chat.MissingName();
        _ = chat.NoMatch("Player");
        _ = chat.Multiple("Player");
    }

    internal void Single(CCSPlayerController? caller, CommandInfo command)
    {
        var chat = formatter();
        if (!PlayerAccess.TryIdentity(caller, out var callerId))
        {
            command.ReplyToCommand(chat.PlayerOnly());
            return;
        }
        var session = sessions.GetOrAdd(caller.Slot, callerId);
        if (!CanUse(session)) return;
        var search = JoinArgs(command);
        if (search.Length == 0)
        {
            caller.PrintToChat(chat.MissingName());
            return;
        }
        var matches = SnapshotPlayers(includeTeam: false)
            .Where(p => p.Name.Contains(search, StringComparison.OrdinalIgnoreCase))
            .OrderBy(p => p.Name).ToList();
        if (matches.Count == 0)
        {
            caller.PrintToChat(chat.NoMatch(search));
            return;
        }
        if (matches.Count > 1)
        {
            caller.PrintToChat(chat.Multiple(string.Join(", ", matches.Take(5).Select(p => p.Name))));
            return;
        }
        var target = matches[0];
        session.CommandPending = true;
        work.Run(async () =>
        {
            string? line = null;
            try
            {
                if (!session.Active || work.Stopping) return;
                var data = await lookup.Get(target.SteamId).ConfigureAwait(false);
                if (!session.Active || work.Stopping) return;
                line = formatter().SingleLine(target.Name, target.SteamId, data);
            }
            catch (OperationCanceledException) when (work.Stopping) { return; }
            catch (Exception ex) { Log(ex, target.SteamId); }
            if (!session.Active || work.Stopping) return;
            Server.NextFrame(() =>
            {
                if (work.Stopping || !sessions.IsCurrent(session)) return;
                session.CommandPending = false;
                if (line != null && sessions.TryResolve(session, out var current)) current.PrintToChat(line);
            });
        }, target.SteamId);
    }

    internal void All(CCSPlayerController? caller, CommandInfo command)
    {
        if (!PlayerAccess.TryIdentity(caller, out var callerId))
        {
            command.ReplyToCommand(formatter().PlayerOnly());
            return;
        }
        var session = sessions.GetOrAdd(caller.Slot, callerId);
        if (!CanUse(session)) return;
        // Capture native fields once, then filter/sort only managed values.
        var targets = SnapshotPlayers(includeTeam: true).OrderBy(p => p.Team).ThenBy(p => p.Name)
            .Take(MaxLines).ToArray();
        session.CommandPending = true;
        work.Run(async () =>
        {
            var lines = new List<string>(targets.Length);
            try
            {
                foreach (var target in targets)
                {
                    if (!session.Active || work.Stopping) return;
                    var data = await lookup.Get(target.SteamId).ConfigureAwait(false);
                    if (!session.Active || work.Stopping) return;
                    lines.Add(formatter().AllLine(target.Name, target.SteamId, data));
                }
            }
            catch (OperationCanceledException) when (work.Stopping) { return; }
            catch (Exception ex) { Log(ex, session.SteamId); }
            if (!session.Active || work.Stopping) return;
            Server.NextFrame(() =>
            {
                if (work.Stopping || !sessions.IsCurrent(session)) return;
                session.CommandPending = false;
                if (lines.Count == 0 || !sessions.TryResolve(session, out var current)) return;
                foreach (var line in lines) current.PrintToChat(line);
            });
        }, session.SteamId);
    }

    private static bool CanUse(PlayerSession session)
    {
        if (session.CommandPending) return false;
        var now = Environment.TickCount64;
        if (session.LastCommandTime is { } last && now - last < CooldownMs) return false;
        session.LastCommandTime = now;
        return true;
    }

    private static List<PlayerSnapshot> SnapshotPlayers(bool includeTeam)
    {
        var result = new List<PlayerSnapshot>(64);
        foreach (var player in Utilities.GetPlayers())
            if (PlayerAccess.TryIdentity(player, out var steamId))
                result.Add(new PlayerSnapshot(steamId, player.PlayerName, includeTeam ? player.TeamNum : (byte)0));
        return result;
    }

    private static string JoinArgs(CommandInfo command)
    {
        var result = new StringBuilder();
        var count = command.ArgCount;
        for (var i = 1; i < count; i++)
        {
            var arg = command.ArgByIndex(i);
            if (string.IsNullOrWhiteSpace(arg)) continue;
            if (result.Length != 0) result.Append(' ');
            result.Append(arg);
        }
        return result.ToString();
    }

    private void Log(Exception ex, ulong steamId)
    {
        if (debug()) logger.LogWarning(ex, "[CS2FaceitLevels] Background FACEIT work failed for {SteamId}.", steamId);
    }

    private readonly record struct PlayerSnapshot(ulong SteamId, string Name, byte Team);
}
