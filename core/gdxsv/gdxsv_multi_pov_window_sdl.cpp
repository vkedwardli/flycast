// Linux backend for the 4-player replay grid, through SDL.
//
// SDL is the right layer here, not a shortcut: Flycast's Linux build runs on
// both X11 and Wayland, and SDL is what knows which one it got. Talking Xlib
// directly would work on one and not the other.
#include "gdxsv_multi_pov_window.h"

#include <SDL.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

static SDL_Window* Window() { return gdxsv_headless() ? nullptr : sdl_get_window(); }

bool gdxsv_multi_pov_window_available() { return Window() != nullptr; }

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	GdxsvMultiPovRect out;
	SDL_Window* w = Window();
	if (w == nullptr) return out;

	// SDL reports the content area, which is the convention the grid is
	// written in, so there is nothing to subtract here.
	SDL_GetWindowPosition(w, &out.x, &out.y);
	SDL_GetWindowSize(w, &out.w, &out.h);
	return out;
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	SDL_Window* w = Window();
	if (w == nullptr || rect.w <= 0 || rect.h <= 0) return;

	// Size first: a window manager that clamps the position to keep the window
	// on screen does it against the size it currently has.
	int cur_w = 0, cur_h = 0;
	SDL_GetWindowSize(w, &cur_w, &cur_h);
	if (cur_w != rect.w || cur_h != rect.h) SDL_SetWindowSize(w, rect.w, rect.h);

	int cur_x = 0, cur_y = 0;
	SDL_GetWindowPosition(w, &cur_x, &cur_y);
	if (cur_x != rect.x || cur_y != rect.y) SDL_SetWindowPosition(w, rect.x, rect.y);
}

bool gdxsv_multi_pov_window_is_maximized() {
	SDL_Window* w = Window();
	return w != nullptr && (SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED) != 0;
}

void gdxsv_multi_pov_window_unmaximize() {
	SDL_Window* w = Window();
	if (w != nullptr) SDL_RestoreWindow(w);
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	GdxsvMultiPovRect out;
	SDL_Window* w = Window();
	if (w == nullptr) return out;

	const int display = SDL_GetWindowDisplayIndex(w);
	SDL_Rect usable{};
	// Usable bounds, not display bounds: panels and docks are not ours to tile
	// over. SDL falls back to the full display when the desktop does not say.
	if (display < 0 || SDL_GetDisplayUsableBounds(display, &usable) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot read the display work area: %s", SDL_GetError());
		return out;
	}
	out.x = usable.x;
	out.y = usable.y;
	out.w = usable.w;
	out.h = usable.h;
	return out;
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	SDL_Window* w = Window();
	if (w != nullptr) SDL_SetWindowBordered(w, borderless ? SDL_FALSE : SDL_TRUE);
}

