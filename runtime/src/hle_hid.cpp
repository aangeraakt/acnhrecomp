// nn::hid at the API level: player 1's Npad state comes from the platform (XInput + keyboard on Windows, game
// controllers + touch on Android). Vibration is accepted and dropped.
#include <array>
#include <chrono>
#include <cctype>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "config.h"
#include "platform.h"

namespace nx {

namespace {

using namespace plat;

constexpr uint32_t kStyleFullKey = 1u << 0;
uint64_t g_sampling = 0;

// Scripted input for automated runs: NX_INPUT="3300:A,3900:A,4200:Down,4300:LUp,4400:T640x360" holds the button (A B X Y
// L R ZL ZR Plus Minus Up Down Left Right; or tilts the left stick: LUp, LDown, LLeft, LRight; or touches the screen at
// x, y) for 8 frames from the given frame. M<x>x<y> moves the mouse there (button up) on that frame.
struct Scripted {
    uint64_t frame;
    uint64_t buttons;
    int32_t lx = 0, ly = 0;  // left stick (LUp, LDown, LLeft, LRight): full tilt
    bool touch = false;      // T<x>x<y>: a touch at that point of the 1280x720 screen
    bool move = false;       // M<x>x<y>: the mouse moves to that point
    uint32_t tx = 0, ty = 0;
};

std::vector<Scripted> parse_script(const std::string& s, uint64_t base) {
    std::vector<Scripted> v;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();  // a file's last line
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            uint64_t bits = b == "A" ? kA : b == "B" ? kB : b == "X" ? kX : b == "Y" ? kY : b == "Plus" ? kPlus :
                            b == "Minus" ? kMinus : b == "Up" ? kUp : b == "Down" ? kDown : b == "Left" ? kLeft :
                            b == "Right" ? kRight : b == "L" ? kL : b == "R" ? kR : b == "ZL" ? kZL : b == "ZR" ? kZR : 0;
            Scripted sc{base + std::strtoull(item.c_str(), nullptr, 10), bits};
            if (b == "LUp") sc.ly = kStickMax;
            if (b == "LDown") sc.ly = -kStickMax;
            if (b == "LLeft") sc.lx = -kStickMax;
            if (b == "LRight") sc.lx = kStickMax;
            if (b.size() > 1 && b[0] == 'T' && std::sscanf(b.c_str() + 1, "%ux%u", &sc.tx, &sc.ty) == 2) sc.touch = true;
            if (b.size() > 1 && b[0] == 'M' && std::sscanf(b.c_str() + 1, "%ux%u", &sc.tx, &sc.ty) == 2) sc.move = true;
            v.push_back(sc);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

std::vector<Scripted> input_script() {
    const char* e = std::getenv("NX_INPUT");
    return e ? parse_script(e, 0) : std::vector<Scripted>{};
}

// The script (NX_INPUT and what input_live adds), shared by the pad, the touch screen and the mouse.
std::mutex g_script_lock;
std::vector<Scripted>& script() {
    static std::vector<Scripted> s = input_script();
    return s;
}

// Live input for interactive test runs: a file named input_live in the data folder, holding the NX_INPUT syntax
// with frames counted from when it is read (buttons, sticks, touches and mouse moves), is added to the script and
// deleted (checked every 15 frames). Together with the dump_frames file, a session can be driven step by step past
// the scripted intro (tools/play/live.py).
void poll_live(std::vector<Scripted>& script) {
    static uint64_t next_check = 0;
    if (g_frame < next_check) return;
    next_check = g_frame + 15;
    // NX_LIVE_FRAME=1: the frame reached, in the data folder's live_frame file (written whole, then renamed), so the
    // driver waits for its input to have played out in game frames, whatever the frame rate.
    static const bool frame_file = std::getenv("NX_LIVE_FRAME") != nullptr;
    if (frame_file) {
        static const std::string out = data_root() + "/live_frame", tmp = out + ".tmp";
        if (FILE* f = fopen_utf8(tmp, "wb")) {
            std::fprintf(f, "%llu\n", (unsigned long long)g_frame.load());
            std::fclose(f);
            std::rename(tmp.c_str(), out.c_str());
        }
    }
    static const std::string path = data_root() + "/input_live";
    FILE* f = fopen_utf8(path, "rb");
    if (!f) return;
    std::string s;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
    std::fclose(f);
    std::remove(path.c_str());
    std::erase_if(script, [](const Scripted& x) { return x.frame + 8 < g_frame; });
    std::vector<Scripted> add = parse_script(s, g_frame + 1);
    log("hid: live input, %zu entries from frame %llu", add.size(), (unsigned long long)g_frame);
    script.insert(script.end(), add.begin(), add.end());
}

// hid_hold_buttons: buttons the game side holds down for a while (a click on a list item ends with an A press).
std::atomic<uint64_t> g_held_buttons{0}, g_held_from{0}, g_held_until{0};

PadState read() {
    PadState p;
    std::lock_guard lk(g_script_lock);
    std::vector<Scripted>& script = nx::script();
    poll_live(script);
    for (const auto& s : script)
        if (g_frame >= s.frame && g_frame < s.frame + 8) p.buttons |= s.buttons;
    // NX_INPUT_TRACE=1: each change of the scripted buttons, with the frame and the sample number the game gets.
    static const bool trace = std::getenv("NX_INPUT_TRACE") != nullptr;
    static uint64_t last = 0;
    if (trace && p.buttons != last)
        log("hid: script buttons %llx at frame %llu, sample %llu", (unsigned long long)p.buttons,
            (unsigned long long)g_frame.load(), (unsigned long long)g_sampling + 1);
    last = p.buttons;
    if (g_frame >= g_held_from && g_frame < g_held_until) p.buttons |= g_held_buttons;
    read_pad(p);
    bool stick = false;  // after the pad: the script's stick wins while it holds it; LUp with LLeft is a diagonal
    for (const auto& s : script)
        if ((s.lx || s.ly) && g_frame >= s.frame && g_frame < s.frame + 8) {
            if (!stick) p.lx = p.ly = 0, stick = true;
            if (s.lx) p.lx = s.lx;
            if (s.ly) p.ly = s.ly;
        }
    // Stick directions as buttons, like the hid sysmodule reports them.
    constexpr int32_t t = kStickMax / 2;
    if (p.lx < -t) p.buttons |= kStickLLeft;
    if (p.lx > t) p.buttons |= kStickLRight;
    if (p.ly > t) p.buttons |= kStickLUp;
    if (p.ly < -t) p.buttons |= kStickLDown;
    if (p.rx < -t) p.buttons |= kStickRLeft;
    if (p.rx > t) p.buttons |= kStickRRight;
    if (p.ry > t) p.buttons |= kStickRUp;
    if (p.ry < -t) p.buttons |= kStickRDown;
    return p;
}

// NpadFullKeyState / NpadJoyDualState / NpadHandheldState:
// {s64 sampling; u64 buttons; {s32 x, y} left; {s32 x, y} right; u32 attributes; pad}
void GetNpadState(CPU& c) {
    uint64_t out = c.x[0];
    uint32_t id = ld<uint32_t>(c.x[1]);
    std::memset(reinterpret_cast<void*>(out), 0, 0x28);
    st<int64_t>(out, int64_t(++g_sampling));
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s", id, hle_current_name());
    }
    if (id != 0) return;  // only player 1 is connected
    PadState p = read();
    static bool pressed = false;
    if (p.buttons && !pressed) log("hid: first buttons %llx", (unsigned long long)p.buttons), pressed = true;
    st<uint64_t>(out + 8, p.buttons);
    st<int32_t>(out + 16, p.lx);
    st<int32_t>(out + 20, p.ly);
    st<int32_t>(out + 24, p.rx);
    st<int32_t>(out + 28, p.ry);
    st<uint32_t>(out + 32, 1);  // IsConnected
}

// GetNpadStates(State* out, int count, const u32& id) -> number written: the newest first. One sample: the current
// one for player 1's full-key style, a not-connected one otherwise.
// GetNpadStates(State* out, int count, const u32& id) -> number written, newest first. Like the real hid, a history:
// Wonder ignores the return value and reads all 16 entries it asks for, telling new samples by their sampling
// numbers, so every call adds a sample and returns up to 16 of the latest (older entries stay as they were).
void GetNpadStatesFullKey(CPU& c) {
    {
        static std::atomic<uint64_t> calls{0};
        uint64_t n = ++calls;
        if (n % 3000 == 0) log("hid: %llu full-key reads (lr %s)", (unsigned long long)n, describe(c.x[30]).c_str());
    }
    uint64_t out = c.x[0], id_ref = c.x[2];
    int n = int(c.x[1]);
    if (n < 1) {
        c.x[0] = 0;
        return;
    }
    static std::mutex m;
    static std::deque<std::array<uint8_t, 0x28>> history;
    std::lock_guard lk(m);
    c.x[0] = out;
    c.x[1] = id_ref;
    GetNpadState(c);  // the new sample, into out[0]
    if (ld<uint32_t>(id_ref) != 0) {
        c.x[0] = 1;
        return;
    }
    std::array<uint8_t, 0x28> now;
    std::memcpy(now.data(), reinterpret_cast<const void*>(out), now.size());
    history.push_front(now);
    if (history.size() > 16) history.pop_back();
    int count = std::min<int>(n, int(history.size()));
    for (int k = 1; k < count; k++) std::memcpy(reinterpret_cast<void*>(out + 0x28ull * k), history[k].data(), 0x28);
    c.x[0] = uint64_t(count);
}

void GetNpadStatesOther(CPU& c) {
    {
        static std::set<std::pair<uint32_t, std::string>> seen;
        static std::mutex m;
        std::lock_guard lk(m);
        uint32_t id = ld<uint32_t>(c.x[2]);
        if (seen.insert({id, hle_current_name()}).second) log("hid: reads id %#x via %s (not connected)", id, hle_current_name());
    }
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// MouseState {s64 sampling; s32 x, y, dx, dy, wheel dx, wheel dy; u32 buttons; u32 attributes}: no mouse.
void GetMouseStates(CPU& c) {
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x28);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    c.x[0] = 1;
}

// The PC keyboard as a USB keyboard (plat::set_keyboard). While the game types with it (its text entry turns keys
// into characters with nn::hid::GetKeyCode), the keys stop driving the pad (platform_sdl.cpp); a key that types and
// is not turned into a character within kTypingProbeMs means the text entry has closed.
std::mutex g_kbd_lock;
uint64_t g_kbd_mods = 0;
uint8_t g_kbd_keys[32] = {};
std::atomic<bool> g_typing{false};
std::atomic<bool> g_typing_known{false};  // the game reports its text entry (plat::keyboard_text_entry)
std::atomic<int64_t> g_keycode_ms{0};  // the last GetKeyCode
int64_t g_probe_ms = 0;                // a typing key pressed while typing, not yet turned into a character
constexpr int64_t kTypingProbeMs = 250;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// KeyboardState {s64 sampling; u32 modifiers; u32 attributes (bit 0: connected); u8 keys[32]}.
void GetDebugPadState(CPU& c) {
    for (uint64_t k = 0; k < 0x20; k += 8) st<uint64_t>(c.x[0] + k, 0);
}

void GetKeyboardState(CPU& c) {
    std::lock_guard lk(g_kbd_lock);
    st<int64_t>(c.x[0], int64_t(g_sampling));
    st<uint32_t>(c.x[0] + 8, uint32_t(g_kbd_mods));
    st<uint32_t>(c.x[0] + 12, 1);
    std::memcpy(reinterpret_cast<void*>(c.x[0] + 16), g_kbd_keys, 32);
}

const char* const kGetKeyCodeName =
    "_ZN2nn3hid10GetKeyCodeEPtiNS_4util10BitFlagSetILi32ENS0_16KeyboardModifierEEENS2_7BitPackIjNS0_14KeyboardLayoutEEE";

// The SDK's own GetKeyCode, called through: only the time is noted.
void GetKeyCode(CPU& c) {
    g_keycode_ms = now_ms();
    if (!g_typing_known && !g_typing.exchange(true))
        log("hid: the game types with the keyboard: its keys no longer drive the pad");
    static const uint64_t real = find_symbol(kGetKeyCodeName);
    call(c, real);
}

void zero(CPU& c) { c.x[0] = 0; }
void one(CPU& c) { c.x[0] = 1; }

void GetNpadStyleSet(CPU& c) {
    static int logged = 0;
    if (logged++ < 4) log("hid: GetNpadStyleSet id %#x", ld<uint32_t>(c.x[0]));
    c.x[0] = ld<uint32_t>(c.x[0]) == 0 ? kStyleFullKey : 0;
}

void BindNpadStyleSetUpdateEvent(CPU& c) {  // (const u32& id, SystemEventType*, EventClearMode)
    uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t ev = c.x[1], mode = c.x[2];
    uint32_t id = ld<uint32_t>(c.x[0]);
    c.x[0] = ev;
    c.x[1] = mode;
    c.x[2] = 0;
    call(c, create);
    // Player 1's controller is there from the start: the style set "changed" (connected) once, and games wait for
    // this event before they look at the npad.
    static uint64_t signal = find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (signal) {
        c.x[0] = ev;
        call(c, signal);
    }
    log("hid: style set event bound for id %#x, signalled", id);
}

void GetVibrationDeviceHandles(CPU& c) {
    uint32_t id = c.x[2] ? ld<uint32_t>(c.x[2]) : 0;
    if (id == 0 && c.x[0] && int32_t(c.x[1]) > 0) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = 1;
        return;
    }
    c.x[0] = 0;
}

void GetVibrationDeviceInfo(CPU& c) {
    if (c.x[0]) {
        st<uint32_t>(c.x[0], 1);      // VibrationDeviceType_LinearResonantActuator
        st<uint32_t>(c.x[0] + 4, 1);  // VibrationDevicePosition_Left
    }
    c.x[0] = 0;
}

void SendVibrationValue(CPU& c) {
    if (config_get_bool("input.rumble", true) && c.x[1]) {
        float amp_low = ld<float>(c.x[1]);
        float amp_high = ld<float>(c.x[1] + 8);
        if (amp_low > 0.001f || amp_high > 0.001f) {
            plat::rumble(amp_low, amp_high, 60);
        } else {
            plat::rumble(0.0f, 0.0f, 0);
        }
    }
    c.x[0] = 0;
}
void ShowControllerSupport(CPU& c) {                     // (ControllerSupportResultInfo*, const Arg&)
    st<int8_t>(c.x[0], 1);        // player count
    st<uint32_t>(c.x[0] + 4, 0);  // selected npad id: No1
    c.x[0] = 0;
}
void noop(CPU& c) { c.x[0] = 0; }

// ---- first needed by ACNH (SDK 20) ----

// TouchScreenState<N> {s64 sampling; s32 count; u32 reserved; TouchState touches[N]}, TouchState (0x28 bytes):
// {u64 delta_time; u32 attributes (1: start, 2: end); u32 finger_id; u32 x, y (1280x720 screen); u32 diameter_x,
// diameter_y; u32 rotation_angle; u32 reserved}. One finger: the mouse's left button over the picture, or the
// script's touches (NX_INPUT "<frame>:T<x>x<y>", held 8 frames).
struct Touch {
    bool down = false;
    uint32_t x = 0, y = 0;
};
std::mutex g_touch_lock;
float g_box[4] = {0, 0, 1, 1};
Touch g_mouse;
bool g_mouse_tap = false;  // pressed since the last sample: a click shorter than a frame still touches once
// Where the mouse is over the picture with its button up, and a count of its moves (mouse_take_hover).
struct Hover {
    uint32_t x = 0, y = 0;
    uint64_t moves = 0;
};
Hover g_hover;

Touch current_touch() {
    {
        std::lock_guard lk(g_script_lock);
        for (const auto& s : script())
            if (s.touch && g_frame >= s.frame && g_frame < s.frame + 8) return {true, s.tx, s.ty};
    }
    std::lock_guard lk(g_touch_lock);
    Touch t = g_mouse;
    if (g_mouse_tap) t.down = true, g_mouse_tap = false;
    return t;
}

// One sample per presented frame, shared by every reader: the game reads the screen through two APIs each frame
// (GetTouchScreenState<1> for its controller, GetTouchScreenStates<16> for its UI), and each must see the same
// edges. As on the console, a touch's last sample is the finger lifted: still listed, with the End attribute, at the
// last position; the samples after it list none. ACNH's UI (guiTouchManager, main+31c3b50) presses a button when
// the touch that grabbed it ends with that sample; a touch that just vanishes cancels instead.
struct TouchSample {
    uint64_t sampling = 0;
    uint32_t attributes = 0;  // 1: Start, 2: End
    bool listed = false;
    uint32_t x = 0, y = 0;
};

struct Tap {
    bool pending = false;
    uint32_t x = 0, y = 0;
    uint64_t frame = 0;
};
Tap g_tap;  // the last touch that ended, until the game side takes it (touch_take_tap)
std::mutex g_tap_lock;

TouchSample touch_sample() {
    static std::mutex lock;
    static uint64_t frame = ~0ull;
    static TouchSample cur;
    std::lock_guard lk(lock);
    if (g_frame == frame) return cur;
    frame = g_frame;
    Touch t = current_touch();
    bool was_down = cur.listed && !(cur.attributes & 2);
    cur.sampling++;
    if (t.down)
        cur.listed = true, cur.attributes = was_down ? 0 : 1, cur.x = t.x, cur.y = t.y;
    else if (was_down) {
        cur.attributes = 2;  // lifted: listed once more, where it was
        std::lock_guard tk(g_tap_lock);
        g_tap = {true, cur.x, cur.y, frame};
    } else
        cur.listed = false, cur.attributes = 0;
    if (std::getenv("NX_TRACE_TOUCH") && cur.listed)
        log("hid: touch at frame %llu: %u x %u%s", (unsigned long long)frame, cur.x, cur.y,
            cur.attributes == 1 ? " (start)" : cur.attributes == 2 ? " (end)" : "");
    return cur;
}

// y_bias: added to the reported y (kept on the screen).
void fill_touch(uint64_t out, uint32_t y_bias = 0) {
    TouchSample t = touch_sample();
    std::memset(reinterpret_cast<void*>(out), 0, 0x10 + 0x28);
    st<int64_t>(out, int64_t(t.sampling));
    if (t.listed) {
        st<int32_t>(out + 8, 1);
        uint64_t s = out + 0x10;
        st<uint32_t>(s + 8, t.attributes);
        st<uint32_t>(s + 16, t.x);
        st<uint32_t>(s + 20, std::min<uint32_t>(t.y + y_bias, 719));
        st<uint32_t>(s + 24, 15);
        st<uint32_t>(s + 28, 15);
    }
}

void GetTouchScreenState1(CPU& c) {
    if (std::getenv("NX_TRACE_TOUCH")) {
        static int n = 0;
        if (n++ < 3) log("hid: GetTouchScreenState<1> from %s", describe(c.x[30]).c_str());
    }
    fill_touch(c.x[0]);
}

void GetTouchScreenStates16(CPU& c) {  // int (TouchScreenState<16>*, int count): the newest state only
    if (std::getenv("NX_TRACE_TOUCH")) {
        static int n = 0;
        if (n++ < 3) log("hid: GetTouchScreenStates<16>(count %d) from %s", int(c.x[1]), describe(c.x[30]).c_str());
    }
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x10 + 0x28 * 16);
    fill_touch(c.x[0], game::kInfo.touch_y_bias);
    c.x[0] = 1;
}

// BitFlagSet<32, NpadStyleTag> GetSupportedNpadStyleSet(): full key, handheld, dual and single Joy-Cons.
void GetSupportedNpadStyleSet(CPU& c) { c.x[0] = 0x1F; }

// Result GetNpadControllerColor(NpadControllerColor* left/main, NpadControllerColor* right/sub, const u32& id):
// {u32 main, u32 sub} as RGBA8. A Pro Controller: dark grey body, black buttons.
void GetNpadControllerColor(CPU& c) {
    for (uint64_t p : {c.x[0], c.x[1]})
        if (p) st<uint32_t>(p, 0xFF2D2D2D), st<uint32_t>(p + 4, 0xFF0F0F0F);
    c.x[0] = 0;
}

// Bit pattern of the player LEDs: player 1 is the first LED.
void GetPlayerLedPattern(CPU& c) { c.x[0] = 1; }

}  // namespace

bool mouse_take_hover(uint32_t& x, uint32_t& y) {
    static uint64_t scripted_until = 0, taken = 0;
    std::vector<Scripted> moves;  // copied first: the pad holds the script's lock while it reads the platform
    {
        std::lock_guard lk(g_script_lock);
        for (const auto& s : script())
            if (s.move && s.frame <= g_frame && s.frame >= scripted_until) moves.push_back(s);
    }
    std::lock_guard lk(g_touch_lock);
    for (const auto& s : moves)  // a scripted move counts once, on its frame
        if (s.frame >= scripted_until) {
            g_hover = {s.tx, s.ty, g_hover.moves + 1};
            scripted_until = s.frame + 1;
        }
    if (g_hover.moves == taken) return false;
    taken = g_hover.moves;
    x = g_hover.x, y = g_hover.y;
    return true;
}

bool touch_take_tap(uint32_t& x, uint32_t& y, uint64_t& frame) {
    std::lock_guard lk(g_tap_lock);
    if (!g_tap.pending) return false;
    g_tap.pending = false;
    x = g_tap.x, y = g_tap.y, frame = g_tap.frame;
    return true;
}

void hid_hold_buttons(uint64_t buttons, uint32_t frames) {
    // After a hold still running, two frames up first: each hold is a press of its own.
    uint64_t from = g_frame + 1, until = g_held_until;
    if (until + 2 > from) from = until + 2;
    g_held_until = 0;
    g_held_buttons = buttons;
    g_held_from = from;
    g_held_until = from + frames;
}

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", noop);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", noop);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
NX_HLE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", GetVibrationDeviceInfo);
NX_HLE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", ShowControllerSupport);
// SDK 16 (Super Mario Bros. Wonder)
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", GetNpadStatesFullKey);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid15InitializeMouseEv", noop);
NX_HLE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", GetMouseStates);
NX_HLE("_ZN2nn3hid18InitializeKeyboardEv", noop);
// The debug pad (DebugPadState {s64 sampling; u32 attributes; u32 buttons; two sticks}: 0x20 bytes), read by ACNH
// 1.0.0's engine: never connected here. Its real initialization went to the hid service, which is a stub.
NX_HLE("_ZN2nn3hid18InitializeDebugPadEv", noop);
NX_HLE("_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE", GetDebugPadState);
NX_HLE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", GetKeyboardState);
NX_HLE(kGetKeyCodeName, GetKeyCode);
NX_HLE("_ZN2nn3hid14DisconnectNpadERKj", noop);
NX_HLE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", noop);
NX_HLE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", noop);
NX_HLE("_ZN2nn3hid21StartLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20StopLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", zero);  // dual
NX_HLE("_ZN2nn3hid20IsVibrationPermittedEv", one);
NX_HLE("_ZN2nn3hid21InitializeTouchScreenEv", noop);
NX_HLE("_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE", GetTouchScreenState1);
NX_HLE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", GetTouchScreenStates16);
NX_HLE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", GetSupportedNpadStyleSet);
NX_HLE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", GetNpadControllerColor);
NX_HLE("_ZN2nn3hid19GetPlayerLedPatternERKj", GetPlayerLedPattern);
NX_HLE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", noop);
NX_HLE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", noop);
NX_HLE("_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", noop);
NX_HLE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", SendVibrationValue);
NX_HLE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", zero);

}  // namespace nx

namespace nx::plat {

void set_keyboard(uint64_t modifiers, const uint8_t keys[32]) {
    std::lock_guard lk(g_kbd_lock);
    int64_t now = now_ms();
    for (int u = 0; u < 256; u++) {
        bool was = g_kbd_keys[u / 8] >> (u % 8) & 1, is = keys[u / 8] >> (u % 8) & 1;
        if (is && !was && key_types(u) && g_typing && !g_probe_ms) g_probe_ms = now;
    }
    if (g_typing_known) g_probe_ms = 0;
    if (g_probe_ms && g_keycode_ms >= g_probe_ms) g_probe_ms = 0;  // typed: still typing
    if (g_probe_ms && now - g_probe_ms > kTypingProbeMs) {
        g_probe_ms = 0;
        if (g_typing.exchange(false)) log("hid: the game no longer types with the keyboard: its keys drive the pad");
    }
    g_kbd_mods = modifiers;
    std::memcpy(g_kbd_keys, keys, 32);
}

namespace {
std::atomic<uint64_t> g_entry_frame{0};  // the last frame the game reported its text entry

void set_text_entry(bool open) {
    if (g_typing.exchange(open) != open)
        log("hid: the game's text entry %s: the keys %s", open ? "opened" : "closed", open ? "type" : "drive the pad");
}
}  // namespace

// A game stops reporting once its text entry has gone (its key handling no longer runs): open only while reported.
bool keyboard_typing() {
    if (g_typing_known && g_typing && g_frame > g_entry_frame + 3) set_text_entry(false);
    return g_typing;
}

void keyboard_text_entry(bool open) {
    g_typing_known = true;
    g_entry_frame = g_frame.load();
    set_text_entry(open);
}

bool keyboard_state_known() { return g_typing_known; }

void set_mouse_touch(bool down, float u, float v) {
    std::lock_guard lk(g_touch_lock);
    float x = (u - g_box[0]) / g_box[2], y = (v - g_box[1]) / g_box[3];
    bool inside = x >= 0 && x < 1 && y >= 0 && y < 1;
    uint32_t px = uint32_t(x * 1280), py = uint32_t(y * 720);
    if (down && inside) {
        if (!g_mouse.down) g_mouse_tap = true;
        g_mouse = {true, px, py};
    } else {
        g_mouse.down = false;
        // A move of a couple of pixels or more: a resting mouse, or one outside the picture, hovers nothing.
        int dx = int(px) - int(g_hover.x), dy = int(py) - int(g_hover.y);
        if (inside && dx * dx + dy * dy >= 4) g_hover = {px, py, g_hover.moves + 1};
    }
}

void set_picture_box(float x, float y, float w, float h) {
    if (w <= 0 || h <= 0) return;
    std::lock_guard lk(g_touch_lock);
    g_box[0] = x, g_box[1] = y, g_box[2] = w, g_box[3] = h;
}

}  // namespace nx::plat
