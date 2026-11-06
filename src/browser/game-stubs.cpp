#include "cpu/x86_context.h"

// the page draws its own frame rate counter and no watermark
namespace app {
__attribute__((weak)) void DrawFps(X86Context&) {}
__attribute__((weak)) void DrawWatermark(X86Context&) {}
}
