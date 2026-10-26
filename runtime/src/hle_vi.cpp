// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is the platform's window
// (Win32 window / Android surface); its handle is the "native window" the game hands to vkCreateViSurfaceNN, which
// the Vulkan bridge turns into the platform's surface.
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx {

// NX_DISPLAY_HZ=<hz> (30..360; study fps-unlock-1710): the host vsync event and the present unit run at <hz> instead of
// 60. Experimental: Wonder's logic steps one 1/60 s unit per present interval, so above 60 the game runs fast.
uint64_t display_period_ns() {
    static const uint64_t p = [] {
        const char* e = std::getenv("NX_DISPLAY_HZ");
        int hz = e ? std::atoi(e) : 0;
        if (hz < 30 || hz > 360) return uint64_t(16'666'667);
        log("vi: display at %d Hz (NX_DISPLAY_HZ)", hz);
        return uint64_t(1'000'000'000ull / uint64_t(hz));
    }();
    return p;
}

// The real display's vblanks (plat::wait_vblank), when it refreshes at 60 Hz or a multiple of it (120, 240: every 2nd,
// 4th one is a 60 Hz tick). A timed 60 Hz tick drifts against the display's own (59.94 / 60.02 Hz panels): every so
// often a frame lands on the other side of a vblank and shows for 3 vblanks, the next for 1, a hitch felt in motion
// although the count stays at 30 fps. With them, the vsync event fires on the display's ticks, the present pacing
// uses their period, and each present is held until the vblank before its own (vblank_hold).
// NX_REAL_VSYNC=0: the timed tick, as before.
std::atomic<uint64_t> g_vblanks{0};       // real vblanks seen
std::atomic<int> g_per_tick{0};           // real vblanks per 60 Hz tick; 0: none (timed)
std::atomic<uint64_t> g_tick_period{0};   // the measured 60 Hz tick, ns
std::atomic<int> g_present_interval{2};   // the window's present interval, in ticks (nvn_queue.cpp)
std::mutex g_vb_lock;
std::condition_variable g_vb_cv;

uint64_t tick_period_ns() {
    uint64_t p = g_tick_period.load(std::memory_order_relaxed);
    return p ? p : display_period_ns();
}

void set_present_interval(int interval) { g_present_interval.store(interval, std::memory_order_relaxed); }

// Called right before an image is queued for display: it waits until the vblank before the one this image is due at
// (the previous image's + interval ticks), so it shows exactly there; a late image shows at the next vblank.
void vblank_hold() {
    const int k = g_per_tick.load(std::memory_order_relaxed), interval = g_present_interval.load(std::memory_order_relaxed);
    if (!k || interval <= 0) return;
    static uint64_t last = 0;
    const uint64_t seen = g_vblanks.load(std::memory_order_acquire);
    uint64_t due = last + uint64_t(interval) * uint64_t(k);
    if (!last || due <= seen) due = seen + 1;
    if (due - 1 > seen) {
        std::unique_lock lk(g_vb_lock);
        g_vb_cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return g_vblanks.load(std::memory_order_acquire) >= due - 1; });
    }
    last = due;
}

namespace {

constexpr uint64_t kFakeDisplay = kKernelBase - 0x1000;  // opaque pointers, never dereferenced by the game
constexpr uint64_t kFakeLayer = kKernelBase - 0x2000;

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], kFakeDisplay);
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    void* w = plat::create_window();
    log("vi: layer created -> native window %p", w);
    plat::release_window(w);
    st<uint64_t>(c.x[0], kFakeLayer);
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(plat::create_window()));
    c.x[0] = 0;
}

void nv_noop(CPU&) {}

// The display's vsync: a kernel event a host thread signals every 1/60 s. The game gets it as an nn::os
// SystemEvent (AttachSystemEvent with our read/write handles, auto clear: each wait consumes one vsync).
std::shared_ptr<EventState> g_vsync = std::make_shared<EventState>();

// Drives the vsync event from the display's vblanks; returns when there are none (or they stop) to fall back to the
// timed tick.
void real_vsync() {
    if (std::getenv("NX_DISPLAY_HZ")) return;
    if (const char* e = std::getenv("NX_REAL_VSYNC"); e && e[0] == '0') return;
    uint64_t t0 = plat::wait_vblank(), t = t0;
    if (!t0) return;
    for (int i = 0; i < 30; i++)
        if (!(t = plat::wait_vblank())) return;
    double p = double(t - t0) / 30.0;
    double r = 16'666'667.0 / p;
    int k = int(std::lround(r));
    if (k < 1 || k > 4 || std::fabs(r - k) > 0.03 * k) {
        log("vi: the display refreshes at %.2f Hz, not a multiple of 60: timed vsync", 1e9 / p);
        return;
    }
    log("vi: vsync from the display's vblanks (%.2f Hz, %d per 60 Hz tick)", 1e9 / p, k);
    double period = p;
    uint64_t prev = t;
    g_tick_period = uint64_t(period * k);
    g_per_tick = k;
    for (uint64_t n = 0;;) {
        uint64_t now = plat::wait_vblank();
        if (!now) break;
        double d = double(now - prev);
        prev = now;
        if (d > 0.5 * p && d < 1.5 * p) period = period * 0.99 + d * 0.01;
        {
            std::lock_guard lk(g_vb_lock);
            g_vblanks.fetch_add(1, std::memory_order_release);
        }
        g_vb_cv.notify_all();
        if (++n % uint64_t(k)) continue;
        g_tick_period.store(uint64_t(period * k), std::memory_order_relaxed);
        g_vsync->signaled = true;
        notify_all();
    }
    g_per_tick = 0;
    g_tick_period = 0;
    log("vi: the display's vblanks stopped: timed vsync");
}

void vsync_thread() {
    plat::set_thread_name("vsync");
    real_vsync();
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += display_period_ns();
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        g_vsync->signaled = true;
        notify_all();
    }
}

void GetDisplayVsyncEvent(CPU& c) {  // (os::SystemEventType*, Display*)
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = g_vsync;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEventType; 1: AutoClear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    static std::once_flag once;
    std::call_once(once, [] { std::thread(vsync_thread).detach(); });
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", GetDisplayVsyncEvent);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_noop);

}  // namespace nx
