// The input waits of src/game/loading.cpp: a mission's loading picture waits for a key before the relaunch, as SCCT's
// PC loading screen does, any key answers the controller layout screen, the keyboard's confirm and back keys skip
// movies, and a button held when a movie starts does not skip it. The wait polls with a runtime checkpoint between
// polls, so stopping the runtime still unwinds it.
#include <bit>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>

#include "core/input.h"
#include "game/loading.h"
#include "gpu/backend.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

bool g_mission_load = false;  // game thread

// the next instance's progress bar, drawn full: black at 335,135, 4 high, 240 wide, then presented
constexpr uint32_t kRenderDevice = 0x5E3E2C, kDrawTile = 0x2E65A4;
constexpr float kBarX = 335, kBarY = 135, kBarWidth = 240, kBarHeight = 4;

void FullBar(X86Context& ctx) {
    uint32_t device = R32(kRenderDevice);
    if (!device) return;
    auto f = [](float v) { return std::bit_cast<uint32_t>(v); };
    uint32_t ecx = ctx.ecx;
    ctx.ecx = device;
    gpu::g_in_splash++;
    CallGuest(ctx, kDrawTile, {f(kBarX), f(kBarY), f(kBarX + kBarWidth), f(kBarY + kBarHeight), f(0), f(0), f(1), f(1), 0,
                               f(0), f(0), f(0)});
    gpu::g_in_splash--;
    ctx.ecx = ecx;
    CallGuest(ctx, SYM(D3DDevice_Swap), {0});
}

std::string Url(uint32_t text) {
    std::string url;
    for (uint32_t n = 0; text && n < 260; text += 2, n++) {
        uint16_t c = R16(text);
        if (!c) break;
        url += c < 0x80 ? char(std::tolower(c)) : '?';
    }
    return url;
}

std::string MapName(const std::string& url) {
    size_t at = url.find("mapname=");
    if (at == std::string::npos) return {};
    at += 8;
    return url.substr(at, url.find_first_of("? ", at) - at);
}

constexpr uint32_t kNetwork = 0x67296C;

bool Waits(const std::string& map) {
    return !map.empty() && map != "menu" && (map.find("_coop_") == std::string::npos || !R32(kNetwork));
}

// a key, click or pad button pressed and let go in the game
void WaitForPress(X86Context& ctx) {
    input::SetMenu(false);
    input::PressReleased();
    auto& runtime = browser::Runtime::From(ctx);
    while (!input::PressReleased()) {
        runtime.Checkpoint(ctx);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGI("loading: the wait ended by a press");
}

}  // namespace

// front end: loading screen, launch data, relaunch
X86_EXTERN_FUNC(__imp__sub_000444B4);
X86_FUNC(sub_000444B4) {
    g_mission_load = true;
    __imp__sub_000444B4(ctx, base);
    g_mission_load = false;
}

// launch data writer (type, url, launch), thiscall; launch relaunches the title
X86_EXTERN_FUNC(__imp__sub_0026AB34);
X86_FUNC(sub_0026AB34) {
    if (g_mission_load && R32(ctx.esp + 12)) {
        g_mission_load = false;
        std::string url = Url(R32(ctx.esp + 8));
        bool wait = Waits(MapName(url));
        LOGI("loading: %s%s", url.c_str(), wait ? ", waiting for a key" : "");
        if (wait) {
            FullBar(ctx);
            FullBar(ctx);
            WaitForPress(ctx);
        }
    }
    __imp__sub_0026AB34(ctx, base);
}

// the controller layout screen polls the movies' skip check until it answers: any key, click or pad button released
namespace {
bool g_controls_screen = false;  // game thread
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00038C3F);
X86_FUNC(sub_00038C3F) {
    g_controls_screen = true;
    __imp__sub_00038C3F(ctx, base);
    g_controls_screen = false;
}

X86_EXTERN_FUNC(__imp__sub_000367E4);
X86_FUNC(sub_000367E4) {
    __imp__sub_000367E4(ctx, base);
    bool answer = g_controls_screen && input::PressReleased();
    if (!ctx.eax && answer) ctx.eax = 1;
}

// the movies' skip check (thiscall viewport), A or Start let go on any pad; the keyboard's confirm and back keys skip
// too, and a button down when the movie started is hidden until let go
namespace {
constexpr uint32_t kSkipState = 0x5E76EC, kSkipPort = 0x2C, kSkipStart = 0x0C, kSkipPorts = 4;
constexpr uint32_t kMovieFirstCheck = 0x39908;
uint32_t g_held_at_start = 0;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0004B46F);
X86_FUNC(sub_0004B46F) {
    bool first = R32(ctx.esp) == kMovieFirstCheck;
    __imp__sub_0004B46F(ctx, base);
    for (uint32_t i = 0; i < kSkipPorts * 2; i++) {
        uint8_t& state = G<uint8_t>(kSkipState + i / 2 * kSkipPort + i % 2 * kSkipStart);
        bool down = state >= 0x32;
        if (first && down) g_held_at_start |= 1u << i;
        if (!(g_held_at_start >> i & 1)) continue;
        if (down) state = 0;
        else g_held_at_start &= ~(1u << i);
    }
    if (!ctx.eax && input::SkipKeyReleased()) ctx.eax = 1;
}

// front end video frame (thiscall, 2 arguments): a mission's outro plays here, the input page still the review's
X86_EXTERN_FUNC(__imp__sub_00038F96);
X86_FUNC(sub_00038F96) {
    input::MovieFrame();
    game::loading::g_movie = true;
    __imp__sub_00038F96(ctx, base);
    game::loading::g_movie = false;
}
