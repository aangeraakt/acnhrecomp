// macOS host (Apple silicon): memory (mmap at fixed addresses), pthreads, an SDL3 window with a Metal layer for
// MoltenVK (VK_EXT_metal_surface), SDL3 gamepad + keyboard, SDL3 audio, a signal crash handler and main.
//
// Cocoa owns the main thread: main() starts the game (nx::boot) on its own thread and then runs the SDL event loop.
// Anything that touches the window (creating it, its Metal view) is done there; the game's threads ask for it and
// wait. Input is sampled on the main thread into a snapshot read_pad() copies.
//
// Data: NX_DATA_ROOT or games/<game>'s pc_data_root ("private/<game>"): analysis/{main,sdk}.bin, extract/romfs, user/.
// Settings: NX_<KEY> environment variables, as on Windows.
#include <dlfcn.h>
#include <pthread.h>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_switch.h>
#include <pthread/qos.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "config.h"
#include "ui/ui.h"
#include "bug_report.h"
#include "platform.h"

namespace nx {
void boot();
}

namespace nx::plat {

namespace {

// Work for the main thread (window creation), run between event polls.
std::mutex g_main_lock;
std::deque<std::function<void()>> g_main_work;

void run_on_main(std::function<void()> fn) {
    if (pthread_main_np()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard lk(g_main_lock);
        g_main_work.push_back([&] {
            fn();
            std::lock_guard l2(m);
            done = true;
            cv.notify_one();
        });
    }
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return done; });
}

SDL_Window* g_window = nullptr;
SDL_MetalView g_view = nullptr;
void* g_layer = nullptr;  // CAMetalLayer*
std::atomic<bool> g_minimized{false};
std::atomic<uint32_t> g_pixel_w{0}, g_pixel_h{0};

std::mutex g_pad_lock;
PadState g_pad;

}  // namespace
void update_refresh_hz();  // below, with display_refresh_hz

// ---- memory ---------------------------------------------------------------------------------------------------

// No MAP_FIXED_NOREPLACE on Darwin: ask for the address as a hint and give it back if the kernel chose another.
// macOS 27 ignores mmap's address hint below 0x70_0000_0000 (the mapping lands up there), and MAP_FIXED would
// silently replace whatever is already mapped. mach_vm_allocate with VM_FLAGS_FIXED takes the exact range or fails
// when any of it is in use.
bool reserve(uint64_t addr, uint64_t size) {
    mach_vm_address_t a = addr;
    if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS) {
        mach_vm_address_t r = addr;  // name what is in the way (svcs and the guest layout are in runtime.h)
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &obj) ==
            KERN_SUCCESS)
            nx::log("memory: %llx (+%llx) overlaps a mapping at %llx (+%llx)", (unsigned long long)addr,
                    (unsigned long long)size, (unsigned long long)r, (unsigned long long)rs);
        return false;
    }
    if (mprotect(reinterpret_cast<void*>(addr), size, PROT_NONE) != 0) {
        mach_vm_deallocate(mach_task_self(), addr, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize()) - 1;  // 16 KiB on Apple silicon
    uint64_t a = addr & ~page, e = (addr + size + page) & ~page;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

// The guest frees 4 KiB pages; Apple silicon pages are 16 KiB, and mprotect would take the neighbours of a partly
// freed page with it (SIGBUS in Wonder when nn::os's VAMM unmapped single pages). Only whole host pages are
// released; the freed part of a shared one is zeroed, which is what the guest would see if it maps it again.
void decommit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize());
    uint64_t lo = (addr + page - 1) & ~(page - 1), hi = (addr + size) & ~(page - 1);
    if (lo >= hi) {
        std::memset(reinterpret_cast<void*>(addr), 0, size);
        return;
    }
    if (addr < lo) std::memset(reinterpret_cast<void*>(addr), 0, lo - addr);
    if (hi < addr + size) std::memset(reinterpret_cast<void*>(hi), 0, addr + size - hi);
    madvise(reinterpret_cast<void*>(lo), hi - lo, MADV_FREE);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    // The game's threads (render, jobs, audio) are latency critical: default QoS lets macOS run them on the M1's
    // efficiency cores and stretch their sleeps and wakeups.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack_size + 0x3FFF) & ~size_t(0x3FFF));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) { _exit(code); }

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

// macOS sched_yield drops the caller to priority 0 for a while: measured 20-180 us per call on the M1, and Wonder's
// job system yields after every job item and in its barriers, so the main thread spent ~60% of its frame there and
// the game's governor fell to 30 fps (docs/nvn/DEBUG.md #44). thread_switch with no hint (2-4 us) gives the core to
// another ready thread without that penalty. NX_YIELD=sched: the old call.
void yield() {
    static const bool sched = [] {
        const char* e = std::getenv("NX_YIELD");
        return e && std::strcmp(e, "sched") == 0;
    }();
    if (sched)
        sched_yield();
    else
        thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
}
void set_thread_name(const char* name) { pthread_setname_np(name); }  // Darwin: the calling thread only

int64_t current_thread_id() {
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return int64_t(tid);
}

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }  // use Instruments

uint64_t monotonic_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? r : nx::game::kInfo.pc_data_root;
}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- window ---------------------------------------------------------------------------------------------------

bool headless() {
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void* create_window() {
    run_on_main([] {
        if (g_window) return;
        int w = 1280, h = 720;
        std::string r = setting("resolution");
        if (!r.empty()) std::sscanf(r.c_str(), "%dx%d", &w, &h);
        g_window = SDL_CreateWindow(nx::game::kInfo.title, w, h,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL |
                                        (headless() ? SDL_WINDOW_HIDDEN : 0));
        if (!g_window) {
            log_line(SDL_GetError());
            return;
        }
        if (!headless()) {
            SDL_RaiseWindow(g_window);  // without an app bundle it does not come to the front
            // The saved choice (video.fullscreen): the window opened in a window though the settings said full screen.
            if (setting("fullscreen") == "1") SDL_SetWindowFullscreen(g_window, true);
        }
        g_view = SDL_Metal_CreateView(g_window);
        g_layer = SDL_Metal_GetLayer(g_view);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        g_pixel_w = uint32_t(pw), g_pixel_h = uint32_t(ph);
        update_refresh_hz();
    });
    return g_layer;  // the Vulkan surface is made from the CAMetalLayer, from any thread
}

void release_window(void*) {}
uint64_t window_generation() { return 1; }
bool window_minimized() { return g_minimized; }

void window_size(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }
float default_render_scale() { return 1.0f; }

// ---- input: first SDL gamepad + keyboard (sampled on the main thread) -----------------------------------------

// Input options from config.ini (the settings screen writes them), re-read when the config changes.
struct InputOptions {
    bool xbox_layout = false;  // confirm on the bottom face button (Xbox/PlayStation habit)
    int deadzone = 7849;       // of 32767
    bool rumble = true;
    SDL_Scancode keys[22] = {};  // the order of kKeyNames
};
const char* const kKeyNames[22][2] = {
    {"move_up", "W"},     {"move_down", "S"},  {"move_left", "A"},  {"move_right", "D"},  {"cam_up", "I"},
    {"cam_down", "K"},    {"cam_left", "J"},   {"cam_right", "L"},  {"a", "Space"},       {"b", "Left Shift"},
    {"x", "X"},           {"y", "F"},          {"l", "Q"},          {"r", "E"},           {"zl", "Z"},
    {"zr", "C"},          {"plus", "Return"},  {"minus", "Escape"}, {"dpad_up", "Up"},    {"dpad_down", "Down"},
    {"dpad_left", "Left"}, {"dpad_right", "Right"},
};
const InputOptions& input_options() {
    static InputOptions o;
    static uint64_t gen = 0;
    if (uint64_t g = nx::config_generation(); g != gen) {
        gen = g;
        o.xbox_layout = nx::config_get("input.layout", "nintendo") == "xbox";
        o.deadzone = std::clamp(nx::config_get_int("input.deadzone", 24), 5, 50) * 32767 / 100;
        o.rumble = nx::config_get_bool("input.rumble", true);
        for (int k = 0; k < 22; k++) {
            std::string name = nx::config_get(std::string("input.key.") + kKeyNames[k][0], kKeyNames[k][1]);
            SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            o.keys[k] = sc != SDL_SCANCODE_UNKNOWN ? sc : SDL_GetScancodeFromName(kKeyNames[k][1]);
        }
    }
    return o;
}

SDL_Gamepad* g_gamepad = nullptr;

static void sample_input() {
    PadState p;
    const InputOptions& opt = input_options();
    SDL_Gamepad*& pad = g_gamepad;
    static uint64_t next_scan = 0;  // no controller: look for one 4 times a second, not on every loop
    if (!pad && monotonic_ns() >= next_scan) {
        next_scan = monotonic_ns() + 250'000'000;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (pad) nx::log("input: controller %s", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
    }
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
    if (pad) {
        // SDL names buttons by position (south/east/...). Nintendo layout: the game's A is the east button, as on a
        // Switch controller. Xbox layout: A is the bottom one (confirm where Xbox and PlayStation players expect it).
        struct Map {
            SDL_GamepadButton b;
            uint64_t bit;
        };
        static const Map map[] = {
            {SDL_GAMEPAD_BUTTON_NORTH, kX},          {SDL_GAMEPAD_BUTTON_WEST, kY},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
            {SDL_GAMEPAD_BUTTON_START, kPlus},       {SDL_GAMEPAD_BUTTON_BACK, kMinus},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        };
        for (const Map& m : map)
            if (SDL_GetGamepadButton(pad, m.b)) p.buttons |= m.bit;
        bool east = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST), south = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
        if (opt.xbox_layout ? south : east) p.buttons |= kA;
        if (opt.xbox_layout ? east : south) p.buttons |= kB;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) p.buttons |= kZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) p.buttons |= kZR;
        auto axis = [&](SDL_GamepadAxis a, bool invert) {
            int v = SDL_GetGamepadAxis(pad, a);
            if (v > -opt.deadzone && v < opt.deadzone) return 0;
            v = std::clamp(v, -kStickMax, kStickMax);
            return invert ? -v : v;
        };
        p.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
        p.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: down is positive; Horizon: up is positive
        p.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
        p.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
    }
    // Keyboard: the bindings from config.ini (defaults laid out for ACNH: WASD left stick, IJKL camera, arrows
    // D-pad, Space A, Shift B, X X, F Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -).
    if (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        auto key = [&](int i) { return k[opt.keys[i]]; };
        if (key(2)) p.lx = -kStickMax;
        if (key(3)) p.lx = kStickMax;
        if (key(0)) p.ly = kStickMax;
        if (key(1)) p.ly = -kStickMax;
        if (key(6)) p.rx = -kStickMax;
        if (key(7)) p.rx = kStickMax;
        if (key(4)) p.ry = kStickMax;
        if (key(5)) p.ry = -kStickMax;
        static const uint64_t bits[14] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight};
        for (int i = 0; i < 14; i++)
            if (key(8 + i)) p.buttons |= bits[i];
        if (k[SDL_SCANCODE_BACKSPACE] && opt.keys[17] == SDL_SCANCODE_ESCAPE) p.buttons |= kMinus;
    }
    // The settings screen takes the input while it is open (and watches for its Minus + Plus combo).
    bool ui = nx::ui::ui_pad(p.buttons, p.lx, p.ly);
    if (ui) p = PadState{};
    // The mouse: its left button touches the console's screen where the picture is (the game's own touch support).
    if (g_window) {
        float mx = 0, my = 0;
        int ww = 0, wh = 0;
        SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        SDL_GetWindowSize(g_window, &ww, &wh);
        bool focus = SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS;
        nx::plat::set_mouse_touch(!ui && focus && (mb & SDL_BUTTON_LMASK) && ww > 0 && wh > 0, mx / float(ww), my / float(wh));
    }
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
}

// ---- settings screen hooks (platform.h) --------------------------------------------------------------------------

std::atomic<float> g_volume{-1.0f};  // master volume 0..1 (-1: not read from config.ini yet; audio_cb applies it)

// The refresh rate of the window's display, read on the main thread when the window is made and when it moves to
// another display (nvn_vk.cpp presents each game frame for that many vsyncs).
std::atomic<int> g_refresh_hz{60};
void update_refresh_hz() {
    if (!g_window) return;
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)); m && m->refresh_rate > 20)
        g_refresh_hz = int(m->refresh_rate + 0.5f);
}
int display_refresh_hz() { return g_refresh_hz.load(); }

void set_fullscreen(bool on) {
    run_on_main([on] {
        if (g_window) SDL_SetWindowFullscreen(g_window, on);
    });
}

void set_master_volume(float v) { g_volume = std::clamp(v, 0.0f, 1.0f); }

std::string controller_name() {
    SDL_Gamepad* pad = g_gamepad;
    const char* n = pad ? SDL_GetGamepadName(pad) : nullptr;
    return n ? n : "";
}

std::string controller_kind() {
    SDL_Gamepad* pad = g_gamepad;
    if (!pad) return "";
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return "xbox";
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return "playstation";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "switch";
        default: return "generic";
    }
}

void rumble(float low_freq, float high_freq, uint32_t duration_ms) {
    uint16_t low16 = static_cast<uint16_t>(std::clamp(low_freq, 0.0f, 1.0f) * 65535.0f);
    uint16_t high16 = static_cast<uint16_t>(std::clamp(high_freq, 0.0f, 1.0f) * 65535.0f);
    std::lock_guard lk(g_main_lock);
    g_main_work.push_back([low16, high16, duration_ms] {
        if (g_gamepad) SDL_RumbleGamepad(g_gamepad, low16, high16, duration_ms);
    });
}

void open_path(const std::string& path) {
    std::string url = "file://" + path;
    if (!path.empty() && path[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) url = std::string("file://") + cwd + "/" + path;
    }
    run_on_main([url] { SDL_OpenURL(url.c_str()); });
}

namespace {
std::mutex g_pick_lock;
bool g_pick_done = false;
std::string g_pick_path;
void SDLCALL picked(void*, const char* const* list, int) {
    std::lock_guard lk(g_pick_lock);
    g_pick_path = list && list[0] ? list[0] : "";
    g_pick_done = true;
}
}  // namespace

void pick_path(bool zip, const std::string& title) {
    {
        std::lock_guard lk(g_pick_lock);
        g_pick_done = false;
    }
    run_on_main([zip, title] {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_window);
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (zip) {
            static const SDL_DialogFileFilter filter = {"ZIP", "zip"};
            SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(&filter));
            SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
        }
        SDL_ShowFileDialogWithProperties(zip ? SDL_FILEDIALOG_OPENFILE : SDL_FILEDIALOG_OPENFOLDER, picked, nullptr, props);
        SDL_DestroyProperties(props);
    });
}

bool pick_result(std::string& path) {
    std::lock_guard lk(g_pick_lock);
    if (!g_pick_done) return false;
    g_pick_done = false;
    path = g_pick_path;
    return true;
}

void read_pad(PadState& p) {
    std::lock_guard lk(g_pad_lock);
    p.buttons |= g_pad.buttons;
    if (g_pad.lx) p.lx = g_pad.lx;
    if (g_pad.ly) p.ly = g_pad.ly;
    if (g_pad.rx) p.rx = g_pad.rx;
    if (g_pad.ry) p.ry = g_pad.ry;
    input_script(p);
}

// ---- audio: SDL3 stream, s16 stereo 48 kHz, fed from the queue of submitted buffers ----------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
AudioDone g_done = nullptr;
SDL_AudioStream* g_stream = nullptr;
std::mutex g_audio;
std::deque<Pending> g_pending;

// Master volume (settings screen, live) and the mute while the window is in the background (audio.mute_background).
void apply_gain(int16_t* pcm, int count) {
    float v = g_volume.load(std::memory_order_relaxed);
    if (v < 0) {
        v = std::clamp(nx::config_get_int("audio.volume", 100), 0, 100) / 100.0f;
        g_volume = v;
    }
    if (g_window && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) &&
        nx::config_get_bool("audio.mute_background", false))
        v = 0;
    if (v >= 0.999f) return;
    for (int i = 0; i < count; i++) pcm[i] = int16_t(float(pcm[i]) * v);
}

void SDLCALL audio_cb(void*, SDL_AudioStream* s, int additional, int) {
    uint8_t buf[4096];
    while (additional > 0) {
        int n = std::min<int>(additional, sizeof buf);
        int filled = 0;
        std::vector<void*> finished;
        {
            std::lock_guard lk(g_audio);
            while (filled < n && !g_pending.empty()) {
                Pending& p = g_pending.front();
                size_t take = std::min(size_t(n - filled), p.size - p.pos);
                std::memcpy(buf + filled, p.data + p.pos, take);
                filled += int(take);
                p.pos += take;
                if (p.pos == p.size) {
                    finished.push_back(p.ctx);
                    g_pending.pop_front();
                }
            }
        }
        if (filled < n) std::memset(buf + filled, 0, size_t(n - filled));  // underrun: silence
        apply_gain(reinterpret_cast<int16_t*>(buf), n / 2);
        SDL_PutAudioStreamData(s, buf, n);
        for (void* ctx : finished) g_done(ctx);
        additional -= n;
    }
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) return false;
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        SDL_PauseAudioStreamDevice(g_stream);
    else
        SDL_ResumeAudioStreamDevice(g_stream);
}

// ---- Vulkan (MoltenVK through the Vulkan loader SDL finds) ------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    static void* gipa = [] {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            log_line(SDL_GetError());
            return static_cast<void*>(nullptr);
        }
        return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    }();
    return gipa;
}

const char* vulkan_surface_extension() { return VK_EXT_METAL_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateMetalSurfaceEXT"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    ci.pLayer = static_cast<const CAMetalLayer*>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

void crash_handler(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    const auto* m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    Dl_info d{};
    uint64_t pc = m->__ss.__pc;
    dladdr(reinterpret_cast<void*>(pc), &d);
    nx::log("CRASH: signal %d at %s+%llx, data address %llx", sig, d.dli_fname ? d.dli_fname : "?",
            (unsigned long long)(pc - uint64_t(d.dli_fbase)), (unsigned long long)uint64_t(info->si_addr));
    nx::CPU& c = nx::current_cpu();
    nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
    for (int k = 0; k < 32; k += 4)
        nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
                (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    nx::backtrace(c);
    nx::log_recent_files();
    signal(sig, SIG_DFL);
    raise(sig);
}

void game_thread() {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

int main() {
    nx::config_apply();  // config.ini -> NX_* defaults, before anything reads them (config.h)
    if (nx::report_mode()) return 0;
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine

    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread keeps Cocoa's
    // event loop.
    nx::plat::start_thread([](void*) { game_thread(); nx::plat::exit_process(0); }, nullptr, 64ull << 20);

    using namespace nx::plat;
    // One Cocoa event pump per turn (SDL_PollEvent pumps on every call: each pump walks AppKit's gesture recognizers,
    // ~0.25 ms on an M1), then the queue drained without pumping; a turn every 4 ms (250 Hz) when idle.
    for (;;) {
        SDL_Event e;
        SDL_PumpEvents();
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
            nx::ui::ui_event(e);
            switch (e.type) {
            case SDL_EVENT_QUIT:
                nx::log("platform: quit requested (window closed / Cmd-Q)");
                exit_process(0);
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
                g_minimized = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                g_minimized = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_pixel_w = uint32_t(e.window.data1), g_pixel_h = uint32_t(e.window.data2);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
                update_refresh_hz();
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
                // However it changed (F11, the green button, the settings screen), the setting follows the window,
                // so the settings screen shows it and the next start opens the same way.
                const char* now = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? "1" : "0";
                nx::log("platform: window %s full screen", *now == '1' ? "entered" : "left");
                if (nx::config_get("video.fullscreen", "0") != now) nx::config_set("video.fullscreen", now), nx::config_save();
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_F11 && g_window)
                    SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
                break;
            default:
                break;
            }
        }
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lk(g_main_lock);
            work.swap(g_main_work);
        }
        for (auto& fn : work) fn();
        sample_input();
        SDL_Delay(4);
    }
}
