#pragma once

#include "Runtime/Hooks.h"
#include "Workshop/AddonHandshake.h"
#include "Workshop/EngineLayout.h"

#include <networksystem/networksystemtypes.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class CNetMessage;
class IVEngineServer2;

namespace cfl {
class Scheduler;
}

namespace cfl::workshop {

class Loader
{
public:
	Loader(std::string directory, Scheduler &scheduler, HookKeeper &hooks, IVEngineServer2 *engine,
		std::optional<std::string> runtimeGamedata, const bool &debug);
	~Loader();

	void Start();
	void RetryStartup();
	bool CanRetryStartup() const;
	std::string Summary() const;
	std::optional<std::string> RuntimeGamedata() const;

	static bool OtherLoaderPresent();
	static bool MamHasClientBadge();
	static bool MamHasBadge();

	void MapStarted();
	void ClientConnect(int slot);
	void ClientActive(int slot, uint64_t steamId);
	void ClientDisconnect(int slot);

	void Dispose();

private:
	using ReplyHook = FunctionHook<Loader, void, void *, void *>;
	using SendHook = FunctionHook<Loader, bool, void *, const CNetMessage *, NetChannelBufType_t>;
	using FillHook = FunctionHook<Loader, void, void *, void *>;
	using HostRequestHook = FunctionHook<Loader, void, void *, void *>;

	KHook::Return<void> OnReply(ReplyHook &hook, void *server, void *client);
	KHook::Return<bool> OnSendMessage(SendHook &hook, void *client, const CNetMessage *data, NetChannelBufType_t type);
	KHook::Return<void> OnFillServerInfo(FillHook &hook, void *server, void *info);
	KHook::Return<void> OnHostRequest(HostRequestHook &hook, void *manager, void *request);

	using Guard = std::lock_guard<std::recursive_mutex>;

	bool Ready() const { return !disposed_ && !faulted_; }
	std::pair<uint64_t, int> Identity(void *client, void *server);
	uint64_t GetClientXuid(int slot);
	std::string ReadServerAddons(void *server) const;
	template <typename Hook>
	Hook *ArmHook(Hook *current, void *address, typename Hook::Handler pre, typename Hook::Handler post);
	void ArmSendHook();
	void NeedSendHook();
	void QueueSendRelease();
	void ReleaseSendHook();
	void Bind(int slot, uint64_t steamId);
	bool WaitForWorkshop(int slot, uint64_t steamId);
	void CompleteWait(int slot) { deadlines_.Remove(slot); }
	void ArmDeadlineCheck(double delay);
	void CheckDeadlines();
	void Fault(const std::string &reason);
	void Detach();
	void VerifyMessages();

	std::string directory_;
	Scheduler &scheduler_;
	HookKeeper &hooks_;
	IVEngineServer2 *engine_;
	const bool &debug_;
	std::optional<std::string> runtimeGamedata_;
	std::shared_ptr<bool> alive_;
	mutable std::recursive_mutex mutex_;

	AddonHandshake handshakes_;
	Slots slots_;
	ConnectionDeadlines deadlines_;
	SendHookSchedule sendSchedule_;
	EngineLayout layout_;

	ReplyHook *replyHook_ = nullptr;
	SendHook *sendHook_ = nullptr;
	std::vector<SendHook *> sendHooks_;
	FillHook *fillHook_ = nullptr;
	HostRequestHook *hostRequestHook_ = nullptr;
	void *replyFunction_ = nullptr;
	void *sendFunction_ = nullptr;
	void *fillServerInfoFunction_ = nullptr;
	void *hostRequestFunction_ = nullptr;
	std::atomic<void *> signonVTable_{nullptr};
	void *serverInfoVTable_ = nullptr;
	std::atomic<void *> engineSignonVTable_{nullptr};
	void *engineServerInfoVTable_ = nullptr;
	void *engineInterface_ = nullptr;
	void *getXuidFunction_ = nullptr;

	bool armed_ = false;
	bool sendArmed_ = false;
	bool releaseQueued_ = false;
	bool deadlineCheckArmed_ = false;
	bool disposed_ = false;
	bool faulted_ = false;
	bool started_ = false;
	std::string status_ = "Not started";
	uint64_t replies_ = 0, signons_ = 0, completed_ = 0, mountLists_ = 0, mapRequests_ = 0;
	uint64_t missingXuids_ = 0, timedOut_ = 0, sendAttaches_ = 0;
};

}
