#include "Workshop/WorkshopLoader.h"

#include "Runtime/Common.h"
#include "Runtime/ModuleImage.h"
#include "Runtime/Scheduler.h"

#include <engine2/ivengineserver2.h>
#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>
#include <networksystem/inetworkserializer.h>
#include <tier0/convar.h>

#include "netmessages.pb.h"
#include "networkbasetypes.pb.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace cfl::workshop {
namespace {

#ifdef _WIN32
constexpr const char *kEngineModule = "engine2.dll";
constexpr int kFirstMessageMethod = 1;
#else
constexpr const char *kEngineModule = "libengine2.so";
constexpr int kFirstMessageMethod = 2;
#endif
constexpr int kMessageMethods = 5;
constexpr int kSignonStateChangeLevel = 7;

std::optional<std::string> ConVarString(const char *name)
{
	if (!g_pCVar)
		return std::nullopt;
	ConVarRefAbstract ref(name);
	if (!ref.IsValidRef())
		return std::nullopt;
	const CUtlString value = ref.GetString();
	return std::string(value.Get() ? value.Get() : "");
}

bool HasBadge(const std::optional<std::string> &addonList)
{
	if (!addonList)
		return false;
	for (std::string_view id : SplitNonEmpty(*addonList, ','))
		if (Trim(id) == kAddonId)
			return true;
	return false;
}

void *VTableEntry(void *vtable, int index)
{
	return KHook::FindOriginalVirtual(static_cast<void **>(vtable), index);
}

bool HasSameMessageMethods(void *factory, void *candidate)
{
	if (!factory || !candidate)
		return false;
	auto **a = static_cast<void **>(factory);
	auto **b = static_cast<void **>(candidate);
	for (int slot = kFirstMessageMethod; slot < kFirstMessageMethod + kMessageMethods; ++slot)
		if (!a[slot] || a[slot] != b[slot])
			return false;
	return true;
}

void *FindMessageVTable(const ModuleImage &engine, const char *name, void *factory, void *otherMessage)
{
	void *table = engine.FindVirtualTable(name);
	if (!table)
		throw WorkshopError(std::string("Engine message vtable ") + name + " was not found.");
	if (table == otherMessage || !HasSameMessageMethods(factory, table))
	{
		char message[256];
		std::snprintf(message, sizeof(message),
			"Engine message vtable %s is not a compatible primary network-message table (factory=%p, engine=%p).", name,
			factory, table);
		throw WorkshopError(message);
	}
	return table;
}

bool IsMessageVTable(void *actual, void *factory, void *engine)
{
	return actual && (actual == factory || actual == engine);
}

template <typename Proto, typename Fill>
void *FactoryVTable(const char *partialName, const char *typeName, Fill fill)
{
	INetworkMessages *messages = g_pNetworkMessages;
	if (!messages)
		throw WorkshopError("The network messages interface is missing.");
	NetMessageInfo_t *info = messages->FindNetworkMessagePartial(partialName);
	std::unique_ptr<CNetMessage> message(info ? info->AllocateMessage() : nullptr);
	if (!message)
		throw WorkshopError(std::string(partialName) + " payload is null.");

	void *vtable = *reinterpret_cast<void **>(message.get());
	Proto *proto = message->template ToPB<Proto>();
	bool verified = vtable && proto->GetTypeName() == typeName;
	if (verified)
	{
		Proto expected;
		fill(expected);
		fill(*proto);
		verified = proto->SerializeAsString() == expected.SerializeAsString();
		proto->Clear();
	}
	if (!verified)
		throw WorkshopError(std::string(partialName) + " protobuf fields could not be verified.");
	return vtable;
}

struct Flag
{
	explicit Flag(bool &value) : value_(value) { value_ = true; }
	~Flag() { value_ = false; }
	bool &value_;
};

thread_local bool tInsideReply = false;
thread_local bool tInsideMessage = false;

}

Loader::Loader(std::string directory, Scheduler &scheduler, HookKeeper &hooks, IVEngineServer2 *engine,
	std::optional<std::string> runtimeGamedata, const bool &debug)
	: directory_(std::move(directory)), scheduler_(scheduler), hooks_(hooks), engine_(engine), debug_(debug),
	  runtimeGamedata_(std::move(runtimeGamedata)), alive_(std::make_shared<bool>(true))
{
}

Loader::~Loader()
{
	Dispose();
}

template <typename Hook>
Hook *Loader::ArmHook(Hook *current, void *address, typename Hook::Handler pre, typename Hook::Handler post)
{
	if (current && current->Address() == address)
	{
		current->Activate();
		return current;
	}
	if (current)
		current->Deactivate();
	Hook *hook = hooks_.Make<Hook>(this, pre, post);
	hook->Attach(address);
	return hook;
}

bool Loader::CanRetryStartup() const
{
	Guard guard(mutex_);
	return !disposed_ && faulted_ && !started_;
}

std::optional<std::string> Loader::RuntimeGamedata() const
{
	Guard guard(mutex_);
	return runtimeGamedata_;
}

std::string Loader::Summary() const
{
	Guard guard(mutex_);
	char text[512];
	std::snprintf(text, sizeof(text),
		"%s; addon=%s; replies=%llu; signons=%llu; joined=%llu; tracked=%zu; missing_xuids=%llu; waiting=%zu; "
		"timed_out=%llu; mount_lists=%llu; map_requests=%llu; send_hook=%s; send_attaches=%llu",
		status_.c_str(), kAddonId, static_cast<unsigned long long>(replies_), static_cast<unsigned long long>(signons_),
		static_cast<unsigned long long>(completed_), handshakes_.Count(), static_cast<unsigned long long>(missingXuids_),
		deadlines_.Count(), static_cast<unsigned long long>(timedOut_), static_cast<unsigned long long>(mountLists_),
		static_cast<unsigned long long>(mapRequests_), sendArmed_ ? "attached" : "idle",
		static_cast<unsigned long long>(sendAttaches_));
	return text;
}

bool Loader::OtherLoaderPresent()
{
	return ConVarString("mm_client_extra_addons") || ConVarString("mm_extra_addons");
}

bool Loader::MamHasClientBadge()
{
	return HasBadge(ConVarString("mm_client_extra_addons"));
}

bool Loader::MamHasBadge()
{
	return MamHasClientBadge() || HasBadge(ConVarString("mm_extra_addons"));
}

void Loader::Start()
{
	Guard guard(mutex_);
	if (disposed_)
		return;
	if (OtherLoaderPresent())
	{
		if (armed_ || sendArmed_)
			Detach();
		if (!MamHasBadge())
		{
			status_ = "Waiting for MultiAddonManager badge registration";
			return;
		}
		if (status_ != "Managed by MultiAddonManager")
		{
			status_ = "Managed by MultiAddonManager";
			Log("Addon has been loaded from MultiAddonManager.");
		}
		return;
	}
	if (faulted_ || armed_)
		return;

	try
	{
		std::string json;
		if (runtimeGamedata_)
			json = *runtimeGamedata_;
		else if (!ReadFile(JoinPath(directory_, "workshop.gamedata.json"), json, 1024 * 1024))
			throw WorkshopError("workshop.gamedata.json could not be read.");
		layout_ = ParseLayout(json, CurrentPlatform());

		ModuleImage engine;
		std::string error;
		if (!engine.Open(kEngineModule, error))
			throw WorkshopError("The engine module could not be inspected: " + error);

		engineInterface_ = engine_;
		if (!engineInterface_)
			throw WorkshopError("The engine server interface was not found.");
		void **engineVTable = *reinterpret_cast<void ***>(engineInterface_);
		if (!engineVTable)
			throw WorkshopError("The engine server interface vtable is null.");
		auto getAppId = reinterpret_cast<uint32_t (*)(void *)>(VTableEntry(engineVTable, layout_.getAppIdIndex));
		if (!getAppId || getAppId(engineInterface_) != 730)
			throw WorkshopError("The engine server interface did not report CS2 App ID 730.");
		getXuidFunction_ = VTableEntry(engineVTable, layout_.getClientXuidIndex);
		if (!getXuidFunction_)
			throw WorkshopError("The engine GetClientXUID function could not be created.");

		void *clientVTable = engine.FindVirtualTable(layout_.clientVTable);
		if (!clientVTable)
			throw WorkshopError("CServerSideClient vtable was not found.");
		void *sendServerInfo = engine.FindSignature(layout_.sendServerInfoSignature);
		if (!sendServerInfo || VTableEntry(clientVTable, layout_.sendServerInfoIndex) != sendServerInfo)
			throw WorkshopError("Client vtable does not match the platform's SendServerInfo gamedata.");

		VerifyMessages();
		engineSignonVTable_ = FindMessageVTable(engine, "CNETMsg_SignonState_t", signonVTable_, serverInfoVTable_);
		engineServerInfoVTable_ = FindMessageVTable(engine, "CSVCMsg_ServerInfo_t", serverInfoVTable_, signonVTable_);
		if (engineSignonVTable_ == engineServerInfoVTable_)
			throw WorkshopError("Engine SignonState and ServerInfo message vtables are identical.");

		replyFunction_ = engine.FindSignature(layout_.replyConnectionSignature);
		sendFunction_ = VTableEntry(clientVTable, layout_.sendNetMessageIndex);
		void *serverVTable = engine.FindVirtualTable(layout_.serverVTable);
		if (!serverVTable)
			throw WorkshopError("Network game server vtable was not found.");
		fillServerInfoFunction_ = VTableEntry(serverVTable, layout_.fillServerInfoIndex);
		if (!replyFunction_ || !sendFunction_ || !fillServerInfoFunction_)
			throw WorkshopError("The Workshop native hooks could not be created.");
		hostRequestFunction_ = engine.FindSignature(layout_.hostStateRequestSignature);
		if (!hostRequestFunction_)
			throw WorkshopError("Host-state request hook was not found.");

		fillHook_ = ArmHook(fillHook_, fillServerInfoFunction_, nullptr, &Loader::OnFillServerInfo);
		ArmSendHook();
		sendArmed_ = true;
		++sendAttaches_;
		replyHook_ = ArmHook(replyHook_, replyFunction_, &Loader::OnReply, nullptr);
		hostRequestHook_ = ArmHook(hostRequestHook_, hostRequestFunction_, &Loader::OnHostRequest, nullptr);
		armed_ = true;

		runtimeGamedata_ = layout_.sourceJson;
		started_ = true;
		status_ = "Hooks ready (" + layout_.platform + ")";
		Log("Addon has been loaded from built-in loader.");
		if (deadlines_.Count())
			ArmDeadlineCheck(1.0);
		QueueSendRelease();
	}
	catch (const std::exception &ex)
	{
		Fault(ex.what());
		Detach();
	}
}

void Loader::VerifyMessages()
{
	signonVTable_ = FactoryVTable<CNETMsg_SignonState>("SignonState", "CNETMsg_SignonState", [](CNETMsg_SignonState &m) {
		m.set_signon_state(static_cast<SignonState_t>(kSignonStateChangeLevel));
		m.set_addons("3724637448");
	});
	serverInfoVTable_ = FactoryVTable<CSVCMsg_ServerInfo>("ServerInfo", "CSVCMsg_ServerInfo",
		[](CSVCMsg_ServerInfo &m) { m.set_addon_name("3724637448"); });
	if (serverInfoVTable_ == signonVTable_)
		throw WorkshopError("ServerInfo message type could not be verified.");
}

void Loader::RetryStartup()
{
	Guard guard(mutex_);
	if (!CanRetryStartup())
		return;
	Detach();
	faulted_ = false;
	Start();
}

std::pair<uint64_t, int> Loader::Identity(void *client, void *server)
{
	if (!client || !server || *reinterpret_cast<void **>(static_cast<uint8_t *>(client) + layout_.clientServerOffset) != server)
		throw WorkshopError("Client/server memory layout does not match Workshop gamedata.");
	const int slot = *reinterpret_cast<int32_t *>(static_cast<uint8_t *>(client) + layout_.clientSlotOffset);
	if (slot < 0 || slot >= 256)
		throw WorkshopError("Client slot " + std::to_string(slot) + " is invalid at Workshop gamedata offset " +
			std::to_string(layout_.clientSlotOffset) + ".");
	const uint64_t steamId = GetClientXuid(slot);
	if (steamId == 0)
		return {0, slot};
	if ((steamId >> 32) != 0x01100001ull || static_cast<uint32_t>(steamId) == 0)
		throw WorkshopError("Engine GetClientXUID returned an invalid Steam ID format for slot " + std::to_string(slot) + ".");
	return {steamId, slot};
}

uint64_t Loader::GetClientXuid(int slot)
{
	if (!getXuidFunction_ || !engineInterface_)
		throw WorkshopError("The engine identity getter is not initialized.");
	return reinterpret_cast<uint64_t (*)(void *, int)>(getXuidFunction_)(engineInterface_, slot);
}

std::string Loader::ReadServerAddons(void *server) const
{
	const char *text = *reinterpret_cast<const char **>(static_cast<uint8_t *>(server) + layout_.serverAddonsOffset);
	if (!text)
		return {};
	std::string result;
	for (int i = 0; i < 1024; ++i)
	{
		const char c = text[i];
		if (c == '\0')
			return result;
		const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ',' ||
			c == ' ' || c == '_' || c == '-' || c == '.';
		if (!allowed)
			throw WorkshopError("Server addon string failed the layout check.");
		result += c;
	}
	throw WorkshopError("Server addon string exceeds the loader limit.");
}

void Loader::ArmSendHook()
{
	if (sendHook_ && sendHook_->Address() == sendFunction_ && !sendHook_->Detaching())
	{
		sendHook_->Activate();
		return;
	}
	if (sendHook_)
		sendHook_->Detach();
	for (SendHook *candidate : sendHooks_)
	{
		if (candidate->RemovalComplete())
		{
			sendHook_ = candidate;
			sendHook_->Attach(sendFunction_);
			return;
		}
	}
	sendHook_ = hooks_.Make<SendHook>(this, &Loader::OnSendMessage, nullptr);
	sendHooks_.push_back(sendHook_);
	sendHook_->Attach(sendFunction_);
}

void Loader::NeedSendHook()
{
	if (sendArmed_ || !Ready() || !armed_)
		return;
	ArmSendHook();
	sendArmed_ = true;
	++sendAttaches_;
}

void Loader::QueueSendRelease()
{
	if (!sendArmed_ || !Ready() || releaseQueued_)
		return;
	releaseQueued_ = true;
	std::weak_ptr<bool> alive = alive_;
	scheduler_.NextFrame([this, alive] {
		if (auto flag = alive.lock(); flag && *flag)
			ReleaseSendHook();
	});
}

void Loader::ReleaseSendHook()
{
	Guard guard(mutex_);
	releaseQueued_ = false;
	if (!sendArmed_ || !Ready() || tInsideMessage || tInsideReply)
		return;
	if (!sendSchedule_.CanRelease(Now(), deadlines_.Count()))
		return;
	sendArmed_ = false;
	sendHook_->Detach();
	if (debug_)
		Log("Workshop SendNetMessage hook released until the next connection or map change.");
}

void Loader::MapStarted()
{
	Guard guard(mutex_);
	if (!Ready() || !armed_)
		return;
	const double now = Now();
	sendSchedule_.MapStarted();
	handshakes_.MapStarted(now);
	for (const auto &bound : slots_)
		sendSchedule_.Loading(bound.first, now);
	NeedSendHook();
}

KHook::Return<void> Loader::OnHostRequest(HostRequestHook &, void *, void *request)
{
	Guard guard(mutex_);
	if (!Ready())
		return {KHook::Action::Ignore};
	try
	{
		if (!request || *static_cast<int32_t *>(request) != 2)
			return {KHook::Action::Ignore};
		const double now = Now();
		sendSchedule_.MapChangeRequested(now);
		handshakes_.MapChanging(now);
		deadlines_.Clear();
		for (const auto &bound : slots_)
			sendSchedule_.Loading(bound.first, now);
		NeedSendHook();
		++mapRequests_;
		if (debug_)
			Log("Workshop map change requested; %zu player(s) get the FACEIT badge again after loading.", slots_.size());
	}
	catch (const std::exception &ex)
	{
		Fault(ex.what());
	}
	return {KHook::Action::Ignore};
}

KHook::Return<void> Loader::OnReply(ReplyHook &hook, void *server, void *client)
{
	if (tInsideReply)
		return {KHook::Action::Ignore};
	std::string advertised;
	const char **field = nullptr;
	int slot = -1;
	{
		Guard guard(mutex_);
		if (!Ready())
			return {KHook::Action::Ignore};
		try
		{
			NeedSendHook();
			if (MamHasBadge())
				throw WorkshopError("MultiAddonManager also manages the FACEIT badge addon. Remove its ID from MAM and restart.");
			const auto [steamId, clientSlot] = Identity(client, server);
			slot = clientSlot;
			if (steamId == 0)
			{
				if (++missingXuids_ == 1)
					LogWarning("Engine GetClientXUID returned zero for connecting slot %d; this reply was left unchanged. "
							   "Check faceit_workshop_status.", slot);
				return {KHook::Action::Ignore};
			}
			Bind(slot, steamId);
			if (WaitForWorkshop(slot, steamId))
				return {KHook::Action::Supersede};
			const std::vector<std::string> required = Required(ReadServerAddons(server), kAddonId);
			advertised = handshakes_.Reply(steamId, required, Now());
			field = reinterpret_cast<const char **>(static_cast<uint8_t *>(server) + layout_.serverAddonsOffset);
		}
		catch (const std::exception &ex)
		{
			Fault(ex.what());
			return {KHook::Action::Ignore};
		}
	}

	const char *original = *field;
	bool failed = false;
	{
		Flag inside(tInsideReply);
		*field = advertised.c_str();
		try
		{
			hook.CallOriginal(server, client);
		}
		catch (...)
		{
			failed = true;
		}
		*field = original;
	}

	Guard guard(mutex_);
	if (failed)
	{
		Fault("The engine connection reply failed while advertising Workshop addons.");
		return {KHook::Action::Supersede};
	}
	++replies_;
	if (debug_)
		Log("Workshop reply slot %d: %s", slot, advertised.c_str());
	return {KHook::Action::Supersede};
}

KHook::Return<void> Loader::OnFillServerInfo(FillHook &, void *, void *info)
{
	Guard guard(mutex_);
	if (!Ready())
		return {KHook::Action::Ignore};
	try
	{
		void *table = info ? *static_cast<void **>(info) : nullptr;
		if (!IsMessageVTable(table, serverInfoVTable_, engineServerInfoVTable_))
		{
			char message[256];
			std::snprintf(message, sizeof(message),
				"FillServerInfo payload does not match the ServerInfo message type (actual=%p, factory=%p, engine=%p, fill_index=%d).",
				table, serverInfoVTable_, engineServerInfoVTable_, layout_.fillServerInfoIndex);
			throw WorkshopError(message);
		}
		CSVCMsg_ServerInfo *message = static_cast<CNetMessage *>(info)->ToPB<CSVCMsg_ServerInfo>();
		const std::string originalAddons = message->addon_name();
		const std::string mountedAddons = Join(Required(originalAddons, kAddonId), ',');
		if (originalAddons != mountedAddons)
		{
			message->set_addon_name(mountedAddons);
			++mountLists_;
			if (debug_)
				Log("Workshop mount list: %s", mountedAddons.c_str());
		}
	}
	catch (const std::exception &ex)
	{
		Fault(ex.what());
	}
	return {KHook::Action::Ignore};
}

KHook::Return<bool> Loader::OnSendMessage(SendHook &hook, void *client, const CNetMessage *data, NetChannelBufType_t type)
{
	if (!data || tInsideMessage)
		return {KHook::Action::Ignore};
	void *const table = *reinterpret_cast<void *const *>(data);
	if (!IsMessageVTable(table, signonVTable_.load(std::memory_order_relaxed), engineSignonVTable_.load(std::memory_order_relaxed)))
		return {KHook::Action::Ignore};
	CNETMsg_SignonState *message = nullptr;
	std::string next;
	int slot = -1;
	{
		Guard guard(mutex_);
		if (!Ready() || !IsMessageVTable(table, signonVTable_, engineSignonVTable_))
			return {KHook::Action::Ignore};
		try
		{
			void *server = *reinterpret_cast<void **>(static_cast<uint8_t *>(client) + layout_.clientServerOffset);
			const auto [steamId, clientSlot] = Identity(client, server);
			slot = clientSlot;
			if (steamId == 0)
				return {KHook::Action::Ignore};
			Bind(slot, steamId);

			message = const_cast<CNetMessage *>(data)->ToPB<CNETMsg_SignonState>();
			const std::string addons = message->addons();
			std::optional<std::string> wanted;
			if (message->signon_state() == kSignonStateChangeLevel)
			{
				wanted = FirstAddon(addons);
				handshakes_.ChangingMap(steamId, wanted, Now());
				if (debug_)
					Log("Workshop map change slot %d: %s", slot, wanted ? wanted->c_str() : "no addons");
				if (!wanted || addons == *wanted)
					return {KHook::Action::Ignore};
			}
			else
			{
				const std::vector<std::string> required = Required(ReadServerAddons(server), kAddonId);
				wanted = handshakes_.Next(steamId, required, Now());
				if (!wanted)
				{
					CompleteWait(slot);
					return {KHook::Action::Ignore};
				}
				if (WaitForWorkshop(slot, steamId))
					return {KHook::Action::Supersede, false};
			}
			next = *wanted;
		}
		catch (const std::exception &ex)
		{
			Fault(ex.what());
			return {KHook::Action::Ignore};
		}
	}

	const int state = message->signon_state();
	const bool hadState = message->has_signon_state();
	const std::string addons = message->addons();
	const bool hadAddons = message->has_addons();
	bool result = false;
	bool failed = false;
	{
		Flag inside(tInsideMessage);
		message->set_signon_state(static_cast<SignonState_t>(kSignonStateChangeLevel));
		message->set_addons(next);
		try
		{
			result = hook.CallOriginal(client, data, type);
		}
		catch (...)
		{
			failed = true;
		}
		if (hadState)
			message->set_signon_state(static_cast<SignonState_t>(state));
		else
			message->clear_signon_state();
		if (hadAddons)
			message->set_addons(addons);
		else
			message->clear_addons();
	}

	Guard guard(mutex_);
	if (failed)
	{
		Fault("The engine signon message failed while sending a Workshop addon.");
		return {KHook::Action::Supersede, result};
	}
	++signons_;
	if (debug_)
		Log("Workshop signon slot %d: %s", slot, next.c_str());
	return {KHook::Action::Supersede, result};
}

void Loader::ClientConnect(int slot)
{
	Guard guard(mutex_);
	if (!Ready() || !getXuidFunction_ || slot < 0 || slot >= 256)
		return;
	try
	{
		const uint64_t steamId = GetClientXuid(slot);
		if (steamId == 0)
			return;
		NeedSendHook();
		sendSchedule_.Loading(slot, Now());
		Bind(slot, steamId);
		CompleteWait(slot);
		handshakes_.Connected(steamId, Now());
	}
	catch (const std::exception &ex)
	{
		Fault(ex.what());
	}
}

void Loader::ClientActive(int slot, uint64_t steamId)
{
	Guard guard(mutex_);
	if (!Ready())
		return;
	Bind(slot, steamId);
	CompleteWait(slot);
	handshakes_.Active(steamId, Now());
	++completed_;
	sendSchedule_.Loaded(slot);
	QueueSendRelease();
}

void Loader::ClientDisconnect(int slot)
{
	Guard guard(mutex_);
	CompleteWait(slot);
	if (auto it = slots_.find(slot); it != slots_.end())
	{
		const uint64_t steamId = it->second;
		slots_.erase(it);
		handshakes_.Disconnected(steamId, Now());
	}
	sendSchedule_.Loaded(slot);
	QueueSendRelease();
}

void Loader::Bind(int slot, uint64_t steamId)
{
	auto it = slots_.find(slot);
	if (it != slots_.end() && it->second != steamId)
	{
		CompleteWait(slot);
		handshakes_.Forget(it->second);
	}
	slots_[slot] = steamId;
}

bool Loader::WaitForWorkshop(int slot, uint64_t steamId)
{
	const bool expired = deadlines_.Wait(slot, steamId, Now());
	ArmDeadlineCheck(expired ? 0.0 : 1.0);
	return expired;
}

void Loader::ArmDeadlineCheck(double delay)
{
	if (deadlineCheckArmed_ || !Ready() || deadlines_.Count() == 0)
		return;
	deadlineCheckArmed_ = true;
	std::weak_ptr<bool> alive = alive_;
	scheduler_.After(delay, [this, alive] {
		if (auto flag = alive.lock(); flag && *flag)
			CheckDeadlines();
	});
}

void Loader::CheckDeadlines()
{
	std::vector<int> expired;
	{
		Guard guard(mutex_);
		deadlineCheckArmed_ = false;
		try
		{
			if (!Ready() || !armed_)
				return;
			const double now = Now();
			for (const ConnectionDeadlines::Waiting &pending : deadlines_.Expired(now))
			{
				const uint64_t currentSteamId = GetClientXuid(pending.slot);
				if (!deadlines_.TakeExpired(pending, currentSteamId, now))
					continue;
				slots_.erase(pending.slot);
				handshakes_.Forget(pending.steamId);
				expired.push_back(pending.slot);
				++timedOut_;
			}
			QueueSendRelease();
			ArmDeadlineCheck(1.0);
		}
		catch (const std::exception &ex)
		{
			Fault(ex.what());
			return;
		}
	}
	for (int slot : expired)
	{
		if (engine_)
			engine_->DisconnectClient(CPlayerSlot(slot), NETWORK_DISCONNECT_TIMEDOUT);
		LogWarning("Disconnected slot %d: required Workshop handshake timed out after %.0fs. Reconnect and accept the download.",
			slot, ConnectionDeadlines::kTimeoutSeconds);
	}
}

void Loader::Fault(const std::string &reason)
{
	if (faulted_)
		return;
	faulted_ = true;
	status_ = "Disabled: " + reason;
	LogError("Workshop loader disabled. FACEIT lookups remain available. %s", reason.c_str());
	std::weak_ptr<bool> alive = alive_;
	scheduler_.NextFrame([this, alive] {
		if (auto flag = alive.lock(); flag && *flag)
		{
			Guard guard(mutex_);
			if (!disposed_ && faulted_)
				Detach();
		}
	});
}

void Loader::Detach()
{
	for (KeptHook *hook : std::initializer_list<KeptHook *>{replyHook_, fillHook_, hostRequestHook_})
		if (hook)
			hook->Deactivate();
	for (SendHook *hook : sendHooks_)
		hook->Detach();
	armed_ = false;
	sendArmed_ = false;
	replyFunction_ = sendFunction_ = fillServerInfoFunction_ = hostRequestFunction_ = nullptr;
	sendSchedule_.Clear();
	signonVTable_ = serverInfoVTable_ = engineSignonVTable_ = engineServerInfoVTable_ = nullptr;
}

void Loader::Dispose()
{
	Guard guard(mutex_);
	if (disposed_)
		return;
	disposed_ = true;
	*alive_ = false;
	Detach();
	handshakes_.Clear();
	slots_.clear();
	deadlines_.Clear();
	status_ = "Stopped";
}

}
