#include "Runtime/Hooks.h"

namespace cfl {

void HookKeeper::DeactivateAll()
{
	for (KeptHook *hook : hooks_)
		hook->Deactivate();
}

}
