#pragma once
#include "types.h"

// SDL setup only gdxsv needs, called from core/sdl/sdl.cpp.

// Before the joystick subsystem starts: SDL hints from the gdxsv input options.
void gdxsv_sdl_input_init();
// Extra SDL_WindowFlags for the main window.
u32 gdxsv_sdl_window_flags();
// Video subsystem started, render API not yet.
void gdxsv_sdl_window_create();
