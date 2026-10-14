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

// Keep SDL's native window, input responder and GPU surface together. Only
// the temporary parent owns the native title bar; its child covers the
// parent's rounded content corners with a rectangular game surface.
@interface GdxsvMultiPovTitleWindow : NSWindow <NSWindowDelegate>
@property(nonatomic, assign) NSWindow* game;
@property(nonatomic, assign) SDL_Window* sdl;
@property(nonatomic, assign) BOOL savedBorderless;
@property(nonatomic, assign) BOOL gridFullscreen;
@property(nonatomic, assign) BOOL syncingSize;
- (void)Attach:(SDL_Window*)window native:(NSWindow*)game;
- (void)SyncContent;
- (void)SetGridFullscreen:(BOOL)fullscreen;
- (void)Restore;
+ (void)FlatWindowWillClose:(NSNotification*)notification;
@end

namespace {
struct GdxsvMultiPovAppleWindowState {
	GdxsvMultiPovTitleWindow* title = nil;
	NSWindow* flatWindow = nil;
	BOOL savedShadow = YES;
};
GdxsvMultiPovAppleWindowState gdxsv_window;
}  // namespace

@implementation GdxsvMultiPovTitleWindow
// SDL continues to own keyboard focus when the native caption is clicked.
- (BOOL)canBecomeKeyWindow {
	return NO;
}
- (BOOL)canBecomeMainWindow {
	return YES;
}

- (void)Attach:(SDL_Window*)window native:(NSWindow*)game {
	self.game = game;
	self.sdl = window;
	self.savedBorderless = (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0;
	self.releasedWhenClosed = NO;
	self.hasShadow = NO;
	self.title = game.title;
	self.delegate = self;
	// Fullscreen belongs to the four-window grid, not an individual Space.
	self.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone | NSWindowCollectionBehaviorFullScreenDisallowsTiling;
	NSButton* fullscreen = [self standardWindowButton:NSWindowZoomButton];
	fullscreen.target = self;
	fullscreen.action = @selector(toggleFullScreen:);
	self.contentMinSize = game.contentMinSize;
	self.contentMaxSize = game.contentMaxSize;
	[[NSNotificationCenter defaultCenter] addObserver:self
											 selector:@selector(GameWillClose:)
												 name:NSWindowWillCloseNotification
											   object:game];
	[[NSNotificationCenter defaultCenter] addObserver:self
											 selector:@selector(GameDidResize:)
												 name:NSWindowDidResizeNotification
											   object:game];
	SDL_SetWindowBordered(window, SDL_FALSE);
	game.hasShadow = NO;
	[self SyncContent];
	[self addChildWindow:game ordered:NSWindowAbove];
	[self orderFront:nil];
	[game makeKeyAndOrderFront:nil];
	[self makeMainWindow];
}

- (void)SyncContent {
	if (self.game == nil || self.gridFullscreen || self.syncingSize) return;
	self.syncingSize = YES;
	const NSRect content = [self contentRectForFrameRect:self.frame];
	int width, height;
	SDL_GetWindowSize(self.sdl, &width, &height);
	const int w = static_cast<int>(std::lround(content.size.width));
	const int h = static_cast<int>(std::lround(content.size.height));
	if (width != w || height != h) SDL_SetWindowSize(self.sdl, w, h);
	[self.game setFrameOrigin:content.origin];
	self.syncingSize = NO;
}

- (void)windowDidResize:(NSNotification*)notification {
	[self SyncContent];
}

- (void)setFrame:(NSRect)frame display:(BOOL)display {
	[super setFrame:frame display:display];
	// AppKit can move child windows after sending the resize notification.
	[self SyncContent];
}

- (void)GameDidResize:(NSNotification*)notification {
	if (self.game.parentWindow != self || self.syncingSize) return;
	self.syncingSize = YES;
	const NSRect content = self.game.frame;
	[self setFrame:[self frameRectForContentRect:content] display:YES];
	// Moving the parent also moves its child. Keep the edge the user dragged.
	[self.game setFrameOrigin:content.origin];
	self.syncingSize = NO;
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
	// Flycast handles SDL_QUIT for its main window, as SDL's native close does.
	SDL_Event event{};
	event.type = SDL_QUIT;
	SDL_PushEvent(&event);
	return NO;
}

- (void)toggleFullScreen:(id)sender {
	gdxsv_multi_pov_toggle_fullscreen();
}

- (void)SetGridFullscreen:(BOOL)fullscreen {
	if (self.gridFullscreen == fullscreen) return;
	self.gridFullscreen = fullscreen;
	if (fullscreen) {
		[self removeChildWindow:self.game];
		[self orderOut:nil];
	} else {
		[self setFrame:[self frameRectForContentRect:self.game.frame] display:YES];
		[self addChildWindow:self.game ordered:NSWindowAbove];
		[self orderFront:nil];
	}
	[self.game makeKeyAndOrderFront:nil];
	if (!fullscreen) [self makeMainWindow];
}

- (void)Restore {
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	const NSRect content = self.game.frame;
	[self removeChildWindow:self.game];
	self.delegate = nil;
	[self close];
	SDL_SetWindowBordered(self.sdl, self.savedBorderless ? SDL_FALSE : SDL_TRUE);
	[self.game setFrame:[self.game frameRectForContentRect:content] display:YES];
	[self.game makeKeyAndOrderFront:nil];
	if (gdxsv_window.flatWindow == self.game) gdxsv_window.flatWindow = nil;
	self.game = nil;
	self.sdl = nullptr;
}

- (void)GameWillClose:(NSNotification*)notification {
	// SDL can destroy/recreate its window when switching renderers or quitting.
	// Tear down the helper without calling back into the dying SDL window.
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	[self removeChildWindow:self.game];
	self.delegate = nil;
	[self close];
	gdxsv_window.title = nil;
	gdxsv_window.flatWindow = nil;
	self.game = nil;
	self.sdl = nullptr;
	[self autorelease];
}

+ (void)FlatWindowWillClose:(NSNotification*)notification {
	[[NSNotificationCenter defaultCenter] removeObserver:self name:NSWindowWillCloseNotification object:notification.object];
	gdxsv_window.flatWindow = nil;
}
@end

namespace {
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

static NSWindow* FrameWindow() {
	if (gdxsv_window.title != nil && !gdxsv_window.title.gridFullscreen) {
		if (![gdxsv_window.title.title isEqualToString:gdxsv_window.title.game.title])
			gdxsv_window.title.title = gdxsv_window.title.game.title;
		return gdxsv_window.title;
	}
	return CocoaWindow();
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

static NSRect ToCocoa(const GdxsvMultiPovRect& r) { return NSMakeRect(r.x, PrimaryHeight() - (r.y + r.h), r.w, r.h); }
}  // namespace

bool gdxsv_multi_pov_window_available() { return CocoaWindow() != nil; }

GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state() {
	GdxsvMultiPovWindowState state;
	NSWindow* win = CocoaWindow();
	if (win == nil) return state;
	state.frame = gdxsv_multi_pov_window_get_frame();
	if ((SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0)
		state.mode = GdxsvMultiPovWindowMode::Fullscreen;
	else if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0)
		state.mode = GdxsvMultiPovWindowMode::NativeFullscreen;
	else if (win.isZoomed)
		state.mode = GdxsvMultiPovWindowMode::Maximized;
	return state;
}

void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState& state) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;
	if (gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_unmaximize();
	// Position on the original display before restoring its window mode.
	gdxsv_multi_pov_window_set_frame(state.frame);
	if (state.mode == GdxsvMultiPovWindowMode::Maximized && !win.isZoomed)
		[win zoom:nil];
	else if (state.mode == GdxsvMultiPovWindowMode::NativeFullscreen) {
		// SDL fullscreen suppresses the native menu action and menu-bar reveal.
		// Keep its flags clear when restoring a native macOS fullscreen window.
		if ((win.styleMask & NSWindowStyleMaskFullScreen) == 0) [win toggleFullScreen:nil];
	} else if (state.mode == GdxsvMultiPovWindowMode::Fullscreen &&
			   SDL_SetWindowFullscreen(sdl_get_window(), SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		WARN_LOG(COMMON, "multi-pov: cannot restore full screen: %s", SDL_GetError());
}

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	NSWindow* win = FrameWindow();
	if (win == nil) return {};
	return FromCocoa([win contentRectForFrameRect:win.frame]);
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	NSWindow* win = FrameWindow();
	if (win == nil || rect.w <= 0 || rect.h <= 0) return;

	// setFrame: moves without ordering the window front.
	const NSRect content = ToCocoa(rect);
	[win setFrame:[win frameRectForContentRect:content] display:YES];
}

bool gdxsv_multi_pov_window_is_maximized() {
	NSWindow* win = FrameWindow();
	if (win == nil) return false;
	// Zoom and full screen both tile the grid.
	if ((CocoaWindow().styleMask & NSWindowStyleMaskFullScreen) != 0) return true;
	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) return true;
	if ((SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0) return true;
	return win.isZoomed;
}

void gdxsv_multi_pov_window_unmaximize() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;

	// Adopt fullscreen entered through the green button before leaving via
	// SDL. SDL waits for the animation, so AppKit cannot overwrite our frame.
	SDL_Window* w = sdl_get_window();
	const bool native_fullscreen = (win.styleMask & NSWindowStyleMaskFullScreen) != 0;
	if ((native_fullscreen && SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) || SDL_SetWindowFullscreen(w, 0) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot leave full screen: %s", SDL_GetError());
		return;
	}
	win = FrameWindow();
	if (win.isZoomed) [win zoom:nil];
	[gdxsv_window.title SyncContent];
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
	gdxsv_window.title.level = win.level;
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	NSWindow* win = FrameWindow();
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
	if (gdxsv_window.title != nil) {
		[gdxsv_window.title SetGridFullscreen:borderless];
		return;
	}
	NSWindow* win = CocoaWindow();
	if (win == nil) return;
	SDL_SetWindowBordered(sdl_get_window(), borderless ? SDL_FALSE : SDL_TRUE);
}

void gdxsv_multi_pov_window_set_flat(bool flat) {
	NSWindow* win = CocoaWindow();
	if (!flat) {
		[[NSNotificationCenter defaultCenter] removeObserver:[GdxsvMultiPovTitleWindow class]
													   name:NSWindowWillCloseNotification
													 object:nil];
		if (gdxsv_window.flatWindow == win) win.hasShadow = gdxsv_window.savedShadow;
		GdxsvMultiPovTitleWindow* title = gdxsv_window.title;
		gdxsv_window.title = nil;
		[title Restore];
		[title release];
		gdxsv_window.flatWindow = nil;
		return;
	}
	if (win == nil) return;
	if (gdxsv_window.flatWindow == win) return;
	if (gdxsv_multi_pov_current_role() == GdxsvMultiPovRole::Host) {
		// Save only the normal style. SDL fullscreen temporarily removes it.
		if (gdxsv_multi_pov_window_is_maximized()) {
			gdxsv_multi_pov_window_unmaximize();
			if (gdxsv_multi_pov_window_is_maximized()) return;
		}
		gdxsv_window.title =
			[[GdxsvMultiPovTitleWindow alloc] initWithContentRect:[win contentRectForFrameRect:win.frame]
														styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
																  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
														  backing:NSBackingStoreBuffered
															defer:NO];
		if (gdxsv_window.title == nil) return;
		gdxsv_window.savedShadow = win.hasShadow;
		[gdxsv_window.title Attach:sdl_get_window() native:win];
	} else {
		gdxsv_window.savedShadow = win.hasShadow;
		win.hasShadow = NO;
		[[NSNotificationCenter defaultCenter] addObserver:[GdxsvMultiPovTitleWindow class]
												 selector:@selector(FlatWindowWillClose:)
													 name:NSWindowWillCloseNotification
												   object:win];
	}
	gdxsv_window.flatWindow = win;
}

// hasShadow = NO already drops the shadow.
int64_t gdxsv_multi_pov_window_native_handle() { return 0; }

void gdxsv_multi_pov_window_stay_below(const int64_t*, int) {}
