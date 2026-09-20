// The platform-independent half of the grid: what the four windows do, in
// terms of the small per-platform interface in gdxsv_multi_pov_window.h.
#include "gdxsv_multi_pov_window.h"

#include "log/LogManager.h"
#include "types.h"

namespace gdxsv_multi_pov {
namespace {

// Host state, kept between frames so the grid is only republished when it
// actually moved.
uint32_t g_generation = 0;
WindowRect g_last_group;
bool g_last_maximized = false;

// Guest state: the generation already applied, so a guest that is not being
// moved does not fight the window manager every frame.
uint32_t g_applied_generation = 0;
bool g_borderless_done = false;

void TickHost() {
	HostWindow hw;

	if (window::IsMaximized()) {
		// Maximized means the grid takes the whole work area, tiled into four
		// equal quadrants. The host cannot be maximized and be one quadrant at
		// the same time, so it drops out of the maximized state into its own.
		const WindowRect area = window::WorkArea();
		WindowRect quadrants[kScreens];
		ComputeGrid(area, quadrants);
		window::Unmaximize();
		window::SetFrame(quadrants[0]);
		hw.group = area;
		hw.maximized = true;
	} else {
		// The host's window is the top-left quadrant, so the grid is twice its
		// size. Moving or resizing the host moves and resizes all four.
		const WindowRect frame = window::GetFrame();
		hw.group = {frame.x, frame.y, frame.w * 2, frame.h * 2};
		hw.maximized = false;
	}
	hw.rect = window::GetFrame();

	if (hw.group != g_last_group || hw.maximized != g_last_maximized) {
		g_last_group = hw.group;
		g_last_maximized = hw.maximized;
		++g_generation;
	}
	hw.generation = g_generation;

	// Published every frame even when nothing moved: this is also the host's
	// heartbeat, and a host that stops ticking is a host the guests give up on.
	PublishHostWindow(hw);
}

void TickGuest() {
	const int screen = ScreenIndex();
	if (screen < 1 || kScreens <= screen) return;

	if (!g_borderless_done) {
		// Borderless: four decorated windows butted together look like four
		// windows. Done once, and only for guests - the host keeps its frame,
		// because that is what the user drags.
		window::SetBorderless(true);
		g_borderless_done = true;
	}

	HostWindow hw;
	if (!ReadHostWindow(hw)) return;
	if (hw.generation == g_applied_generation) return;
	g_applied_generation = hw.generation;

	WindowRect quadrants[kScreens];
	ComputeGrid(hw.group, quadrants);
	window::SetFrame(quadrants[screen]);
}

}  // namespace

void WindowTick() {
	const Role role = CurrentRole();
	if (role == Role::None) return;
	if (!window::Available()) return;

	if (role == Role::Host)
		TickHost();
	else
		TickGuest();
}

}  // namespace gdxsv_multi_pov
