// macOS backend for the 4-player replay grid.
//
// Cocoa rather than SDL because of the coordinate system: AppKit measures from
// the bottom-left of the primary screen and talks in frame rects (title bar
// included), while the grid is written in top-left desktop coordinates over
// content rects. Doing that conversion here, once, against the real NSWindow
// is what keeps the quadrants exact - and visibleFrame is the only honest
// answer for "the area a maximized window covers" with the menu bar and the
// Dock in the way.
#include "gdxsv_multi_pov_window.h"

#import <AppKit/AppKit.h>

#include <cmath>

#include <SDL.h>
#include <SDL_syswm.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

static NSWindow* CocoaWindow() {
	if (gdxsv_headless()) return nil;
	SDL_Window* w = sdl_get_window();
	if (w == nullptr) return nil;

	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(w, &info)) return nil;
	if (info.subsystem != SDL_SYSWM_COCOA) return nil;
	return info.info.cocoa.window;
}

// The primary screen is the one AppKit measures everything else from: its
// frame has origin (0,0) and its height is the flip axis.
static CGFloat PrimaryHeight() {
	NSArray<NSScreen*>* screens = [NSScreen screens];
	if (screens.count == 0) return 0;
	return NSMaxY(screens[0].frame);
}

// AppKit rect (bottom-left origin) -> desktop rect (top-left origin).
static GdxsvMultiPovRect FromCocoa(NSRect r) {
	GdxsvMultiPovRect out;
	out.x = static_cast<int32_t>(std::lround(NSMinX(r)));
	out.y = static_cast<int32_t>(std::lround(PrimaryHeight() - NSMaxY(r)));
	out.w = static_cast<int32_t>(std::lround(NSWidth(r)));
	out.h = static_cast<int32_t>(std::lround(NSHeight(r)));
	return out;
}

static NSRect ToCocoa(const GdxsvMultiPovRect& r) {
	return NSMakeRect(r.x, PrimaryHeight() - (r.y + r.h), r.w, r.h);
}

bool gdxsv_multi_pov_window_available() { return CocoaWindow() != nil; }

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};
	return FromCocoa([win contentRectForFrameRect:win.frame]);
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	NSWindow* win = CocoaWindow();
	if (win == nil || rect.w <= 0 || rect.h <= 0) return;

	// setFrame: moves without ordering the window front, so a follower does
	// not take focus from the screen the user is driving.
	const NSRect content = ToCocoa(rect);
	[win setFrame:[win frameRectForContentRect:content] display:YES];
}

bool gdxsv_multi_pov_window_is_maximized() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return false;
	// Green-button zoom and full screen both mean "take the whole display" to
	// the user, so both put the grid into its tiled layout.
	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) return true;
	return win.isZoomed;
}

void gdxsv_multi_pov_window_unmaximize() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;

	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) {
		// Leaving full screen is animated and takes a moment; the next tick
		// picks up where it lands.
		[win toggleFullScreen:nil];
		return;
	}
	if (win.isZoomed) [win zoom:nil];
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};

	NSScreen* screen = win.screen;
	if (screen == nil) screen = [NSScreen mainScreen];
	if (screen == nil) return {};

	// visibleFrame, not frame: the menu bar and the Dock are not ours to tile
	// over.
	return FromCocoa(screen.visibleFrame);
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;

	if (borderless) {
		// Keep it resizable so AppKit still lets us set an arbitrary frame.
		win.styleMask = NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable;
	} else {
		win.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
						NSWindowStyleMaskResizable;
	}
}

