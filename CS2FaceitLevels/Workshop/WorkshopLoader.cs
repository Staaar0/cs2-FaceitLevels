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
    private nint _tier0Module;
    private SetUtlStringDirect? _setUtlString;
    private nint _engineInterface, _getXuidFunction;
    private UserMessage? _message, _serverInfo;
    private bool _replyHooked, _sendHooked, _fillServerInfoHooked, _hostRequestHooked, _insideReply, _insideMessage;
    private volatile bool _disposed;
    private volatile bool _faulted;
    private long _replies, _signons, _completed, _mountLists, _mapRequests;
    private long _missingXuids, _timedOut;

    public string Status { get; private set; } = "Not started";
    public string Summary => $"{Status}; addon={AddonId}; replies={_replies}; signons={_signons}; " +
        $"joined={_completed}; tracked={_handshakes.Count}; missing_xuids={_missingXuids}; " +
        $"waiting={_deadlines.Count}; timed_out={_timedOut}; mount_lists={_mountLists}; map_requests={_mapRequests}";
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
        if (_disposed || _replyHooked || _faulted) return;
        if (OtherLoaderPresent())
        {
            Status = "Disabled: MultiAddonManager detected";
            _log.LogWarning("[CS2FaceitLevels] {Status}. Remove it and fully restart to enable the built-in loader.", Status);
            return;
        }
        try
        {
            _layout = EngineLayout.Read(_directory);
            var engine = Addresses.EnginePath;
            // Use the engine's connection-time XUID API. GetClientSteamID is an
            // authenticated-player API and can still be null during ReplyConnection.
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
            // VibeSignatures build 14182 supplies this signature/index for both
            // platforms. Validate the client vtable once, without another hook.
            var sendServerInfo = NativeAPI.FindSignature(engine, _layout.SendServerInfoSignature);
            if (sendServerInfo == nint.Zero ||
                Marshal.ReadIntPtr(vtable, _layout.SendServerInfoIndex * IntPtr.Size) != sendServerInfo)
                throw new InvalidOperationException("Client vtable does not match the platform's SendServerInfo gamedata.");

            // Network messages are available after plugins/map initialization, not in Load().
            _message = UserMessage.FromPartialName("SignonState");
            var payload = Marshal.ReadIntPtr(_message.Handle, _layout.UserMessagePayloadOffset);
            if (payload == nint.Zero) throw new InvalidOperationException("SignonState payload is null.");
            _signonVTable = Marshal.ReadIntPtr(payload);
            if (_signonVTable == nint.Zero) throw new InvalidOperationException("SignonState vtable is null.");
            _message.SetInt("signon_state", 0);
            _message.SetString("addons", "");
            if (_message.ReadInt("signon_state") != 0 || _message.ReadString("addons") != "")
                throw new InvalidOperationException("SignonState protobuf fields could not be verified.");

            // Download replies are not the final mount list. ServerInfo must also
            // retain the badge override on reconnects and after every map load.
            _serverInfo = UserMessage.FromPartialName("ServerInfo");
            var infoPayload = Marshal.ReadIntPtr(_serverInfo.Handle, _layout.UserMessagePayloadOffset);
            if (infoPayload == nint.Zero) throw new InvalidOperationException("ServerInfo payload is null.");
            _serverInfoVTable = Marshal.ReadIntPtr(infoPayload);
            if (_serverInfoVTable == nint.Zero || _serverInfoVTable == _signonVTable)
                throw new InvalidOperationException("ServerInfo message type could not be verified.");
            _serverInfo.SetString("addon_name", "");
            if (_serverInfo.ReadString("addon_name") != "")
                throw new InvalidOperationException("ServerInfo addon field could not be verified.");

            // Use CSS's process-wide managed signature cache. Native UnhookFunction
            // removes our callback but leaves the detour in place; rescanning the
            // patched entry on hot reload can bind a different signature match.
            // The cached native function survives plugin reloads; our delegates do not.
            _replyFunction = new MemoryFunctionVoid<nint, nint>(
                _layout.ReplyConnectionSignature, engine).Handle;
            // NetChannelBufType_t is an 8-bit value in the SDK (including BUF_DEFAULT=-1).
            _sendFunction = NativeAPI.CreateVirtualFunctionFromVTable(vtable, _layout.SendNetMessageIndex,
                3, (int)DataType.DATA_TYPE_BOOL, new object[] { (int)DataType.DATA_TYPE_POINTER,
                (int)DataType.DATA_TYPE_POINTER, (int)DataType.DATA_TYPE_CHAR });
            // SendServerInfo serializes its messages directly into a connection
            // buffer, bypassing SendNetMessage. Edit the final mount list after
            // FillServerInfo populates it, before the engine serializes it.
            var serverVTable = NativeAPI.FindVirtualTable(engine, _layout.ServerVTable);
            if (serverVTable == nint.Zero)
                throw new InvalidOperationException("Network game server vtable was not found.");
            _fillServerInfoFunction = NativeAPI.CreateVirtualFunctionFromVTable(serverVTable,
                _layout.FillServerInfoIndex, 2, (int)DataType.DATA_TYPE_VOID,
                new object[] { (int)DataType.DATA_TYPE_POINTER, (int)DataType.DATA_TYPE_POINTER });
            if (_replyFunction == nint.Zero || _sendFunction == nint.Zero || _fillServerInfoFunction == nint.Zero)
                throw new InvalidOperationException("CounterStrikeSharp could not create the Workshop native hooks.");

            // A map change constructs a new host-state request. Its addon list
            // becomes the next map's mount list before OnMapStart can run.
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
            _replyReference = FunctionReference.Create(_replyHandler);
            NativeAPI.HookFunction(_replyFunction, _replyHandler, false);
            _replyHooked = true;
            _hostRequestReference = FunctionReference.Create(_hostRequestHandler);
            NativeAPI.HookFunction(_hostRequestFunction, _hostRequestHandler, false);
            _hostRequestHooked = true;
            // A wall-clock timer queues main-thread work even with no fully joined
            // players. Ordinary game timers/NextFrame can stop during hibernation.
            _deadlineTimer = new System.Threading.Timer(_ => QueueDeadlineCheck(), null, 1000, 1000);
            Status = $"Hooks ready ({_layout.Platform})";
            _log.LogInformation("[CS2FaceitLevels] Workshop hooks ready for {Addon}. Waiting for client handshakes.", AddonId);
        }
        catch (Exception ex)
        {
            Fault(ex);
            Detach();
        }
    }

    private static bool OtherLoaderPresent() =>
        ConVar.Find("mm_client_extra_addons") != null || ConVar.Find("mm_extra_addons") != null;

    private bool Ready => !_disposed && !_faulted;

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
        // A bounded ASCII read, then Required() validates every numeric Workshop ID.
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

    private HookResult OnHostRequest(DynamicHook hook)
    {
        if (!Ready || !CheckThread()) return HookResult.Continue;
        try
        {
            var request = hook.GetParam<nint>(1);
            if (request == nint.Zero || Marshal.ReadInt32(request) != 2) // HSR_GAME
                return HookResult.Continue;
            var original = ReadAddons(request, _layout.HostStateRequestAddonsOffset);
            var required = string.Join(',', AddonHandshake.Required(original, AddonId));
            if (original == required) return HookResult.Continue;
            // CUtlString::SetDirect makes its own engine-owned copy. This request
            // outlives the hook, so borrowing a temporary pointer here is unsafe.
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
            if (OtherLoaderPresent()) throw new InvalidOperationException("Another addon loader was loaded; restart with only one loader.");
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

            // CUtlString contains one char*. Only borrow a temporary buffer for this call.
            // Never free the engine's allocation or leave the server addon list modified.
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
            if (payload == nint.Zero || Marshal.ReadIntPtr(payload) != _serverInfoVTable)
                throw new InvalidOperationException("FillServerInfo payload does not match the ServerInfo message type.");
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
                // Keep the edited engine-owned protobuf for subsequent serialization.
                // Only the borrowed wrapper pointer is restored below.
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
            // The common path only compares vtables; do not decode gameplay packets.
            if (payload == nint.Zero) return HookResult.Continue;
            if (Marshal.ReadIntPtr(payload) != _signonVTable) return HookResult.Continue;
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
                // CSS has no public constructor for a borrowed CNetMessage pointer. Its
                // native UserMessage wrapper exposes this field at offset 0 (gamedata).
                Marshal.WriteIntPtr(message.Handle, _layout.UserMessagePayloadOffset, payload);
                int state = message.ReadInt("signon_state");
                string addons = message.ReadString("addons");
                string? next;
                if (state == 7) // SIGNONSTATE_CHANGELEVEL
                {
                    // The engine's actual destination Workshop map must download first.
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
                    // A network message may be reused for other clients.
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
            // Resolve again rather than trust a slot binding from a previous occupant.
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
        // Preserve only per-Steam download progress, never a departed slot's deadline.
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
        // Suppress further replies after expiry. Never let an empty-addon reply
        // escape while the disconnect is waiting for the next world update.
        return expired;
    }

    private void CompleteWait(int slot)
    {
        _deadlines.Remove(slot);
        Volatile.Write(ref _waitingCount, _deadlines.Count);
    }

    private void QueueDeadlineCheck()
    {
        // Called by the timer too: no native calls or dictionary reads on its thread.
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
                    // Works before a CCSPlayerController exists. Do not kick from
                    // inside ReplyConnection/SendNetMessage's native call stack.
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
            var slots = _handshakes.ImportReload(json, Now, _deadlines);
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
        try { WorkshopReloadBridge.Save(_directory, _handshakes.ExportReload(_slots, Now, _deadlines)); }
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
        // Never unhook a function while its callback is still on the stack.
        Server.NextWorldUpdate(() => { if (!_disposed) Detach(); });
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
        // Direct native hooks are not tracked by BasePlugin's listener cleanup.
        // Release their managed references only after all callbacks are detached.
        if (_replyReference != null) FunctionReference.Remove(_replyReference.Identifier);
        if (_sendReference != null) FunctionReference.Remove(_sendReference.Identifier);
        if (_fillServerInfoReference != null) FunctionReference.Remove(_fillServerInfoReference.Identifier);
        if (_hostRequestReference != null) FunctionReference.Remove(_hostRequestReference.Identifier);
        _replyReference = _sendReference = _fillServerInfoReference = _hostRequestReference = null;
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
