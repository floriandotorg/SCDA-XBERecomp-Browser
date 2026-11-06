#include "cpu/x86_context.h"
#include <cstdint>

// Game side functions mouse.cpp and keypad.cpp call that live in hooks a build may leave out: the chunk that selects
// the hook's source replaces these with its strong definitions
namespace game {
__attribute__((weak)) void PromptsTick(X86Context&) {}
__attribute__((weak)) bool ProjectModel(const float*, float&, float&) { return false; }
}

// prompts.cpp's element draws (C04): nothing relabelled, the element is drawn as the game has it
namespace game {
__attribute__((weak)) void EventPrompts(X86Context&, uint32_t) {}
__attribute__((weak)) bool ComboElement(X86Context&, uint8_t*, uint32_t) { return false; }
__attribute__((weak)) bool HintElement(X86Context&, uint8_t*, uint32_t) { return false; }
__attribute__((weak)) bool WordingElement(X86Context&, uint8_t*, uint32_t) { return false; }
}

// the page draws its own frame rate counter and no watermark
namespace app {
__attribute__((weak)) void DrawFps(X86Context&) {}
__attribute__((weak)) void DrawWatermark(X86Context&) {}
}
