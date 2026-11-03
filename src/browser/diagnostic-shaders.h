#pragma once
#include "browser/cpu_runtime.h"
#include "gpu/state.h"

namespace browser {

gpu::VertexShader ReadVertexShader(Memory& memory, const X86Context& ctx, uint32_t handle, uint32_t declaration, uint32_t function);
gpu::PixelShader ReadPixelShader(Memory& memory, const X86Context& ctx, uint32_t handle, uint32_t definition);
void ApplyDiagnosticPixelConstants(gpu::State& state, uint32_t first, uint32_t count);

}
