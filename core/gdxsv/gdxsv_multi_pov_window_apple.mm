// macOS backend for the 4-player replay grid. Cocoa rather than SDL: AppKit
// uses bottom-left frame rects, the grid top-left content rects, and the
// conversion is done here against the real NSWindow.
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

// AppKit measures from the primary screen, whose height is the flip axis.
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

// Leaving full screen: AppKit restores the pre-full-screen frame at the end of
// the exit, so the host keeps re-applying its quadrant until it holds.
static bool g_leaving_fullscreen = false;
static int g_settled_ticks = 0;
static constexpr int kSettleTicks = 30;

bool gdxsv_multi_pov_window_available() { return CocoaWindow() != nil; }

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};
	return FromCocoa([win contentRectForFrameRect:win.frame]);
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	NSWindow* win = CocoaWindow();
	if (win == nil || rect.w <= 0 || rect.h <= 0) return;

	// setFrame: moves without ordering the window front.
	const NSRect content = ToCocoa(rect);
	[win setFrame:[win frameRectForContentRect:content] display:YES];

	if (g_leaving_fullscreen && (win.styleMask & NSWindowStyleMaskFullScreen) == 0) {
		if (FromCocoa([win contentRectForFrameRect:win.frame]) == rect) {
			if (++g_settled_ticks >= kSettleTicks) g_leaving_fullscreen = false;
		} else {
			g_settled_ticks = 0;
		}
	}
}

bool gdxsv_multi_pov_window_is_maximized() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return false;
	// Zoom and full screen both tile the grid.
	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) return true;
	if (g_leaving_fullscreen) return true;
	return win.isZoomed;
}

void gdxsv_multi_pov_window_unmaximize() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;

	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) {
		// Animated; asked once, or a second toggle would go back in.
		if (!g_leaving_fullscreen) [win toggleFullScreen:nil];
		g_leaving_fullscreen = true;
		g_settled_ticks = 0;
		return;
	}
	if (g_leaving_fullscreen) return;
	if (win.isZoomed) [win zoom:nil];
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};

	NSScreen* screen = win.screen;
	if (screen == nil) screen = [NSScreen mainScreen];
	if (screen == nil) return {};

	return FromCocoa(screen.visibleFrame);
}

GdxsvMultiPovRect gdxsv_multi_pov_window_display_area() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};

	NSScreen* screen = win.screen;
	if (screen == nil) screen = [NSScreen mainScreen];
	if (screen == nil) return {};

	return FromCocoa(screen.frame);
}

void gdxsv_multi_pov_window_set_topmost(bool topmost) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;
	win.level = topmost ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	NSWindow* win = CocoaWindow();
	if (win == nil) return out;

	const NSRect content = NSMakeRect(0, 0, 100, 100);
	const NSRect frame = [win frameRectForContentRect:content];
	out.left = static_cast<int32_t>(std::lround(NSMinX(content) - NSMinX(frame)));
	out.right = static_cast<int32_t>(std::lround(NSMaxX(frame) - NSMaxX(content)));
	out.top = static_cast<int32_t>(std::lround(NSMaxY(frame) - NSMaxY(content)));
	out.bottom = static_cast<int32_t>(std::lround(NSMinY(content) - NSMinY(frame)));
	return out;
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

