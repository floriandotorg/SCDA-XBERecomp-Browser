#include "cpu/x86_context.h"
#include <cstdint>

// Game side functions mouse.cpp and keypad.cpp call that live in hooks a build may leave out: the chunk that selects
// the hook's source replaces these with its strong definitions
namespace game {
__attribute__((weak)) void PromptsTick(X86Context&) {}
__attribute__((weak)) bool ProjectModel(const float*, float&, float&) { return false; }
}
