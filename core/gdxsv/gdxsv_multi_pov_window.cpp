// The platform-independent half of the grid: what the four windows do, in
// terms of the small per-platform interface in gdxsv_multi_pov_window.h.
#include "gdxsv_multi_pov_window.h"

#include "log/LogManager.h"
#include "types.h"

// Host state, kept between frames so the grid is only republished when it
// actually moved.
static uint32_t g_generation = 0;
static GdxsvMultiPovRect g_last_group;
static bool g_last_maximized = false;

// Guest state: the generation already applied, so a guest that is not being
// moved does not fight the window manager every frame.
static uint32_t g_applied_generation = 0;
static bool g_borderless_done = false;

// The host has not laid the grid out yet. Its window is a whole window at this
// point, not a quadrant, so the first tick has to place it.
static bool g_host_placed = false;

// Does `inner` fit inside `outer`?
static bool FitsWithin(const GdxsvMultiPovRect& inner, const GdxsvMultiPovRect& outer) {
	if (outer.w <= 0 || outer.h <= 0) return true;  // nothing known to fit inside
	return outer.x <= inner.x && outer.y <= inner.y && inner.x + inner.w <= outer.x + outer.w &&
		   inner.y + inner.h <= outer.y + outer.h;
}

static void TickHost() {
	GdxsvMultiPovHostWindow hw;

	if (gdxsv_multi_pov_window_is_maximized()) {
		// Maximized means the grid takes the whole work area, tiled into four
		// equal quadrants. The host cannot be maximized and be one quadrant at
		// the same time, so it drops out of the maximized state into its own.
		const GdxsvMultiPovRect area = gdxsv_multi_pov_window_work_area();
		GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
		gdxsv_multi_pov_compute_grid(area, quadrants);
		gdxsv_multi_pov_window_unmaximize();
		gdxsv_multi_pov_window_set_frame(quadrants[0]);
		hw.group = area;
		hw.maximized = true;
	} else {
		// The host's window is the top-left quadrant, so the grid is twice its
		// size. Moving or resizing the host moves and resizes all four.
		const GdxsvMultiPovRect frame = gdxsv_multi_pov_window_get_frame();
		hw.group = {frame.x, frame.y, frame.w * 2, frame.h * 2};
		hw.maximized = false;

		if (!g_host_placed) {
			// First tick of the session. The host is still a full-size window,
			// so a grid of twice that usually runs off the bottom-right of the
			// display and three of the four screens would come up off-screen.
			// Lay the grid over the work area instead and take the top-left
			// quadrant; the user can move and resize it from there.
			const GdxsvMultiPovRect area = gdxsv_multi_pov_window_work_area();
			if (!FitsWithin(hw.group, area)) {
				hw.group = area;
				GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
				gdxsv_multi_pov_compute_grid(hw.group, quadrants);
				gdxsv_multi_pov_window_set_frame(quadrants[0]);
				NOTICE_LOG(COMMON, "multi-pov: grid laid out over the work area %dx%d", area.w, area.h);
			}
			g_host_placed = true;
		}
	}
	hw.rect = gdxsv_multi_pov_window_get_frame();

	if (hw.group != g_last_group || hw.maximized != g_last_maximized) {
		g_last_group = hw.group;
		g_last_maximized = hw.maximized;
		++g_generation;
	}
	hw.generation = g_generation;

	// Published every frame even when nothing moved: this is also the host's
	// heartbeat, and a host that stops ticking is a host the guests give up on.
	gdxsv_multi_pov_publish_host_window(hw);
}

static void TickGuest() {
	const int screen = gdxsv_multi_pov_screen_index();
	if (screen < 1 || kGdxsvMultiPovScreens <= screen) return;

	if (!g_borderless_done) {
		// Borderless: four decorated windows butted together look like four
		// windows. Done once, and only for guests - the host keeps its frame,
		// because that is what the user drags.
		gdxsv_multi_pov_window_set_borderless(true);
		g_borderless_done = true;
	}

	GdxsvMultiPovHostWindow hw;
	if (!gdxsv_multi_pov_read_host_window(hw)) return;
	if (hw.generation == g_applied_generation) return;
	g_applied_generation = hw.generation;

	GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
	gdxsv_multi_pov_compute_grid(hw.group, quadrants);
	gdxsv_multi_pov_window_set_frame(quadrants[screen]);
}

void gdxsv_multi_pov_window_tick() {
	const GdxsvMultiPovRole role = gdxsv_multi_pov_current_role();
	if (role == GdxsvMultiPovRole::None) return;
	if (!gdxsv_multi_pov_window_available()) return;

	if (role == GdxsvMultiPovRole::Host)
		TickHost();
	else
		TickGuest();
}

