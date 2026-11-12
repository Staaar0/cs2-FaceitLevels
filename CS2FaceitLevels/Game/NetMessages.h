#pragma once

class CNetMessage;
class INetworkMessageInternal;

namespace cfl::game {

INetworkMessageInternal *FindNetMessage(const char *partialName);
CNetMessage *AllocateNetMessage(INetworkMessageInternal *type);
void FreeNetMessage(CNetMessage *message);

}
