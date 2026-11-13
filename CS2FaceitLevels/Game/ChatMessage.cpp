#include "Game/ChatMessage.h"

#include <engine2/igameeventsystem.h>
#include <interfaces/interfaces.h>
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
	if (!g_eventSystem || !g_pNetworkMessages || slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT || message.empty())
		return false;
	NetMessageInfo_t *info = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	CNetMessage *net = info ? info->AllocateMessage() : nullptr;
	if (!net)
		return false;
	CUserMessageTextMsg *text = net->ToPB<CUserMessageTextMsg>();
	const bool sent = text->GetTypeName() == "CUserMessageTextMsg";
	if (sent)
	{
		text->set_dest(HUD_PRINTTALK);
		text->add_param(message);
		SingleRecipientFilter filter(slot);
		g_eventSystem->PostEventAbstract(CSplitScreenSlot(-1), false, &filter, info, net, 0);
	}
	delete net;
	return sent;
}

}
