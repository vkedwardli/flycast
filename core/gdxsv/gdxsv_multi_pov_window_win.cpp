// Win32 backend for the 4-player replay grid. Native rather than SDL so a
// guest can be moved without being activated (SWP_NOACTIVATE).
#include "gdxsv_multi_pov_window.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
// clang-format on

#include "gdxsv_emu_hooks.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

// Defined in core/sdl/sdl.cpp.
HWND getNativeHwnd();

namespace {
static HWND Hwnd() { return gdxsv_headless() ? nullptr : getNativeHwnd(); }

struct GdxsvMultiPovWinWindowState {
	HWND flatHost = nullptr;
};
GdxsvMultiPovWinWindowState gdxsv_window;
static constexpr UINT_PTR kGdxsvFlatHostSubclass = 1;

static bool WindowArranged(HWND hwnd) {
	using IsWindowArrangedFn = BOOL(WINAPI*)(HWND);
	static const auto isArranged =
		reinterpret_cast<IsWindowArrangedFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged"));
	return isArranged != nullptr && isArranged(hwnd);
}

static UINT WindowDpi(HWND hwnd) {
	using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
	static const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	if (getDpi != nullptr) {
		const UINT dpi = getDpi(hwnd);
		if (dpi != 0) return dpi;
	}
	HDC dc = GetDC(hwnd);
	const UINT dpi = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if (dc != nullptr) ReleaseDC(hwnd, dc);
	return dpi != 0 ? dpi : 96;
}

// Keep the native caption, buttons and hit testing, but extend the client
// area over the side/bottom frame. DWM border color alone does not remove the
// thick Win10 resize frame. Only the host is subclassed, temporarily.
static LRESULT CALLBACK FlatHostWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
	if (message == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, FlatHostWindowProc, id);
		if (gdxsv_window.flatHost == hwnd) gdxsv_window.flatHost = nullptr;
		return DefSubclassProc(hwnd, message, wp, lp);
	}
	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	if (message == WM_NCCALCSIZE && (style & WS_CAPTION) == WS_CAPTION && !IsZoomed(hwnd)) {
		RECT* client = wp ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] : reinterpret_cast<RECT*>(lp);
		const RECT window = *client;
		DefSubclassProc(hwnd, message, wp, lp);
		const RECT nativeClient = *client;
		client->left = window.left;
		client->right = window.right;
		client->bottom = window.bottom;
		// Snap places the invisible resize frame outside the work area. Do not
		// extend an otherwise visible native client edge into that frame. Use
		// the proposed rectangle so a snap onto another monitor uses its area.
		MONITORINFO monitor{sizeof(MONITORINFO)};
		if (WindowArranged(hwnd) && GetMonitorInfoW(MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &monitor)) {
			if (window.left < monitor.rcWork.left && nativeClient.left >= monitor.rcWork.left) client->left = monitor.rcWork.left;
			if (window.right > monitor.rcWork.right && nativeClient.right <= monitor.rcWork.right) client->right = monitor.rcWork.right;
			if (window.bottom > monitor.rcWork.bottom && nativeClient.bottom <= monitor.rcWork.bottom)
				client->bottom = monitor.rcWork.bottom;
		}
		return 0;  // retain only the caption height calculated by Windows
	}
	const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
	if (message == WM_NCHITTEST && result == HTCLIENT && (style & WS_THICKFRAME) != 0 && !IsZoomed(hwnd) &&
		(SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) == 0) {
		// Replay controls and other ImGui interactions own their mouse input.
		if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) return HTCLIENT;
		// Retain resize handles inside the client edges whose visible frame
		// was removed. Signed coordinates also work on displays left of primary.
		RECT client;
		if (GetClientRect(hwnd, &client)) {
			POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
			ClientToScreen(hwnd, &topLeft);
			ClientToScreen(hwnd, &bottomRight);
			const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
			// Hit-test the rendered controls at the message's live coordinates.
			// WantCaptureMouse alone still describes the previous UI frame when
			// the pointer moves onto a resize strip and clicks immediately.
			if (ImGui::GetCurrentContext() != nullptr) {
				const ImGuiWindow* controls = ImGui::FindWindowByName("##gdxsv-replay-ctrlbar");
				const ImVec2 display = ImGui::GetIO().DisplaySize;
				if (controls != nullptr && (controls->Active || controls->WasActive) && client.right > 0 && client.bottom > 0 &&
					controls->Rect().Contains(
						ImVec2((x - topLeft.x) * display.x / client.right, (y - topLeft.y) * display.y / client.bottom)))
					return HTCLIENT;
			}
			const int extent = MulDiv(2, WindowDpi(hwnd), 96);
			const bool left = x >= topLeft.x && x < topLeft.x + extent;
			const bool right = x < bottomRight.x && x >= bottomRight.x - extent;
			const bool bottom = y < bottomRight.y && y >= bottomRight.y - extent;
			if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
			if (left) return HTLEFT;
			if (right) return HTRIGHT;
		}
	}
	return result;
}

// The window rect that produces the wanted client rect.
static RECT ClientToWindowRect(HWND hwnd, const GdxsvMultiPovRect& client) {
	RECT r{client.x, client.y, client.x + client.w, client.y + client.h};
	const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
	const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
	AdjustWindowRectEx(&r, style, FALSE, ex_style);
	if (hwnd == gdxsv_window.flatHost) {
		r.left = client.x;
		r.right = client.x + client.w;
		r.bottom = client.y + client.h;
	}
	return r;
}

}  // namespace

bool gdxsv_multi_pov_window_available() { return Hwnd() != nullptr; }

GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state() {
	GdxsvMultiPovWindowState state;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return state;
	state.frame = gdxsv_multi_pov_window_get_frame();
	if ((SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0)
		state.mode = GdxsvMultiPovWindowMode::Fullscreen;
	else if (IsZoomed(hwnd))
		state.mode = GdxsvMultiPovWindowMode::Maximized;
	return state;
}

void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState& state) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	if (gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_unmaximize();
	gdxsv_multi_pov_window_set_frame(state.frame);
	if (state.mode == GdxsvMultiPovWindowMode::Maximized)
		ShowWindow(hwnd, SW_MAXIMIZE);
	else if (state.mode == GdxsvMultiPovWindowMode::Fullscreen &&
		SDL_SetWindowFullscreen(sdl_get_window(), SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		WARN_LOG(COMMON, "multi-pov: cannot restore full screen: %s", SDL_GetError());
}

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
	return hwnd != nullptr && (IsZoomed(hwnd) ||
		(SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0);
}

void gdxsv_multi_pov_window_unmaximize() {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	if (SDL_SetWindowFullscreen(sdl_get_window(), 0) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot leave full screen: %s", SDL_GetError());
		return;
	}
	// SW_RESTORE keeps the window on the display it was maximized on.
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

	out.x = info.rcWork.left;
	out.y = info.rcWork.top;
	out.w = info.rcWork.right - info.rcWork.left;
	out.h = info.rcWork.bottom - info.rcWork.top;
	return out;
}

GdxsvMultiPovRect gdxsv_multi_pov_window_display_area() {
	GdxsvMultiPovRect out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfo(monitor, &info)) return out;

	out.x = info.rcMonitor.left;
	out.y = info.rcMonitor.top;
	out.w = info.rcMonitor.right - info.rcMonitor.left;
	out.h = info.rcMonitor.bottom - info.rcMonitor.top;
	return out;
}

void gdxsv_multi_pov_window_set_topmost(bool topmost) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	// AdjustWindowRectEx on an empty rect yields the frame alone.
	RECT r{0, 0, 0, 0};
	const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
	const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
	if (!AdjustWindowRectEx(&r, style, FALSE, ex_style)) return out;
	out.left = -r.left;
	out.top = -r.top;
	out.right = r.right;
	out.bottom = r.bottom;
	if (hwnd == gdxsv_window.flatHost) out.left = out.right = out.bottom = 0;
	return out;
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;

	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	const LONG_PTR decorations = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
	const LONG_PTR wanted = borderless ? (style & ~decorations) : (style | decorations);
	if (wanted == style) return;

	SetWindowLongPtrW(hwnd, GWL_STYLE, wanted);
	// The frame only changes once the window is told to recalculate it.
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// dwmapi is loaded at run time: the attributes below are Windows 11 only,
// and an older DWM just rejects them.
static void SetDwmAttribute(HWND hwnd, DWORD attribute, const void* value, DWORD size) {
	using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
	static const DwmSetWindowAttributeFn set_attribute = []() -> DwmSetWindowAttributeFn {
		HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
		if (dwmapi == nullptr) return nullptr;
		return reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
	}();
	if (set_attribute != nullptr) set_attribute(hwnd, attribute, value, size);
}

void gdxsv_multi_pov_window_set_flat(bool flat) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	if ((flat && gdxsv_window.flatHost == nullptr && gdxsv_multi_pov_current_role() == GdxsvMultiPovRole::Host) ||
		(!flat && gdxsv_window.flatHost == hwnd)) {
		// Capture the native restored client before extending its frame. The
		// grid already saved the maximized state and restores it at session end.
		// Restoring after subclass installation would include the old resize
		// borders in the client and grow the saved normal size each session.
		if (flat && gdxsv_multi_pov_window_is_maximized()) {
			gdxsv_multi_pov_window_unmaximize();
			if (gdxsv_multi_pov_window_is_maximized()) return;
		}
		const GdxsvMultiPovRect client = gdxsv_multi_pov_window_get_frame();
		if (flat) {
			if (!SetWindowSubclass(hwnd, FlatHostWindowProc, kGdxsvFlatHostSubclass, 0)) {
				WARN_LOG(COMMON, "multi-pov: cannot flatten host frame (%lu)", GetLastError());
			} else
				gdxsv_window.flatHost = hwnd;
		} else {
			if (!RemoveWindowSubclass(hwnd, FlatHostWindowProc, kGdxsvFlatHostSubclass)) {
				WARN_LOG(COMMON, "multi-pov: cannot restore host frame (%lu)", GetLastError());
				return;
			}
			gdxsv_window.flatHost = nullptr;
		}
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		// Keep the game area's coordinates unchanged when the frame changes.
		if (!gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_set_frame(client);
	}

	// Values from dwmapi.h, which older SDKs lack. The shadow cannot go
	// without changing how the title bar is drawn; see
	// gdxsv_multi_pov_window_stay_below for that.
	constexpr DWORD kCornerPreference = 33;		 // DWMWA_WINDOW_CORNER_PREFERENCE
	constexpr DWORD kBorderColor = 34;			 // DWMWA_BORDER_COLOR
	const DWORD corner = flat ? 1 : 0;			 // DWMWCP_DONOTROUND : DWMWCP_DEFAULT
	const DWORD border = flat ? 0xFFFFFFFE : 0xFFFFFFFF;  // DWMWA_COLOR_NONE : DWMWA_COLOR_DEFAULT

	SetDwmAttribute(hwnd, kCornerPreference, &corner, sizeof(corner));
	SetDwmAttribute(hwnd, kBorderColor, &border, sizeof(border));
}

int64_t gdxsv_multi_pov_window_native_handle() {
	return static_cast<int64_t>(reinterpret_cast<intptr_t>(Hwnd()));
}

void gdxsv_multi_pov_window_stay_below(const int64_t* windows, int count) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	// A guest stops pumping messages while it waits for the host to catch up
	// (WaitForMultiPovHost), so the requests are posted, never waited on, and
	// not repeated every frame while one may still be queued.
	static DWORD last_request = 0;
	const DWORD now = GetTickCount();
	if (now - last_request < 250) return;
	for (int i = 0; i < count; ++i) {
		HWND guest = reinterpret_cast<HWND>(static_cast<intptr_t>(windows[i]));
		if (guest == nullptr || !IsWindow(guest)) continue;
		bool above = false;
		for (HWND w = GetWindow(hwnd, GW_HWNDPREV); w != nullptr; w = GetWindow(w, GW_HWNDPREV)) {
			if (w == guest) {
				above = true;
				break;
			}
		}
		if (above) continue;
		// Directly above the host: anything else keeps its place.
		HWND over_host = GetWindow(hwnd, GW_HWNDPREV);
		SetWindowPos(guest, over_host != nullptr ? over_host : HWND_TOP, 0, 0, 0, 0,
					 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
		last_request = now;
	}
}
