// ACNH 3.0.3 (main build id FF1D1C05670DB6021C85B624A710B963): 3.0.2's main with 16 bytes more code before
// main+31c3b50, so the hooks from there on sit 0x10 further and the data (vtables, the budget table, the page turner
// class) stays where 3.0.2 has it. Its hook list is games/acnh/hooks-3.0.3.txt (games/acnh/versions.txt); an offset
// here does nothing in a build whose list does not have it. The 60 FPS and wide screen hooks are next to 3.0.2's
// (native60.cpp, runtime/src/nvn/nvn_ultrawide.cpp); Pane::Calculate and the Phive guard are at 3.0.2's addresses.
#include <nx/runtime.h>

namespace nx {

void acnh_gui_touch_update(uint64_t manager);  // games/acnh/pointer.cpp
void acnh_focus_input(CPU& c, uint64_t set_focus);
void acnh_button_input(CPU& c, uint64_t page_turner);

namespace {

constexpr uint64_t kPageTurner303 = 0x49fc8f0;  // the message window's page turner class: 3.0.2's

// guiTouchManager::update (main+31c3b60) right before its hit tests (x24 = the manager, as in 3.0.2).
void gui_touch(CPU& c) { acnh_gui_touch_update(c.x[24]); }
// The focus manager input (main+3c48c70) and its focus move (main+3c48b78; 3.0.2's main+3c48b68).
void focus_input(CPU& c) { acnh_focus_input(c, 0x3c48b78); }
// A button handler's input (main+3c4b20c): a click while a message waits.
void button_input(CPU& c) { acnh_button_input(c, kPageTurner303); }

}  // namespace

NX_HOOK(0x31c3fa8, gui_touch);     // guiTouchManager::update, before its hit tests
NX_HOOK(0x3c48c70, focus_input);   // focus manager input: a click on a list item
NX_HOOK(0x3c4b20c, button_input);  // button handler input: a click while a message waits

}  // namespace nx
