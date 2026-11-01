#pragma once
#include "browser/dispatcher-handles.h"

namespace browser {

void InvokeDispatcherObject(DispatcherHandles& handles, Memory& memory, X86Context& ctx, uint32_t ordinal);

}
