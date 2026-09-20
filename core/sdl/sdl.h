#pragma once
#include <SDL.h>
#include "types.h"

void input_sdl_init();
void input_sdl_handle();
void input_sdl_quit();
// The window this process is showing, or nullptr before it is created or in
// a headless run. Lets code outside core/sdl (e.g. the 4-player replay grid,
// which has to move the window) reach it without another global.
SDL_Window *sdl_get_window();
void sdl_window_create();
void sdl_window_destroy();
bool sdl_recreate_window(u32 flags);
void sdl_fix_steamdeck_dpi(SDL_Window *window);
