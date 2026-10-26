// ACNH 1.0.0, the base game (main build id 7FC1BAFF976AECA414520CB89F4616CB): the fixes of the other builds, at this
// build's own addresses (found from 2.0.7's by their code, branch and data offsets masked). Its hook list is
// games/acnh/hooks-1.0.0.txt (games/acnh/versions.txt); an offset here does nothing in a build whose list does not
// have it. docs/VERSIONS.md.
//
// This build has no PC keyboard text entry: its software keyboard does not read the USB keyboard (no
// nn::hid::GetKeyCode; GetKeyboardState is only read by the engine's keyboard/mouse device, main+19b8e90). Names
// are typed with clicks on the on-screen keys.
#include <nx/runtime.h>

namespace nx {

void acnh_gui_touch_update(uint64_t manager);  // games/acnh/pointer.cpp
void acnh_focus_input(CPU& c, uint64_t set_focus);
void acnh_button_input(CPU& c, uint64_t page_turner);

namespace {

constexpr uint64_t kPageTurner100 = 0x3122e50;  // the message window's page turner class (2.0.7: main+43667b0)

// The engine's clear-on-bind (main+19d0c00; 2.0.7's main+2748650, v2_0_7.cpp): each of its two clear calls marked.
void bind_clear(CPU&) { nvn_mark_bind_clear(); }

// guiTouchManager::update (main+14b8e50; 2.0.7's main+200e9e0), right before its hit tests (x19 = the manager).
void gui_touch(CPU& c) { acnh_gui_touch_update(c.x[19]); }

// The focus manager input (main+1d95cf0; 2.0.7's main+2b106b0) and its focus move (main+1d959e0; 2.0.7's
// main+2b10360), and a button handler's input (main+1d98a70; 2.0.7's main+2b13430): the same fields as 2.0.7.
void focus_input(CPU& c) { acnh_focus_input(c, 0x1d959e0); }
void button_input(CPU& c) { acnh_button_input(c, kPageTurner100); }

}  // namespace

NX_HOOK(0x19d0d80, bind_clear);   // blr to nvnCommandBufferClearColor
NX_HOOK(0x19d0dd4, bind_clear);   // br to nvnCommandBufferClearDepthStencil
NX_HOOK(0x14b8f34, gui_touch);    // guiTouchManager::update, before its hit tests
NX_HOOK(0x1d95cf0, focus_input);  // focus manager input: a click on a list item
NX_HOOK(0x1d98a70, button_input); // button handler input: a click while a message waits

}  // namespace nx
