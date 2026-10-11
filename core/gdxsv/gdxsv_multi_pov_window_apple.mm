// macOS backend for the 4-player replay grid. Cocoa rather than SDL: AppKit
// uses bottom-left frame rects, the grid top-left content rects, and the
// conversion is done here against the real NSWindow.
#include "gdxsv_multi_pov_window.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cmath>
#include <cstring>
#include <cstdlib>

#include <SDL.h>
#include <SDL_syswm.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

// AppKit's private _cornerMask hook keeps the real native title bar while
// changing only the bottom corners. Copy its native image so the top corners
// keep the exact macOS shape. The override is limited to SDLWindow and this
// host; methods are checked at runtime and restored when the session ends.
static NSWindow* flatHost;
static NSImage* flatMask;
static Class maskClass;
static IMP savedCornerMask;
static NSUInteger savedStyle;
static BOOL savedShadow;

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

static void SetStyleKeepingResponder(NSWindow* win, NSUInteger style) {
	// Like SDL's SetWindowStyle: setStyleMask can change the view's responder
	// chain, which otherwise disconnects the host's keyboard/mouse listener.
	NSResponder* next = [win.contentView.nextResponder retain];
	win.styleMask = style;
	win.contentView.nextResponder = next;
	[next release];
}

static NSImage* FlatCornerMask(id window, SEL selector) {
	if (window == flatHost && flatMask != nil) {
		const NSUInteger style = flatHost.styleMask;
		if ((style & NSWindowStyleMaskTitled) == 0) return nil; // square grid fullscreen
		if ((style & NSWindowStyleMaskFullScreen) == 0) return flatMask;
	}
	return savedCornerMask == nullptr ? nil :
		reinterpret_cast<NSImage* (*)(id, SEL)>(savedCornerMask)(window, selector);
}

static void RefreshCornerMask(NSWindow* win) {
	const SEL selector = NSSelectorFromString(@"_cornerMaskChanged");
	if ([win respondsToSelector:selector])
		reinterpret_cast<void (*)(id, SEL)>([win methodForSelector:selector])(win, selector);
	[win.contentView.superview setNeedsDisplay:YES];
	[win invalidateShadow];
}

static bool HasCornerMethod(Class cls, SEL selector, const char* resultType) {
	Method method = class_getInstanceMethod(cls, selector);
	if (method == nullptr || method_getNumberOfArguments(method) != 2) return false;
	char* type = method_copyReturnType(method);
	const bool valid = type != nullptr && std::strcmp(type, resultType) == 0;
	std::free(type);
	return valid;
}

static NSImage* CopyMaskWithSquareBottom(NSImage* original) {
	NSRect rect = NSMakeRect(0, 0, original.size.width, original.size.height);
	CGImageRef image = [original CGImageForProposedRect:&rect context:nil hints:nil];
	if (image == nullptr || CGImageGetBitsPerComponent(image) != 8 ||
		(CGImageGetBitsPerPixel(image) != 8 && CGImageGetBitsPerPixel(image) != 32)) return nil;
	const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
	const size_t stride = CGImageGetBytesPerRow(image), pixelSize = CGImageGetBitsPerPixel(image) / 8;
	CFDataRef source = CGDataProviderCopyData(CGImageGetDataProvider(image));
	if (source == nullptr) return nil;
	CFMutableDataRef pixels = CFDataCreateMutableCopy(nullptr, 0, source);
	CFRelease(source);
	if (pixels == nullptr) return nil;
	if (width == 0 || height == 0 || stride < width * pixelSize ||
		static_cast<size_t>(CFDataGetLength(pixels)) < stride * height) {
		CFRelease(pixels);
		return nil;
	}
	UInt8* data = CFDataGetMutableBytePtr(pixels);
	UInt8 opaque[4];
	std::memcpy(opaque, data + (height / 2) * stride + (width / 2) * pixelSize, pixelSize);
	// Retain every native top-cap pixel. Use the opaque centre pixel for the
	// entire lower mask, preserving native alpha/mask encoding and colorspace.
	const size_t topRows = static_cast<size_t>(std::lround(original.capInsets.top * height / original.size.height));
	for (size_t y = topRows; y < height; ++y)
		for (size_t x = 0; x < width; ++x)
			std::memcpy(data + y * stride + x * pixelSize, opaque, pixelSize);
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
	CFRelease(pixels);
	if (provider == nullptr) return nil;
	CGImageRef copy = CGImageIsMask(image) ?
		CGImageMaskCreate(width, height, CGImageGetBitsPerComponent(image), CGImageGetBitsPerPixel(image), stride,
			provider, CGImageGetDecode(image), CGImageGetShouldInterpolate(image)) :
		CGImageCreate(width, height, CGImageGetBitsPerComponent(image), CGImageGetBitsPerPixel(image), stride,
			CGImageGetColorSpace(image), CGImageGetBitmapInfo(image), provider, CGImageGetDecode(image),
			CGImageGetShouldInterpolate(image), CGImageGetRenderingIntent(image));
	CGDataProviderRelease(provider);
	if (copy == nullptr) return nil;
	NSImage* result = [[NSImage alloc] initWithCGImage:copy size:original.size];
	CGImageRelease(copy);
	result.capInsets = original.capInsets;
	result.resizingMode = original.resizingMode;
	return result;
}

static void ApplyFlatHost() {
	NSWindow* win = CocoaWindow();
	if (win == nil || win != flatHost || flatMask != nil) return;
	// Read the normal window's native mask after leaving zoom/fullscreen.
	if (win.isZoomed || (win.styleMask & NSWindowStyleMaskFullScreen) != 0 ||
		(SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0) return;
	const SEL selector = NSSelectorFromString(@"_cornerMask");
	Class cls = NSClassFromString(@"SDLWindow");
	if (cls == Nil || ![win isKindOfClass:cls] || !HasCornerMethod(cls, selector, "@") ||
		!HasCornerMethod(object_getClass(win), NSSelectorFromString(@"_cornerMaskChanged"), "v")) {
		WARN_LOG(COMMON, "multi-pov: native corner-mask hooks unavailable; keeping native corners");
		return;
	}
	NSImage* original = reinterpret_cast<NSImage* (*)(id, SEL)>([win methodForSelector:selector])(win, selector);
	if (![original isKindOfClass:NSImage.class] ||
		!std::isfinite(original.size.width) || !std::isfinite(original.size.height) || !std::isfinite(original.capInsets.top) ||
		original.size.width <= 0 || original.size.height <= 0 ||
		original.capInsets.top <= 0 || original.capInsets.top >= original.size.height) {
		WARN_LOG(COMMON, "multi-pov: native corner mask unavailable; keeping native corners");
		return;
	}
	flatMask = CopyMaskWithSquareBottom(original);
	if (flatMask == nil) return;
	maskClass = cls;
	Method method = class_getInstanceMethod(cls, selector);
	savedCornerMask = method_getImplementation(method);
	// Add/replace only SDLWindow's implementation, leaving NSWindow and SDL's
	// KVO instance class intact. Other SDL windows still use the original IMP.
	class_replaceMethod(cls, selector, reinterpret_cast<IMP>(FlatCornerMask), method_getTypeEncoding(method));
	RefreshCornerMask(win);
}

static void RestoreFlatHost() {
	if (flatHost == nil) return;
	NSWindow* win = flatHost;
	if (flatMask != nil) {
		const SEL selector = NSSelectorFromString(@"_cornerMask");
		class_replaceMethod(maskClass, selector, savedCornerMask,
			method_getTypeEncoding(class_getInstanceMethod(maskClass, selector)));
		[flatMask release];
		flatMask = nil;
		maskClass = Nil;
		savedCornerMask = nullptr;
		RefreshCornerMask(win);
	}
	win.hasShadow = savedShadow;
	flatHost = nil;
	[win release];
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
		if ((win.styleMask & NSWindowStyleMaskFullScreen) == 0)
			[win toggleFullScreen:nil];
	}
	else if (state.mode == GdxsvMultiPovWindowMode::Fullscreen &&
		SDL_SetWindowFullscreen(sdl_get_window(), SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		WARN_LOG(COMMON, "multi-pov: cannot restore full screen: %s", SDL_GetError());
}

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
}

bool gdxsv_multi_pov_window_is_maximized() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return false;
	// Zoom and full screen both tile the grid.
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
	if ((native_fullscreen && SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) ||
		SDL_SetWindowFullscreen(w, 0) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot leave full screen: %s", SDL_GetError());
		return;
	}
	if (win.isZoomed) [win zoom:nil];
	// SDL's exclusive fullscreen temporarily uses a borderless style. Save
	// the normal caption after leaving it, before grid fullscreen can toggle.
	if (win == flatHost && (win.styleMask & NSWindowStyleMaskTitled) != 0)
		savedStyle = win.styleMask & ~NSWindowStyleMaskFullScreen;
	ApplyFlatHost();
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
		SetStyleKeepingResponder(win, NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable);
	} else {
		SetStyleKeepingResponder(win, win == flatHost ? savedStyle :
			NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
			NSWindowStyleMaskResizable);
	}
	if (win == flatHost && flatMask != nil) RefreshCornerMask(win);
}

void gdxsv_multi_pov_window_set_flat(bool flat) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;
	if (!flat && flatHost != nil) {
		RestoreFlatHost();
		return;
	}
	if (flat && gdxsv_multi_pov_current_role() == GdxsvMultiPovRole::Host) {
		if (flatHost == nil) {
			flatHost = [win retain];
			savedStyle = win.styleMask & ~NSWindowStyleMaskFullScreen;
			savedShadow = win.hasShadow;
		}
		ApplyFlatHost();
	}
	win.hasShadow = !flat;
}

// hasShadow = NO already drops the shadow.
int64_t gdxsv_multi_pov_window_native_handle() { return 0; }

void gdxsv_multi_pov_window_stay_below(const int64_t*, int) {}
