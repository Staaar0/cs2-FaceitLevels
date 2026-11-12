#include "Game/NetMessages.h"

#include <interfaces/interfaces.h>
#include <networksystem/inetworkmessages.h>

#include <cctype>
#include <cstddef>

namespace cfl::game {
namespace {

constexpr int kFindNetMessagePartialIndex = 10;

struct NetMessageType
{
	const char *name;
	CNetMessage *(*allocate)();
};

const NetMessageType *Layout(INetworkMessageInternal *type)
{
	return reinterpret_cast<const NetMessageType *>(type);
}

bool ContainsNoCase(const char *text, const char *part)
{
	for (; *text; ++text)
	{
		size_t i = 0;
		while (part[i] && text[i] &&
			std::tolower(static_cast<unsigned char>(text[i])) == std::tolower(static_cast<unsigned char>(part[i])))
			++i;
		if (!part[i])
			return true;
	}
	return false;
}

}

INetworkMessageInternal *FindNetMessage(const char *partialName)
{
	if (!g_pNetworkMessages || !partialName || !*partialName)
		return nullptr;
	void **vtable = *reinterpret_cast<void ***>(g_pNetworkMessages);
	if (!vtable || !vtable[kFindNetMessagePartialIndex])
		return nullptr;
	using FindFn = INetworkMessageInternal *(*)(INetworkMessages *, const char *);
	INetworkMessageInternal *type =
		reinterpret_cast<FindFn>(vtable[kFindNetMessagePartialIndex])(g_pNetworkMessages, partialName);
	if (!type)
		return nullptr;
	const NetMessageType *layout = Layout(type);
	if (!layout->name || !layout->allocate || !ContainsNoCase(layout->name, partialName))
		return nullptr;
	return type;
}

CNetMessage *AllocateNetMessage(INetworkMessageInternal *type)
{
	if (!type || !Layout(type)->allocate)
		return nullptr;
	return Layout(type)->allocate();
}

void FreeNetMessage(CNetMessage *message)
{
	delete message;
}

}
