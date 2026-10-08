using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Cvars;
using CounterStrikeSharp.API.Modules.Memory;
using CounterStrikeSharp.API.Modules.Memory.DynamicFunctions;
using CounterStrikeSharp.API.Modules.UserMessages;
using CounterStrikeSharp.API.ValveConstants.Protobuf;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels.Workshop;

// x64 bridge. No separate Metamod addon or helper DLL is loaded.
// The protocol follows Source2ZE/MultiAddonManager (GPL-3.0); see README.md.
internal sealed class WorkshopLoader : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void SetUtlStringDirect(nint value, nint utf8, int length);

    public const string AddonId = "3724637448";
    private readonly string _directory;
    private readonly ILogger _log;
    private readonly Func<bool> _debug;
    private readonly AddonHandshake _handshakes = new();
    private readonly Dictionary<int, ulong> _slots = new();
    private readonly ConnectionDeadlines _deadlines = new();
    private readonly SendHookSchedule _sendSchedule = new();
    private System.Threading.Timer? _deadlineTimer;
    private int _waitingCount, _deadlineCheckQueued;
    private readonly int _gameThread = Environment.CurrentManagedThreadId;
    private readonly Func<DynamicHook, HookResult> _replyHandler;
    private readonly Func<DynamicHook, HookResult> _sendHandler;
    private readonly Func<DynamicHook, HookResult> _fillServerInfoHandler;
    private readonly Func<DynamicHook, HookResult> _hostRequestHandler;
    private readonly Func<int, ulong> _getClientXuid;
    private FunctionReference? _replyReference, _sendReference, _fillServerInfoReference, _hostRequestReference;
    private EngineLayout _layout = null!;
    private nint _replyFunction, _sendFunction, _fillServerInfoFunction, _hostRequestFunction, _signonVTable, _serverInfoVTable;
    private nint _engineSignonVTable, _engineServerInfoVTable;
    private nint _tier0Module;
    private SetUtlStringDirect? _setUtlString;
    private nint _engineInterface, _getXuidFunction;
    private UserMessage? _message, _serverInfo;
    private bool _replyHooked, _sendHooked, _fillServerInfoHooked, _hostRequestHooked, _insideReply, _insideMessage;
    private volatile bool _disposed;
    private volatile bool _faulted;
    private bool _started;
    private long _replies, _signons, _completed, _mountLists, _mapRequests;
    private long _missingXuids, _timedOut, _sendAttaches;

    public string Status { get; private set; } = "Not started";
    public bool CanRetryStartup => !_disposed && _faulted && !_started;
    public string Summary => $"{Status}; addon={AddonId}; replies={_replies}; signons={_signons}; " +
        $"joined={_completed}; tracked={_handshakes.Count}; missing_xuids={_missingXuids}; " +
        $"waiting={_deadlines.Count}; timed_out={_timedOut}; mount_lists={_mountLists}; map_requests={_mapRequests}; " +
        $"send_hook={(_sendHooked ? "attached" : "idle")}; send_attaches={_sendAttaches}";
    private static double Now => (double)Stopwatch.GetTimestamp() / Stopwatch.Frequency;

    public WorkshopLoader(string directory, ILogger log, Func<bool> debug)
    {
        _directory = directory;
        _log = log;
        _debug = debug;
        _replyHandler = OnReply;
        _sendHandler = OnSendMessage;
        _fillServerInfoHandler = OnFillServerInfo;
        _hostRequestHandler = OnHostRequest;
        _getClientXuid = GetClientXuid;
    }

    public void Start()
    {
        if (_disposed || _faulted) return;
        if (MamHasBadge())
        {
            // MAM may load later. Stop our hooks once it takes over the badge.
            if (_replyHooked) Detach();
            if (Status != "Managed by MultiAddonManager")
            {
                Status = "Managed by MultiAddonManager";
                _log.LogInformation("[CS2FaceitLevels] Addon has been loaded from MultiAddonManager.");
            }
            return;
        }
        if (_replyHooked) return;
        try
        {
            _layout = EngineLayout.Read(_directory);
            var engine = Addresses.EnginePath;
            // GetClientSteamID may be null this early; use the connection-time XUID API.
            _engineInterface = NativeAPI.GetValveInterface(0, _layout.EngineInterface);
            if (_engineInterface == nint.Zero) throw new InvalidOperationException("The engine server interface was not found.");
            var engineVTable = Marshal.ReadIntPtr(_engineInterface);
            if (engineVTable == nint.Zero) throw new InvalidOperationException("The engine server interface vtable is null.");
            var getAppId = NativeAPI.CreateVirtualFunctionFromVTable(engineVTable, _layout.GetAppIdIndex,
                1, (int)DataType.DATA_TYPE_UINT, new object[] { (int)DataType.DATA_TYPE_POINTER });
            if (getAppId == nint.Zero || NativeAPI.ExecuteVirtualFunction<uint>(getAppId, false,
                    new object[] { _engineInterface }) != 730)
                throw new InvalidOperationException("The engine server interface did not report CS2 App ID 730.");
            _getXuidFunction = NativeAPI.CreateVirtualFunctionFromVTable(engineVTable, _layout.GetClientXuidIndex,
                2, (int)DataType.DATA_TYPE_ULONG_LONG,
                new object[] { (int)DataType.DATA_TYPE_POINTER, (int)DataType.DATA_TYPE_INT });
            if (_getXuidFunction == nint.Zero) throw new InvalidOperationException("The engine GetClientXUID function could not be created.");
            var vtable = NativeAPI.FindVirtualTable(engine, _layout.ClientVTable);
            if (vtable == nint.Zero) throw new InvalidOperationException("CServerSideClient vtable was not found.");
            // Check the gamedata signature against the client vtable.
            var sendServerInfo = NativeAPI.FindSignature(engine, _layout.SendServerInfoSignature);
            if (sendServerInfo == nint.Zero ||
                Marshal.ReadIntPtr(vtable, _layout.SendServerInfoIndex * IntPtr.Size) != sendServerInfo)
                throw new InvalidOperationException("Client vtable does not match the platform's SendServerInfo gamedata.");

            // Wait for plugin/map initialization before looking up network messages.
            _message = UserMessage.FromPartialName("SignonState");
            var payload = Marshal.ReadIntPtr(_message.Handle, _layout.UserMessagePayloadOffset);
            if (payload == nint.Zero) throw new InvalidOperationException("SignonState payload is null.");
            _signonVTable = Marshal.ReadIntPtr(payload);
            if (_signonVTable == nint.Zero) throw new InvalidOperationException("SignonState vtable is null.");
            _message.SetInt("signon_state", 0);
            _message.SetString("addons", "");
            if (_message.ReadInt("signon_state") != 0 || _message.ReadString("addons") != "")
                throw new InvalidOperationException("SignonState protobuf fields could not be verified.");

            // ServerInfo must keep the badge in the final mount list too.
            _serverInfo = UserMessage.FromPartialName("ServerInfo");
            var infoPayload = Marshal.ReadIntPtr(_serverInfo.Handle, _layout.UserMessagePayloadOffset);
            if (infoPayload == nint.Zero) throw new InvalidOperationException("ServerInfo payload is null.");
            _serverInfoVTable = Marshal.ReadIntPtr(infoPayload);
            if (_serverInfoVTable == nint.Zero || _serverInfoVTable == _signonVTable)
                throw new InvalidOperationException("ServerInfo message type could not be verified.");
            _serverInfo.SetString("addon_name", "");
            if (_serverInfo.ReadString("addon_name") != "")
                throw new InvalidOperationException("ServerInfo addon field could not be verified.");

            // Factory and engine messages can use different vtables. Validate both forms.
            _engineSignonVTable = FindMessageVTable(engine, "CNETMsg_SignonState_t", _signonVTable, _serverInfoVTable);
            _engineServerInfoVTable = FindMessageVTable(engine, "CSVCMsg_ServerInfo_t", _serverInfoVTable, _signonVTable);
            if (_engineSignonVTable == _engineServerInfoVTable)
                throw new InvalidOperationException("Engine SignonState and ServerInfo message vtables are identical.");

            // CSS keeps the detour after unhooking. Use its cache instead of rescanning patched code.
            _replyFunction = new MemoryFunctionVoid<nint, nint>(
                _layout.ReplyConnectionSignature, engine).Handle;
            // The SDK uses an 8-bit buffer type, including BUF_DEFAULT = -1.
            _sendFunction = NativeAPI.CreateVirtualFunctionFromVTable(vtable, _layout.SendNetMessageIndex,
                3, (int)DataType.DATA_TYPE_BOOL, new object[] { (int)DataType.DATA_TYPE_POINTER,
                (int)DataType.DATA_TYPE_POINTER, (int)DataType.DATA_TYPE_CHAR });
            // SendServerInfo bypasses SendNetMessage. Edit the list after FillServerInfo.
            var serverVTable = NativeAPI.FindVirtualTable(engine, _layout.ServerVTable);
            if (serverVTable == nint.Zero)
                throw new InvalidOperationException("Network game server vtable was not found.");
            _fillServerInfoFunction = NativeAPI.CreateVirtualFunctionFromVTable(serverVTable,
                _layout.FillServerInfoIndex, 2, (int)DataType.DATA_TYPE_VOID,
                new object[] { (int)DataType.DATA_TYPE_POINTER, (int)DataType.DATA_TYPE_POINTER });
            if (_replyFunction == nint.Zero || _sendFunction == nint.Zero || _fillServerInfoFunction == nint.Zero)
                throw new InvalidOperationException("CounterStrikeSharp could not create the Workshop native hooks.");

            // The host-state request sets the next map's addons before OnMapStart.
            _hostRequestFunction = new MemoryFunctionVoid<nint, nint>(
                _layout.HostStateRequestSignature, engine).Handle;
            if (_hostRequestFunction == nint.Zero)
                throw new InvalidOperationException("Host-state request hook was not found.");
            var libraryName = OperatingSystem.IsWindows() ? "tier0.dll" : "libtier0.so";
            var engineDirectory = Path.GetDirectoryName(engine);
            var tier0 = string.IsNullOrEmpty(engineDirectory)
                ? libraryName : Path.Combine(engineDirectory, libraryName);
            var setDirectSymbol = OperatingSystem.IsWindows()
                ? "?SetDirect@CUtlString@@QEAAXPEBDH@Z" : "_ZN10CUtlString9SetDirectEPKci";
            if (!NativeLibrary.TryLoad(tier0, out _tier0Module) &&
                !NativeLibrary.TryLoad(libraryName, out _tier0Module))
                throw new InvalidOperationException("Engine string library is unavailable.");
            if (!NativeLibrary.TryGetExport(_tier0Module, setDirectSymbol, out var setDirect))
                throw new InvalidOperationException("Engine string setter is unavailable.");
            _setUtlString = Marshal.GetDelegateForFunctionPointer<SetUtlStringDirect>(setDirect);

            _fillServerInfoReference = FunctionReference.Create(_fillServerInfoHandler);
            NativeAPI.HookFunction(_fillServerInfoFunction, _fillServerInfoHandler, true);
            _fillServerInfoHooked = true;
            _sendReference = FunctionReference.Create(_sendHandler);
            NativeAPI.HookFunction(_sendFunction, _sendHandler, false);
            _sendHooked = true;
            ++_sendAttaches;
            _sendSchedule.Activity(Now);
            _replyReference = FunctionReference.Create(_replyHandler);
            NativeAPI.HookFunction(_replyFunction, _replyHandler, false);
            _replyHooked = true;
            _hostRequestReference = FunctionReference.Create(_hostRequestHandler);
            NativeAPI.HookFunction(_hostRequestFunction, _hostRequestHandler, false);
            _hostRequestHooked = true;
            // Game timers may stop during hibernation; this timer queues a world update.
            _deadlineTimer = new System.Threading.Timer(_ => QueueDeadlineCheck(), null, 1000, 1000);
            _layout.Remember(_directory);
            _started = true;
            Status = $"Hooks ready ({_layout.Platform})";
            _log.LogInformation("[CS2FaceitLevels] Addon has been loaded from built-in loader.");
        }
        catch (Exception ex)
        {
            Fault(ex);
            Detach();
        }
    }

    public void RetryStartup()
    {
        if (!CanRetryStartup) return;
        Detach();
        _faulted = false;
        Start();
    }

    internal static bool OtherLoaderPresent() =>
        ConVar.Find("mm_client_extra_addons") != null || ConVar.Find("mm_extra_addons") != null;

    // Match whole Workshop IDs without changing MAM's other addons.
    internal static bool HasBadge(string? addonList) => addonList?.Split(',',
        StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries)
        .Contains(AddonId, StringComparer.Ordinal) ?? false;

    internal static bool MamHasClientBadge() =>
        HasBadge(ConVar.Find("mm_client_extra_addons")?.StringValue);

    internal static bool MamHasBadge() =>
        MamHasClientBadge() ||
        HasBadge(ConVar.Find("mm_extra_addons")?.StringValue);

    private bool Ready => !_disposed && !_faulted;

    private static nint FindMessageVTable(string engine, string name, nint factory, nint otherMessage)
    {
        var table = NativeAPI.FindVirtualTable(engine, name);
        if (table == nint.Zero)
            throw new InvalidOperationException($"Engine message vtable {name} was not found.");
        if (table == otherMessage || !HasSameMessageMethods(factory, table, OperatingSystem.IsWindows()))
            throw new InvalidOperationException(
                $"Engine message vtable {name} is not a compatible primary network-message table " +
                $"(factory=0x{factory:X}, engine=0x{table:X}).");
        return table;
    }

    private static bool HasSameMessageMethods(nint factory, nint candidate, bool windows)
    {
        if (factory == nint.Zero || candidate == nint.Zero) return false;
        // Skip one destructor entry on Windows, two on Linux. Compare the six primary methods.
        int first = windows ? 1 : 2;
        for (int slot = first; slot < first + 6; ++slot)
        {
            var method = Marshal.ReadIntPtr(factory, slot * IntPtr.Size);
            if (method == nint.Zero || method != Marshal.ReadIntPtr(candidate, slot * IntPtr.Size)) return false;
        }
        return true;
    }

    private static bool IsMessageVTable(nint actual, nint factory, nint engine)
        => actual != nint.Zero && (actual == factory || actual == engine);

    private bool CheckThread()
    {
        if (Environment.CurrentManagedThreadId == _gameThread) return true;
        Fault(new InvalidOperationException("Workshop hook ran outside the game thread; loader disabled."));
        return false;
    }

    private (ulong SteamId, int Slot) Identity(nint client, nint server)
        => ClientIdentityReader.Read(client, server, _layout, _getClientXuid);

    private ulong GetClientXuid(int slot)
    {
        if (_getXuidFunction == nint.Zero || _engineInterface == nint.Zero)
            throw new InvalidOperationException("The engine identity getter is not initialized.");
        return NativeAPI.ExecuteVirtualFunction<ulong>(_getXuidFunction, false,
            new object[] { _engineInterface, slot });
    }

    private string ReadServerAddons(nint server) => ReadAddons(server, _layout.ServerAddonsOffset);

    private static string ReadAddons(nint owner, int offset)
    {
        var pointer = Marshal.ReadIntPtr(owner, offset);
        if (pointer == nint.Zero) return "";
        // Limit the ASCII read; Required() checks the Workshop IDs.
        var bytes = new List<byte>();
        for (int i = 0; i < 1024; ++i)
        {
            byte value = Marshal.ReadByte(pointer, i);
            if (value == 0) return Encoding.ASCII.GetString(bytes.ToArray());
            if (value != ',' && value != ' ' && (value < '0' || value > '9'))
                throw new InvalidOperationException("Server addon string failed the layout check.");
            bytes.Add(value);
        }
        throw new InvalidOperationException("Server addon string exceeds the loader limit.");
    }

    // Attach before signon/map messages to avoid hooking every gameplay packet.
    // Never attach or detach from inside a SendNetMessage callback.
    private void NeedSendHook()
    {
        _sendSchedule.Activity(Now);
        if (_sendHooked || !Ready || !_replyHooked) return;
        NativeAPI.HookFunction(_sendFunction, _sendHandler, false);
        _sendHooked = true;
        ++_sendAttaches;
    }

    // Runs on the game thread, outside SendNetMessage callbacks.
    public void ReleaseIdleSendHook()
    {
        if (!_sendHooked || !Ready || _insideMessage) return;
        try
        {
            if (!_sendSchedule.CanRelease(Now, _deadlines.Count, _handshakes)) return;
            NativeAPI.UnhookFunction(_sendFunction, _sendHandler, false);
            _sendHooked = false;
            if (_debug()) _log.LogInformation("[CS2FaceitLevels] Workshop SendNetMessage hook released until the next connection or map change.");
        }
        catch (Exception ex) { Fault(ex); }
    }

    public void MapStarted()
    {
        if (!Ready || !_replyHooked) return;
        try
        {
            _sendSchedule.MapStarted(Now);
            NeedSendHook();
        }
        catch (Exception ex) { Fault(ex); }
    }

    private HookResult OnHostRequest(DynamicHook hook)
    {
        if (!Ready || !CheckThread()) return HookResult.Continue;
        try
        {
            var request = hook.GetParam<nint>(1);
            if (request == nint.Zero || Marshal.ReadInt32(request) != 2) // HSR_GAME
                return HookResult.Continue;

            // Keep the hook for the upcoming CHANGELEVEL message.
            _sendSchedule.MapChangeRequested(Now);
            NeedSendHook();
            // Add the badge through client hooks; leave the server's addon list alone.
            if (OtherLoaderPresent()) return HookResult.Continue;
            var original = ReadAddons(request, _layout.HostStateRequestAddonsOffset);
            var required = string.Join(',', AddonHandshake.Required(original, AddonId));
            if (original == required) return HookResult.Continue;
            // SetDirect copies the string. The request outlives this temporary buffer.
            nint utf8 = Marshal.StringToCoTaskMemUTF8(required);
            try
            {
                _setUtlString!(request + _layout.HostStateRequestAddonsOffset, utf8,
                    Encoding.UTF8.GetByteCount(required));
            }
            finally { Marshal.FreeCoTaskMem(utf8); }
            ++_mapRequests;
            if (_debug()) _log.LogInformation("[CS2FaceitLevels] Workshop map request: {Addons}", required);
        }
        catch (Exception ex) { Fault(ex); }
        return HookResult.Continue;
    }

    private HookResult OnReply(DynamicHook hook)
    {
        if (!Ready || _insideReply || !CheckThread()) return HookResult.Continue;
        bool called = false;
        try
        {
            NeedSendHook();
            if (MamHasBadge()) throw new InvalidOperationException("MultiAddonManager also manages the FACEIT badge addon. Remove its ID from MAM and restart.");
            var server = hook.GetParam<nint>(0);
            var client = hook.GetParam<nint>(1);
            var (steamId, slot) = Identity(client, server);
            if (steamId == 0)
            {
                if (++_missingXuids == 1)
                    _log.LogWarning("[CS2FaceitLevels] Engine GetClientXUID returned zero for connecting slot {Slot}; this reply was left unchanged. Check css_faceit_workshop_status.", slot);
                return HookResult.Continue;
            }
            Bind(slot, steamId);
            if (WaitForWorkshop(slot, steamId)) return HookResult.Handled;
            var required = AddonHandshake.Required(ReadServerAddons(server), AddonId);
            string advertised = _handshakes.Reply(steamId, required, Now);

            // Borrow a temporary char* for this call, then restore the engine's pointer.
            nint original = Marshal.ReadIntPtr(server, _layout.ServerAddonsOffset);
            nint replacement = Marshal.StringToCoTaskMemUTF8(advertised);
            _insideReply = true;
            try
            {
                Marshal.WriteIntPtr(server, _layout.ServerAddonsOffset, replacement);
                called = true;
                NativeAPI.ExecuteVirtualFunction<object>(_replyFunction, true, new object[] { server, client });
                ++_replies;
            }
            finally
            {
                Marshal.WriteIntPtr(server, _layout.ServerAddonsOffset, original);
                Marshal.FreeCoTaskMem(replacement);
                _insideReply = false;
            }
            if (_debug()) _log.LogInformation("[CS2FaceitLevels] Workshop reply slot {Slot}: {Addons}", slot, advertised);
            return HookResult.Handled;
        }
        catch (Exception ex)
        {
            Fault(ex);
            return called ? HookResult.Handled : HookResult.Continue;
        }
    }

    private HookResult OnFillServerInfo(DynamicHook hook)
    {
        if (!Ready || !CheckThread()) return HookResult.Continue;
        try
        {
            var payload = hook.GetParam<nint>(1);
            var table = payload == nint.Zero ? nint.Zero : Marshal.ReadIntPtr(payload);
            if (!IsMessageVTable(table, _serverInfoVTable, _engineServerInfoVTable))
                throw new InvalidOperationException(
                    $"FillServerInfo payload does not match the ServerInfo message type " +
                    $"(actual=0x{table:X}, factory=0x{_serverInfoVTable:X}, " +
                    $"engine=0x{_engineServerInfoVTable:X}, fill_index={_layout.FillServerInfoIndex}).");
            var message = _serverInfo!;
            var ownedPayload = Marshal.ReadIntPtr(message.Handle, _layout.UserMessagePayloadOffset);
            try
            {
                Marshal.WriteIntPtr(message.Handle, _layout.UserMessagePayloadOffset, payload);
                var originalAddons = message.ReadString("addon_name");
                var mountedAddons = string.Join(',', AddonHandshake.Required(originalAddons, AddonId));
                if (originalAddons != mountedAddons)
                {
                    message.SetString("addon_name", mountedAddons);
                    ++_mountLists;
                    if (_debug()) _log.LogInformation("[CS2FaceitLevels] Workshop mount list: {Addons}", mountedAddons);
                }
                // Keep the protobuf edit; restore only the borrowed wrapper pointer.
            }
            finally { Marshal.WriteIntPtr(message.Handle, _layout.UserMessagePayloadOffset, ownedPayload); }
        }
        catch (Exception ex) { Fault(ex); }
        return HookResult.Continue;
    }

    private HookResult OnSendMessage(DynamicHook hook)
    {
        if (!Ready || _insideMessage || !CheckThread()) return HookResult.Continue;
        bool called = false, result = false;
        try
        {
            var payload = hook.GetParam<nint>(1);
            // Compare vtables first so gameplay packets skip decoding.
            if (payload == nint.Zero) return HookResult.Continue;
            if (!IsMessageVTable(Marshal.ReadIntPtr(payload), _signonVTable, _engineSignonVTable))
                return HookResult.Continue;
            _sendSchedule.Activity(Now);
            var client = hook.GetParam<nint>(0);
            var server = Marshal.ReadIntPtr(client, _layout.ClientServerOffset);
            var (steamId, slot) = Identity(client, server);
            if (steamId == 0) return HookResult.Continue;
            Bind(slot, steamId);
            var message = _message!;
            var ownedPayload = Marshal.ReadIntPtr(message.Handle, _layout.UserMessagePayloadOffset);
            _insideMessage = true;
            try
            {
                // Borrow the CNetMessage pointer through UserMessage's field at gamedata offset 0.
                Marshal.WriteIntPtr(message.Handle, _layout.UserMessagePayloadOffset, payload);
                int state = message.ReadInt("signon_state");
                string addons = message.ReadString("addons");
                string? next;
                if (state == 7) // SIGNONSTATE_CHANGELEVEL
                {
                    // Download the destination Workshop map first.
                    next = AddonHandshake.MapChangeAddon(addons, AddonId);
                    _handshakes.ChangingMap(steamId, next, Now);
                }
                else
                {
                    var required = AddonHandshake.Required(ReadServerAddons(server), AddonId);
                    next = _handshakes.Next(steamId, required, Now);
                    if (next == null)
                    {
                        CompleteWait(slot);
                        return HookResult.Continue;
                    }
                }
                if (WaitForWorkshop(slot, steamId))
                {
                    hook.SetReturn(false);
                    return HookResult.Handled;
                }
                if (state == 7 && addons == next) return HookResult.Continue;
                try
                {
                    message.SetInt("signon_state", 7);
                    message.SetString("addons", next);
                    called = true;
                    result = NativeAPI.ExecuteVirtualFunction<bool>(_sendFunction, true,
                        new object[] { client, payload, hook.GetParam<byte>(2) });
                    hook.SetReturn(result);
                    ++_signons;
                }
                finally
                {
                    // Restore the message before it is reused for another client.
                    message.SetInt("signon_state", state);
                    message.SetString("addons", addons);
                }
                if (_debug()) _log.LogInformation("[CS2FaceitLevels] Workshop signon slot {Slot}: {Addon}", slot, next);
                return HookResult.Handled;
            }
            finally
            {
                Marshal.WriteIntPtr(message.Handle, _layout.UserMessagePayloadOffset, ownedPayload);
                _insideMessage = false;
            }
        }
        catch (Exception ex)
        {
            Fault(ex);
            if (called) hook.SetReturn(result);
            return called ? HookResult.Handled : HookResult.Continue;
        }
    }

    public void ClientConnect(int slot)
    {
        if (!Ready || _getXuidFunction == nint.Zero || slot is < 0 or >= 256) return;
        try
        {
            NeedSendHook();
            // Check the identity again because the slot may have been reused.
            var steamId = GetClientXuid(slot);
            if (steamId == 0) return;
            Bind(slot, steamId);
            CompleteWait(slot);
            _handshakes.Connected(steamId, Now);
        }
        catch (Exception ex) { Fault(ex); }
    }

    public void ClientActive(int slot, ulong steamId)
    {
        if (!Ready) return;
        Bind(slot, steamId);
        CompleteWait(slot);
        _handshakes.Active(steamId, Now);
        ++_completed;
    }

    public void ClientDisconnect(int slot)
    {
        CompleteWait(slot);
        if (_slots.Remove(slot, out var steamId)) _handshakes.Disconnected(steamId, Now);
        // Keep download progress, but clear this connection's deadline.
    }

    private void Bind(int slot, ulong steamId)
    {
        if (_slots.TryGetValue(slot, out var previous) && previous != steamId)
        {
            CompleteWait(slot);
            _handshakes.Forget(previous);
        }
        _slots[slot] = steamId;
    }

    private bool WaitForWorkshop(int slot, ulong steamId)
    {
        bool expired = _deadlines.Wait(slot, steamId, Now);
        Volatile.Write(ref _waitingCount, _deadlines.Count);
        if (expired) QueueDeadlineCheck();
        // Block replies while the expired connection waits to be disconnected.
        return expired;
    }

    private void CompleteWait(int slot)
    {
        _deadlines.Remove(slot);
        Volatile.Write(ref _waitingCount, _deadlines.Count);
    }

    private void QueueDeadlineCheck()
    {
        // Also called by the timer; do all native and dictionary work in the queued update.
        if (!Ready || Volatile.Read(ref _waitingCount) == 0 ||
            Interlocked.CompareExchange(ref _deadlineCheckQueued, 1, 0) != 0) return;
        Server.NextWorldUpdate(() =>
        {
            try
            {
                if (!Ready || !_replyHooked) return;
                foreach (var pending in _deadlines.Expired(Now))
                {
                    var currentSteamId = GetClientXuid(pending.Slot);
                    if (!_deadlines.TakeExpired(pending, currentSteamId, Now)) continue;
                    _slots.Remove(pending.Slot);
                    _handshakes.Forget(pending.SteamId);
                    // Kick through the server in this update, outside the native network callback.
                    NativeAPI.DisconnectClient(pending.Slot, (int)NetworkDisconnectionReason.NETWORK_DISCONNECT_TIMEDOUT);
                    ++_timedOut;
                    _log.LogWarning("[CS2FaceitLevels] Disconnected slot {Slot}: required Workshop handshake timed out after {Seconds}s. Reconnect and accept the download.",
                        pending.Slot, ConnectionDeadlines.TimeoutSeconds);
                }
                Volatile.Write(ref _waitingCount, _deadlines.Count);
            }
            catch (Exception ex) { Fault(ex); }
            finally { Interlocked.Exchange(ref _deadlineCheckQueued, 0); }
        });
    }

    public void Maintenance()
    {
        if (_disposed) return;
        if (_faulted) Detach();
        else _handshakes.Prune(Now);
    }

    public void RestoreReload(string? json)
    {
        if (json == null) return;
        try
        {
            var slots = _handshakes.ImportReload(json, Now, _deadlines, _sendSchedule);
            _slots.Clear();
            foreach (var pair in slots) _slots.Add(pair.Key, pair.Value);
            Volatile.Write(ref _waitingCount, _deadlines.Count);
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[CS2FaceitLevels] Discarded expired or invalid Workshop reload state.");
        }
    }

    public void PreserveReload(bool hotReload)
    {
        WorkshopReloadBridge.Save(_directory, null);
        if (!hotReload || _faulted || !_replyHooked) return;
        try { WorkshopReloadBridge.Save(_directory, _handshakes.ExportReload(_slots, Now, _deadlines, _sendSchedule)); }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "[CS2FaceitLevels] Could not preserve Workshop handshakes across reload.");
        }
    }

    private void Fault(Exception ex)
    {
        if (_faulted) return;
        _faulted = true;
        Status = "Disabled: " + ex.Message;
        _log.LogError(ex, "[CS2FaceitLevels] Workshop loader disabled. FACEIT lookups remain available. {Reason}", ex.Message);
        // Wait until the callback returns before unhooking.
        Server.NextWorldUpdate(() => { if (!_disposed && _faulted) Detach(); });
    }

    private void Detach()
    {
        _deadlineTimer?.Dispose();
        _deadlineTimer = null;
        if (_replyHooked)
        {
            NativeAPI.UnhookFunction(_replyFunction, _replyHandler, false);
            _replyHooked = false;
        }
        if (_sendHooked)
        {
            NativeAPI.UnhookFunction(_sendFunction, _sendHandler, false);
            _sendHooked = false;
        }
        if (_fillServerInfoHooked)
        {
            NativeAPI.UnhookFunction(_fillServerInfoFunction, _fillServerInfoHandler, true);
            _fillServerInfoHooked = false;
        }
        if (_hostRequestHooked)
        {
            NativeAPI.UnhookFunction(_hostRequestFunction, _hostRequestHandler, false);
            _hostRequestHooked = false;
        }
        // BasePlugin does not track these hooks. Detach callbacks before releasing references.
        if (_replyReference != null) FunctionReference.Remove(_replyReference.Identifier);
        if (_sendReference != null) FunctionReference.Remove(_sendReference.Identifier);
        if (_fillServerInfoReference != null) FunctionReference.Remove(_fillServerInfoReference.Identifier);
        if (_hostRequestReference != null) FunctionReference.Remove(_hostRequestReference.Identifier);
        _replyReference = _sendReference = _fillServerInfoReference = _hostRequestReference = null;
        _sendSchedule.Clear();
        _setUtlString = null;
        if (_tier0Module != nint.Zero)
        {
            NativeLibrary.Free(_tier0Module);
            _tier0Module = nint.Zero;
        }
        _message?.Dispose();
        _message = null;
        _serverInfo?.Dispose();
        _serverInfo = null;
        _signonVTable = _serverInfoVTable = _engineSignonVTable = _engineServerInfoVTable = nint.Zero;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Detach();
        _handshakes.Clear();
        _slots.Clear();
        _deadlines.Clear();
        Volatile.Write(ref _waitingCount, 0);
        Status = "Stopped";
    }
}
