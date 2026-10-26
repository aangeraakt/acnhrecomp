// ACNH 2.0.7 (main build id 0948E48778171EE6D5D7A65813ED5FCC): the fixes of the other builds, at this build's own
// addresses. Its hook list is games/acnh/hooks-2.0.7.txt (games/acnh/versions.txt); an offset here does nothing in a
// build whose list does not have it. docs/VERSIONS.md.
#include <nx/runtime.h>

#include "platform.h"

namespace nx {

void acnh_gui_touch_update(uint64_t manager);  // games/acnh/pointer.cpp
void acnh_focus_input(CPU& c, uint64_t set_focus);
void acnh_button_input(CPU& c, uint64_t page_turner);

namespace {

constexpr uint64_t kPageTurner207 = 0x43667b0;  // the message window's page turner class (3.0.2: main+49fc8f0)

// The engine's clear-on-bind (main+2748650; 3.0.2's main+37fb390, games/acnh/game.cpp) clears the colour target and
// the depth of every pass it binds. The depth clear is a tail call here, so its lr is whichever of ~45 callers bound
// the pass and NX_CLEAR_ONCE_AT cannot name it: each of the two calls is marked by a hook instead. Without this the
// picture stayed black (the UI captures in the HDR target wiped at each re-bind, STATUS #19, #27).
void bind_clear(CPU&) { nvn_mark_bind_clear(); }

// guiTouchManager::update (main+200e9e0; 3.0.2's main+31c3b50, games/acnh/pointer.cpp) right before it hit-tests
// the touches (main+200f050, x19 = the manager): the confirmation windows' buttons are made reachable to a click.
void gui_touch(CPU& c) { acnh_gui_touch_update(c.x[19]); }

// The layout framework's focus manager input (main+2b106b0; 3.0.2's main+3c48c60: x0 the manager, x1 the input
// device, w3 bit 0 input blocked) and its focus move (main+2b10360; 3.0.2's main+3c48b68): a click on a list item
// moves the cursor there and presses A (pointer.cpp). Same layout as 3.0.2's (targets +0x14a0/+0x14a8/+0x14b0).
void focus_input(CPU& c) { acnh_focus_input(c, 0x2b10360); }
// A button handler's input (main+2b13430; 3.0.2's main+3c4b1fc, same fields): a click while a message waits.
void button_input(CPU& c) { acnh_button_input(c, kPageTurner207); }

// The software keyboard's manager (main+204ff60, x0; reached each frame through main+205fe90) drops the keys while
// its state (+0x20) is one of its idle ones (+0x230, +0x38, +0xe0) or +0x163e9 is set: otherwise its text entry is
// open and the PC keyboard types (runtime/src/hle_hid.cpp); closed, the keys drive the pad again.
void text_entry(CPU& c) {
    uint64_t m = c.x[0], st = ld<uint64_t>(m + 0x20);
    plat::keyboard_text_entry(st && st != m + 0x230 && st != m + 0x38 && st != m + 0xe0 && !ld<uint8_t>(m + 0x163e9));
}

}  // namespace

NX_HOOK(0x27487d0, bind_clear);  // blr to nvnCommandBufferClearColor
NX_HOOK(0x2748824, bind_clear);  // br to nvnCommandBufferClearDepthStencil
NX_HOOK(0x200eac4, gui_touch);    // guiTouchManager::update, before its hit tests
NX_HOOK(0x204ff60, text_entry);   // the software keyboard's key handling: is its text entry open
NX_HOOK(0x2b106b0, focus_input);  // focus manager input: a click on a list item
NX_HOOK(0x2b13430, button_input); // button handler input: a click while a message waits

}  // namespace nx
