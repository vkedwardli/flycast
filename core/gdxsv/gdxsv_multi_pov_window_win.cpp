// Win32 backend for the 4-player replay grid.
//
// Native rather than through SDL because of one thing SDL cannot express: a
// follower has to be moved without being activated. SDL_SetWindowPosition
// raises and focuses; SetWindowPos with SWP_NOACTIVATE | SWP_NOZORDER does
// not, which is what keeps the keyboard on the screen the user is driving
// while the other three are dragged along.
#include "gdxsv_multi_pov_window.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "types.h"

// Defined in core/sdl/sdl.cpp.
HWND getNativeHwnd();

static HWND Hwnd() { return gdxsv_headless() ? nullptr : getNativeHwnd(); }

// The window rect that produces the wanted client rect. Flycast's window has
// no menu bar, so bMenu is FALSE.
static RECT ClientToWindowRect(HWND hwnd, const GdxsvMultiPovRect& client) {
	RECT r{client.x, client.y, client.x + client.w, client.y + client.h};
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
	const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
	AdjustWindowRectEx(&r, style, FALSE, ex_style);
	return r;
}

bool gdxsv_multi_pov_window_available() { return Hwnd() != nullptr; }

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	GdxsvMultiPovRect out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	RECT client{};
	POINT origin{0, 0};
	if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return out;
	out.x = origin.x;
	out.y = origin.y;
	out.w = client.right - client.left;
	out.h = client.bottom - client.top;
	return out;
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr || rect.w <= 0 || rect.h <= 0) return;

	const RECT r = ClientToWindowRect(hwnd, rect);
	SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
}

bool gdxsv_multi_pov_window_is_maximized() {
	HWND hwnd = Hwnd();
	return hwnd != nullptr && IsZoomed(hwnd);
}

void gdxsv_multi_pov_window_unmaximize() {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	// SW_RESTORE rather than ShowWindow(SW_SHOWNORMAL): it keeps the window on
	// the display it was maximized on, which is the one the grid belongs to.
	ShowWindow(hwnd, SW_RESTORE);
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	GdxsvMultiPovRect out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfo(monitor, &info)) return out;

	// rcWork, not rcMonitor: the taskbar is not ours to tile over.
	out.x = info.rcWork.left;
	out.y = info.rcWork.top;
	out.w = info.rcWork.right - info.rcWork.left;
	out.h = info.rcWork.bottom - info.rcWork.top;
	return out;
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	// AdjustWindowRectEx on an empty rect: what comes back is the frame alone,
	// negative on the sides the frame grows outwards.
	RECT r{0, 0, 0, 0};
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
	const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
	if (!AdjustWindowRectEx(&r, style, FALSE, ex_style)) return out;
	out.left = -r.left;
	out.top = -r.top;
	out.right = r.right;
	out.bottom = r.bottom;
	return out;
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;

	LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
	const LONG_PTR decorations = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
	const LONG_PTR wanted = borderless ? (style & ~decorations) : (style | decorations);
	if (wanted == style) return;

	SetWindowLongPtr(hwnd, GWL_STYLE, wanted);
	// The frame only changes once the window is told to recalculate it.
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

