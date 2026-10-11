// Standalone AppKit/SDL integration check, linked with the real grid backend.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include "gdxsv/gdxsv_multi_pov_window.h"
#include "log/Log.h"
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

static SDL_Window* window;
static CGFloat nativeCornerExtent;
static bool skippedChecks;
// Exercise the CI fallback path on a desktop that supports all capabilities.
static bool disableOptionalChecks;
static void skip(const char* reason) {
	skippedChecks = true;
	std::fprintf(stderr, "SKIP: %s\n", reason);
}
static bool hasCornerMethod(NSWindow* win, SEL selector, const char* returnType) {
	Method method = class_getInstanceMethod(object_getClass(win), selector);
	if (method == nullptr || method_getNumberOfArguments(method) != 2) return false;
	char type[16] {};
	method_getReturnType(method, type, sizeof(type));
	return std::strcmp(type, returnType) == 0;
}
SDL_Window* sdl_get_window() { return window; }
bool gdxsv_headless() { return false; }
GdxsvMultiPovRole gdxsv_multi_pov_current_role() { return GdxsvMultiPovRole::Host; }
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char*, int, const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}
static void pump() {
	for (int i = 0; i < 10; ++i) { SDL_PumpEvents(); SDL_Delay(10); }
}
static NSWindow* nativeWindow(SDL_Window* w) {
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	CHECK(SDL_GetWindowWMInfo(w, &info));
	return info.info.cocoa.window;
}
static NSBitmapImageRep* capture(NSWindow* win, NSString* path = nil) {
	if (disableOptionalChecks || !CGPreflightScreenCaptureAccess()) {
		skip("screen capture unavailable; pixel checks omitted");
		return nil;
	}
	CGImageRef image = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow,
		static_cast<CGWindowID>(win.windowNumber), kCGWindowImageBoundsIgnoreFraming);
	if (image == nullptr) {
		skip("window capture unavailable; pixel checks omitted");
		return nil;
	}
	NSBitmapImageRep* bitmap = [[[NSBitmapImageRep alloc] initWithCGImage:image] autorelease];
	CGImageRelease(image);
	if (path) CHECK([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]);
	return bitmap;
}
static void checkContentSize(int w, int h) {
	int actualW, actualH;
	SDL_GetWindowSize(window, &actualW, &actualH);
	CHECK(actualW == w && actualH == h);
}
static void draw(SDL_Renderer* renderer) {
	SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
	SDL_RenderClear(renderer);
	SDL_RenderPresent(renderer);
	pump();
}
static void checkCorners(NSWindow* win, NSBitmapImageRep* bitmap, NSBitmapImageRep* baseline) {
	if (bitmap == nil || baseline == nil) return;
	const NSRect body = [win contentRectForFrameRect:win.frame];
	const CGFloat scale = bitmap.pixelsWide / NSWidth(win.frame);
	const NSInteger gameTop = std::lround((NSMaxY(win.frame) - NSMaxY(body)) * scale);
	CHECK(gameTop > 0);
	for (NSInteger x : {NSInteger(0), bitmap.pixelsWide - 1}) {
		CHECK([bitmap colorAtX:x y:0].alphaComponent < 0.01);
		// Preserve AppKit's original one-point separator below the title bar.
		for (NSInteger y : {gameTop + static_cast<NSInteger>(std::ceil(scale)), bitmap.pixelsHigh - 1}) {
			NSColor* color = [[bitmap colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
			printf("game corner %ld,%ld: %.3f %.3f %.3f %.3f\n", long(x), long(y),
				color.redComponent, color.greenComponent, color.blueComponent, color.alphaComponent);
			CHECK(color.alphaComponent > 0.99);
			CHECK(color.redComponent > 0.85 && color.blueComponent > 0.85 && color.greenComponent < 0.5);
		}
	}
	// Compare actual native top-corner alpha, not just our mask image.
	// AppKit's cap contains the curved corner. Its straight one-pixel frame
	// outside that cap can change opacity when the native shadow is disabled.
	const NSInteger extent = std::lround(nativeCornerExtent * scale);
	for (NSInteger dx = 0; dx < extent; ++dx)
		for (NSInteger y = 0; y < extent; ++y)
			for (bool right : {false, true}) {
				const NSInteger x = right ? bitmap.pixelsWide - 1 - dx : dx;
				const NSInteger originalX = right ? baseline.pixelsWide - 1 - dx : dx;
				CHECK(std::abs([bitmap colorAtX:x y:y].alphaComponent - [baseline colorAtX:originalX y:y].alphaComponent) < 0.01);
			}
	puts("native top-corner alpha matches the original window exactly");
}

int main(int argc, char** argv) {
	@autoreleasepool {
		disableOptionalChecks = std::getenv("FLYCAST_WINDOW_TEST_NO_OPTIONAL_CHECKS") != nullptr;
		SDL_SetMainReady();
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			skip(SDL_GetError());
			SDL_Quit();
			return 77;
		}
		window = SDL_CreateWindow("Flycast native title-bar check", 180, 180, 480, 360, SDL_WINDOW_RESIZABLE);
		if (window == nullptr) {
			skip(SDL_GetError());
			SDL_Quit();
			return 77;
		}
		SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
		CHECK(renderer);
		draw(renderer);
		NSWindow* win = nativeWindow(window);
		const NSUInteger style = win.styleMask;
		const BOOL shadow = win.hasShadow;
		const Class originalClass = object_getClass(win);
		const auto original = gdxsv_multi_pov_window_get_state();
		// Starting while SDL uses exclusive fullscreen saves a temporary
		// borderless style. Leaving fullscreen must establish the normal native
		// caption before the grid's own borderless toggle restores that style.
		SDL_DisplayMode mode;
		const bool exclusiveSupported = !disableOptionalChecks && SDL_GetCurrentDisplayMode(0, &mode) == 0 &&
			SDL_SetWindowDisplayMode(window, &mode) == 0 && SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) == 0;
		if (exclusiveSupported) {
			pump();
			gdxsv_multi_pov_window_set_flat(true);
			gdxsv_multi_pov_window_unmaximize();
			pump();
			CHECK((win.styleMask & NSWindowStyleMaskTitled) != 0);
			gdxsv_multi_pov_window_set_borderless(true);
			CHECK((win.styleMask & NSWindowStyleMaskTitled) == 0);
			gdxsv_multi_pov_window_set_borderless(false);
			pump();
			CHECK((win.styleMask & NSWindowStyleMaskTitled) != 0);
			CHECK([win standardWindowButton:NSWindowCloseButton] != nil);
			gdxsv_multi_pov_window_set_flat(false);
			gdxsv_multi_pov_window_restore_state(original);
			win.hasShadow = shadow; // restore this test's initial windowed baseline
		} else {
			skip("exclusive fullscreen unavailable; fullscreen-start checks omitted");
			CHECK(SDL_SetWindowFullscreen(window, 0) == 0);
		}
		draw(renderer);
		NSResponder* responder = win.contentView.nextResponder;
		NSButton* close = [win standardWindowButton:NSWindowCloseButton];
		NSButton* minimize = [win standardWindowButton:NSWindowMiniaturizeButton];
		NSButton* zoom = [win standardWindowButton:NSWindowZoomButton];
		CHECK(close && minimize && zoom);
		const SEL corner = NSSelectorFromString(@"_cornerMask");
		IMP originalCorner = nullptr;
		NSBitmapImageRep* baseline = nil;
		if (!disableOptionalChecks && hasCornerMethod(win, corner, "@") &&
				hasCornerMethod(win, NSSelectorFromString(@"_cornerMaskChanged"), "v")) {
			originalCorner = [win methodForSelector:corner];
			NSImage* mask = reinterpret_cast<NSImage* (*)(id, SEL)>(originalCorner)(win, corner);
			if ([mask isKindOfClass:NSImage.class] && std::isfinite(mask.capInsets.top) &&
					mask.capInsets.top > 0 && mask.capInsets.top < mask.size.height) {
				nativeCornerExtent = mask.capInsets.top;
				baseline = capture(win, argc > 1 ? [[NSString stringWithUTF8String:argv[1]] stringByAppendingString:@"-original.png"] : nil);
				if (baseline != nil) CHECK([baseline colorAtX:0 y:baseline.pixelsHigh - 1].alphaComponent < 0.01);
			} else skip("native corner mask unavailable; corner checks omitted");
		} else skip("private corner-mask hooks unavailable; corner checks omitted");
		for (int cycle = 0; cycle < 3; ++cycle) {
			gdxsv_multi_pov_window_set_flat(true);
			draw(renderer);
			CHECK(win.styleMask == style && !win.hasShadow);
			CHECK(object_getClass(win) == originalClass);
			CHECK(win.contentView.nextResponder == responder);
			CHECK(win.childWindows.count == 0);
			CHECK([win standardWindowButton:NSWindowCloseButton] == close);
			CHECK([win standardWindowButton:NSWindowMiniaturizeButton] == minimize);
			CHECK([win standardWindowButton:NSWindowZoomButton] == zoom);
			CHECK(gdxsv_multi_pov_window_get_frame() == original.frame);
			checkContentSize(480, 360);
			checkCorners(win, capture(win, cycle == 0 && argc > 1 ? [[NSString stringWithUTF8String:argv[1]] stringByAppendingString:@"-initial.png"] : nil), baseline);
			const GdxsvMultiPovRect resized {220, 220, 560, 420};
			gdxsv_multi_pov_window_set_frame(resized);
			draw(renderer);
			CHECK(gdxsv_multi_pov_window_get_frame() == resized);
			checkContentSize(560, 420);
			CHECK(gdxsv_multi_pov_window_frame_insets().top >= 22);
			checkCorners(win, capture(win, cycle == 0 && argc > 1 ? [NSString stringWithUTF8String:argv[1]] : nil), baseline);
			if (cycle == 0) {
				// The host-only override must leave another genuine SDL window rounded.
				SDL_Window* peer = SDL_CreateWindow("Unmodified native peer", 850, 300, 240, 180, SDL_WINDOW_RESIZABLE);
				CHECK(peer);
				pump();
				NSBitmapImageRep* peerImage = capture(nativeWindow(peer));
				if (peerImage != nil && baseline != nil) CHECK([peerImage colorAtX:0 y:peerImage.pixelsHigh - 1].alphaComponent < 0.01);
				SDL_DestroyWindow(peer);
				if (CGPreflightPostEventAccess()) {
					[NSApp activateIgnoringOtherApps:YES];
					[win makeKeyAndOrderFront:nil];
					pump();
					CGEventRef pointer = CGEventCreate(nullptr);
					const CGPoint savedPointer = CGEventGetLocation(pointer);
					CFRelease(pointer);
					const auto before = gdxsv_multi_pov_window_get_frame();
					const CGPoint down = CGPointMake(before.x + before.w / 2, before.y - gdxsv_multi_pov_window_frame_insets().top / 2);
					std::thread input([down]() {
						for (int step = 0; step < 3; ++step) {
							std::this_thread::sleep_for(std::chrono::milliseconds(150));
							const CGEventType types[] = {kCGEventLeftMouseDown, kCGEventLeftMouseDragged, kCGEventLeftMouseUp};
							CGPoint point = step == 0 ? down : CGPointMake(down.x + 64, down.y + 24);
							CGEventRef event = CGEventCreateMouseEvent(nullptr, types[step], point, kCGMouseButtonLeft);
							CGEventPost(kCGHIDEventTap, event);
							CFRelease(event);
						}
					});
					for (int tick = 0; tick < 12; ++tick) pump();
					input.join();
					CGEventRef restorePointer = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, savedPointer, kCGMouseButtonLeft);
					CGEventPost(kCGHIDEventTap, restorePointer);
					CFRelease(restorePointer);
					const auto moved = gdxsv_multi_pov_window_get_frame();
					printf("native drag: %d,%d -> %d,%d\n", before.x, before.y, moved.x, moved.y);
					CHECK(moved.x == before.x + 64 && moved.y == before.y + 24);
					gdxsv_multi_pov_window_set_frame(before);
				} else skip("native drag check: input permission unavailable");
				[[win standardWindowButton:NSWindowMiniaturizeButton] performClick:nil];
				for (int tick = 0; tick < 5; ++tick) pump();
				CHECK(win.isMiniaturized);
				[win deminiaturize:nil];
				for (int tick = 0; tick < 5; ++tick) pump();
				CHECK(!win.isMiniaturized && win.isVisible);
			}
			// Grid fullscreen temporarily removes the native caption too.
			gdxsv_multi_pov_window_set_borderless(true);
			gdxsv_multi_pov_window_set_frame(resized);
			draw(renderer);
			CHECK((win.styleMask & NSWindowStyleMaskTitled) == 0);
			CHECK(gdxsv_multi_pov_window_frame_insets().top == 0);
			NSBitmapImageRep* fullImage = capture(win);
			if (fullImage != nil && baseline != nil)
				for (NSInteger x : {NSInteger(0), fullImage.pixelsWide - 1})
					for (NSInteger y : {NSInteger(0), fullImage.pixelsHigh - 1})
						CHECK([fullImage colorAtX:x y:y].alphaComponent > 0.99);
			gdxsv_multi_pov_window_set_borderless(false);
			gdxsv_multi_pov_window_set_frame(resized);
			draw(renderer);
			CHECK(win.styleMask == style && win.childWindows.count == 0);
			checkCorners(win, capture(win), baseline);
			[[win standardWindowButton:NSWindowCloseButton] performClick:nil];
			bool closeReceived = false;
			SDL_Event closeEvent;
			while (SDL_PollEvent(&closeEvent))
				if (closeEvent.type == SDL_WINDOWEVENT && closeEvent.window.event == SDL_WINDOWEVENT_CLOSE &&
					closeEvent.window.windowID == SDL_GetWindowID(window)) closeReceived = true;
			CHECK(closeReceived);
			gdxsv_multi_pov_window_set_flat(false);
			gdxsv_multi_pov_window_restore_state(original);
			draw(renderer);
			CHECK(win.styleMask == style && win.hasShadow == shadow);
			CHECK(win.contentView.nextResponder == responder);
			if (originalCorner != nullptr) CHECK([win methodForSelector:corner] == originalCorner);
			CHECK(gdxsv_multi_pov_window_get_frame() == original.frame);
			checkContentSize(480, 360);
			NSBitmapImageRep* restored = capture(win);
			if (restored != nil && baseline != nil) CHECK([restored colorAtX:0 y:restored.pixelsHigh - 1].alphaComponent < 0.01);
			// AppKit recreates its standard button views on caption toggles.
			close = [win standardWindowButton:NSWindowCloseButton];
			minimize = [win standardWindowButton:NSWindowMiniaturizeButton];
			zoom = [win standardWindowButton:NSWindowZoomButton];
		}
		[win zoom:nil];
		pump();
		CHECK(win.isZoomed);
		auto zoomed = gdxsv_multi_pov_window_get_state();
		gdxsv_multi_pov_window_set_flat(true);
		gdxsv_multi_pov_window_unmaximize();
		draw(renderer);
		CHECK(!win.isZoomed && win.styleMask == style);
		zoomed.frame = gdxsv_multi_pov_window_get_frame();
		CHECK(zoomed.frame == original.frame);
		checkCorners(win, capture(win), baseline);
		gdxsv_multi_pov_window_set_flat(false);
		gdxsv_multi_pov_window_restore_state(zoomed);
		pump();
		CHECK(win.isZoomed && win.styleMask == style);
		[win zoom:nil];
		pump();
		CHECK(gdxsv_multi_pov_window_get_frame() == original.frame);
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
		puts("PASS: all supported native-window checks completed");
		return skippedChecks ? 77 : 0;
	}
}
