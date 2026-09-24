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
static bool g_last_fullscreen = false;

// Guest state: the generation already applied, so a guest that is not being
// moved does not fight the window manager every frame.
static uint32_t g_applied_generation = 0;
static bool g_borderless_done = false;

// The host has not laid the grid out yet. Its window is a whole window at this
// point, not a quadrant, so the first tick has to place it.
static bool g_host_placed = false;

// Host: the grid is full screen (see gdxsv_multi_pov_toggle_fullscreen), and
// the quadrant to go back to when it leaves it.
static bool g_fullscreen = false;
static GdxsvMultiPovRect g_fullscreen_restore;

// Guest: whether this window is currently held on top, so it is only changed
// when the host's full-screen state does.
static bool g_guest_topmost = false;

// Does `inner` fit inside `outer`?
static bool FitsWithin(const GdxsvMultiPovRect& inner, const GdxsvMultiPovRect& outer) {
	if (outer.w <= 0 || outer.h <= 0) return true;  // nothing known to fit inside
	return outer.x <= inner.x && outer.y <= inner.y && inner.x + inner.w <= outer.x + outer.w &&
		   inner.y + inner.h <= outer.y + outer.h;
}

// The part of the work area the grid may cover. The grid is laid out in
// client rects and the host keeps its frame, so its title bar and left border
// live outside its quadrant: laid out over the whole work area, the host's
// caption lands above the top of the display and the user is left with a
// window they cannot move, drag or close - including after the replay ends,
// because nothing moves the window back.
static GdxsvMultiPovRect HostGridArea() {
	GdxsvMultiPovRect area = gdxsv_multi_pov_window_work_area();
	const GdxsvMultiPovInsets insets = gdxsv_multi_pov_window_frame_insets();
	// Only the top and the left: the host's own frame is what has to fit, and
	// the other two sides of it hang over the guests, which are on-screen.
	area.x += insets.left;
	area.w -= insets.left;
	area.y += insets.top;
	area.h -= insets.top;
	return area;
}

static void TickHost() {
	GdxsvMultiPovHostWindow hw;

	if (g_fullscreen) {
		// Full screen means the grid takes the whole display, tiled into four
		// equal quadrants, with no frames in the way. The host's quadrant is
		// re-applied whenever something moved it - the desktop restoring a
		// frame, say - so the grid stays put until the user leaves.
		const GdxsvMultiPovRect area = gdxsv_multi_pov_window_display_area();
		GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
		gdxsv_multi_pov_compute_grid(area, quadrants);
		if (gdxsv_multi_pov_window_get_frame() != quadrants[0]) gdxsv_multi_pov_window_set_frame(quadrants[0]);
		hw.group = area;
		hw.maximized = false;
		hw.fullscreen = true;
	} else if (gdxsv_multi_pov_window_is_maximized()) {
		// Maximized means the grid takes the whole work area, tiled into four
		// equal quadrants. The host cannot be maximized and be one quadrant at
		// the same time, so it drops out of the maximized state into its own.
		const GdxsvMultiPovRect area = HostGridArea();
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
			const GdxsvMultiPovRect area = HostGridArea();
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

	if (hw.group != g_last_group || hw.maximized != g_last_maximized || hw.fullscreen != g_last_fullscreen) {
		g_last_group = hw.group;
		g_last_maximized = hw.maximized;
		g_last_fullscreen = hw.fullscreen;
		++g_generation;
	}
	hw.generation = g_generation;

	// Published every frame even when nothing moved: it is a handful of stores,
	// and it keeps the header the live truth for a guest that is still booting
	// and has not read the layout yet.
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

	// On top with the host while the grid is full screen, so the task bar
	// does not sit over the bottom row; back to normal with it.
	if (hw.fullscreen != g_guest_topmost) {
		g_guest_topmost = hw.fullscreen;
		gdxsv_multi_pov_window_set_topmost(hw.fullscreen);
	}

	GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
	gdxsv_multi_pov_compute_grid(hw.group, quadrants);
	gdxsv_multi_pov_window_set_frame(quadrants[screen]);
}

// The host leaves full screen: frame and place back, on top no longer.
static void LeaveFullscreen() {
	g_fullscreen = false;
	gdxsv_multi_pov_window_set_topmost(false);
	gdxsv_multi_pov_window_set_borderless(false);
	gdxsv_multi_pov_window_set_frame(g_fullscreen_restore);
}

bool gdxsv_multi_pov_toggle_fullscreen() {
	const GdxsvMultiPovRole role = gdxsv_multi_pov_current_role();
	if (role == GdxsvMultiPovRole::None) return false;
	if (!gdxsv_multi_pov_window_available()) return false;
	// A guest is a view: the user drives the grid from the host's window.
	if (role == GdxsvMultiPovRole::Guest) return true;

	if (g_fullscreen) {
		LeaveFullscreen();
		NOTICE_LOG(COMMON, "multi-pov: grid left full screen");
	} else {
		// A maximized window cannot be a quadrant; and the quadrant to come
		// back to is the one it has as a plain window.
		if (gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_unmaximize();
		g_fullscreen_restore = gdxsv_multi_pov_window_get_frame();
		gdxsv_multi_pov_window_set_borderless(true);
		gdxsv_multi_pov_window_set_topmost(true);
		g_fullscreen = true;
		NOTICE_LOG(COMMON, "multi-pov: grid full screen");
	}
	return true;
}

void gdxsv_multi_pov_window_tick() {
	const GdxsvMultiPovRole role = gdxsv_multi_pov_current_role();
	if (!gdxsv_multi_pov_window_available()) return;

	if (role == GdxsvMultiPovRole::None) {
		// The session is over. A host that was full screen gets its window
		// back the way it was, rather than being left a borderless quadrant
		// stuck on top of the desktop; and the next session lays the grid out
		// afresh.
		if (g_fullscreen) LeaveFullscreen();
		g_host_placed = false;
		return;
	}

	if (role == GdxsvMultiPovRole::Host)
		TickHost();
	else
		TickGuest();
}

