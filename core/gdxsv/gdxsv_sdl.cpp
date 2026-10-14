#if defined(USE_SDL)
#include "gdxsv_sdl.h"

#include <SDL.h>

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"

void gdxsv_sdl_input_init() {
#ifdef _WIN32
	if (!config::UseXInput) {
		// Disable XInput for some old joysticks
		NOTICE_LOG(INPUT, "Disabling XInput, using DirectInput");
		SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
	}
	if (!config::UseDirectInput) {
		NOTICE_LOG(INPUT, "Disabling DirectInput");
		SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
	}
	// Use separate thread for joystick input handling
	SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
#endif
}

u32 gdxsv_sdl_window_flags() {
	// A capture rig tiles instances edge to edge, so it wants no border.
	// Set at creation, not after: removing the decoration later moves the frame
	// and the grid comes out overlapping.
	return config::loadBool("gdxsv", "borderless", false) ? SDL_WINDOW_BORDERLESS : 0;
}

void gdxsv_sdl_window_create() {
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	if (gdxsv_headless()) {
		// No window: give the UI a nominal size so layout math stays finite.
		// Configurable so tests can exercise aspect-ratio-dependent code
		// (e.g. the widescreen HUD layout) at a chosen viewport.
		settings.display.width = config::loadInt("gdxsv", "headless_width", 640);
		settings.display.height = config::loadInt("gdxsv", "headless_height", 480);
	}
}
#endif
