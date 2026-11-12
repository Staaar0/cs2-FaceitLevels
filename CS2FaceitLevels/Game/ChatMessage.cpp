#include "Game/ChatMessage.h"

#include "Game/NetMessages.h"

#include <engine2/igameeventsystem.h>
#include <irecipientfilter.h>
#include <networksystem/inetworkmessages.h>
#include <shareddefs.h>

#include "usermessages.pb.h"

namespace cfl::game {
namespace {

IGameEventSystem *g_eventSystem = nullptr;

class SingleRecipientFilter final : public IRecipientFilter
{
public:
	explicit SingleRecipientFilter(int slot)
	{
		recipients_.ClearAll();
		recipients_.Set(slot);
	}

	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec &GetRecipients() const override { return recipients_; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return CPlayerSlot(-1); }

private:
	CPlayerBitVec recipients_;
};

}

void SetGameEventSystem(IGameEventSystem *system)
{
	g_eventSystem = system;
}

bool PrintToChat(int slot, const std::string &message)
{
	if (!g_eventSystem || slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT || message.empty())
		return false;
	INetworkMessageInternal *type = FindNetMessage("TextMsg");
	CNetMessage *net = AllocateNetMessage(type);
	if (!net)
		return false;
	CUserMessageTextMsg *text = net->ToPB<CUserMessageTextMsg>();
	const bool sent = text->GetTypeName() == "CUserMessageTextMsg";
	if (sent)
	{
		text->set_dest(HUD_PRINTTALK);
		text->add_param(message);
		SingleRecipientFilter filter(slot);
		g_eventSystem->PostEventAbstract(CSplitScreenSlot(-1), false, &filter, type, net, 0);
	}
	FreeNetMessage(net);
	return sent;
}

}
