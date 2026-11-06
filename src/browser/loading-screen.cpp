// The parts of src/game/loading.cpp hud.cpp's ShowScreen hook calls: which picture ShowScreen loads, and black edge
// columns on a 4:3 loading picture so a fade from a full width frame clamps to black past its sides. The picture is
// GPU visible memory, written through a sized span so every page it touches is uploaded again.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "app/app.h"
#include "game/loading.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// ShowScreen (sub_002E5F95) closes the TGA (sub_00051673) right after expanding it into the picture texture: its
// frame's ebp+0x44 is the texture header (+4 data), +0x58 the width, +0x64 the height; LIN_A8R8G8B8
constexpr uint32_t kPictureDecoded = 0x2E63BD, kPictureHeader = 0x44, kPictureWidth = 0x58, kPictureHeight = 0x64;
bool g_edges = true;  // the picture ShowScreen is loading is shown 4:3 (the ESRB notice is not)

void PaintEdges(uint32_t frame) {
  if (R32(frame + kPictureWidth) != 640 || R32(frame + kPictureHeight) != 480) { return; }
  const uint32_t pitch = 640 * 4;
  auto* picture = browser::hooks::Span(mem::VirtualFromPhysical(R32(frame + kPictureHeader + 4)), pitch * 480);
  for (uint32_t y = 0; y < 480; ++y) {
    for (uint32_t x : {0u, 639u}) { std::memset(picture + y * pitch + x * 4, 0, 3); }
  }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00051673);
X86_FUNC(sub_00051673) {
  const bool picture = R32(ctx.esp) == kPictureDecoded;
  __imp__sub_00051673(ctx, base);
  if (picture && g_edges && app::Widescreen()) { PaintEdges(ctx.ebp); }
}

void game::loading::BeforeShowScreen(X86Context& ctx) {
  const uint32_t p = R32(ctx.esp + 4);
  if (!p) { return; }
  const bool wide = !R8(p + 1);
  std::string path;
  for (uint32_t n = 0; n < 519; ++n) {
    const uint32_t c = wide ? R16(p + n * 2) : R8(p + n);
    if (!c) { break; }
    path += char(std::tolower(int(c & 0xFF)));
  }
  g_edges = path.find("esrb") == std::string::npos;
}

void game::loading::AfterShowScreen(X86Context&) {}

// ShowScreen with no picture, kept display or fade: the game's own, whose copies go through the console sized buffers
// the browser draws at
X86_EXTERN_FUNC(__imp__sub_002E5F95);
void game::loading::BothBuffers(X86Context& ctx, uint32_t) { __imp__sub_002E5F95(ctx, browser::hooks::Base()); }
