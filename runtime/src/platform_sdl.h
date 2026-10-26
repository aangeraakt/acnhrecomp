// The SDL3 half of the PC hosts (platform_sdl.cpp), used by platform_win.cpp: the window and
// its Vulkan surface, input, audio, the settings screen's hooks and the main thread's event loop.
#pragma once

namespace nx::plat {

bool sdl_start();                   // on the main thread, before the game's thread starts: SDL_Init, the main thread
[[noreturn]] void sdl_main_loop();  // the main thread's event loop, for the rest of the process
void* sdl_native_window();          // the window's HWND once it exists, null before; from any thread

}  // namespace nx::plat
