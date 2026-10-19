// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_viewer.h"

#include <algorithm>
#include <mutex>

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_multi_pov.h"
#include "gdxsv_video_hooks.h"
#include "log/LogManager.h"
#include "types.h"
#ifdef USE_SDL
#include "sdl/sdl.h"
#endif

bool gdxsv_video_grid_active = false;

namespace {
struct GdxsvViewerState {
	std::mutex mutex;
	std::string requested;
	// The remainder is used only on the UI thread.
	std::string session;
	std::string names[kGdxsvMultiPovScreens];
	bool connected[kGdxsvMultiPovScreens]{};
	GdxsvMultiPovRect group;
	uint32_t generation = 0;
};
GdxsvViewerState viewer;

std::string SenderName(const std::string& session, int screen) { return "Flycast " + session + " " + std::to_string(screen + 1) + "P"; }

}  // namespace

const char* gdxsv_multi_pov_viewer_unavailable() {
#if defined(VIDEO_ROUTING) && defined(USE_SDL) && !defined(LIBRETRO)
	if (gdxsv_headless()) return "Four-screen playback needs a display";
#if defined(TARGET_MAC)
	if (isOpenGL(config::RendererType) || isVulkan(config::RendererType)) return nullptr;
#elif defined(_WIN32)
	if (isOpenGL(config::RendererType) || config::RendererType == RenderType::DirectX11 ||
		config::RendererType == RenderType::DirectX11_OIT)
		return nullptr;
	return "Select OpenGL or DirectX 11 for four-screen playback";
#endif
	return "Select OpenGL, DirectX 11 or Vulkan for four-screen playback";
#else
	return "Four-screen video sharing is unavailable on this platform";
#endif
}

bool gdxsv_multi_pov_viewer_begin(const std::string& session) {
	if (gdxsv_multi_pov_viewer_unavailable() != nullptr) return false;
	std::lock_guard<std::mutex> lock(viewer.mutex);
	viewer.requested = session;
	return true;
}

void gdxsv_multi_pov_viewer_end() {
	std::lock_guard<std::mutex> lock(viewer.mutex);
	viewer.requested.clear();
}

bool gdxsv_video_hidden_window() {
	// Saved options cannot turn an ordinary launch into an invisible worker.
#if defined(VIDEO_ROUTING) && defined(USE_SDL) && (defined(TARGET_MAC) || defined(_WIN32)) && !defined(LIBRETRO)
	return config::isTransient("gdxsv", "MultiPovSession") && !config::loadStr("gdxsv", "MultiPovSession", "").empty() &&
		   config::isTransient("gdxsv", "MultiPovScreen") && gdxsv_is_multi_pov_guest();
#else
	return false;
#endif
}

std::string gdxsv_video_sender_name() {
	std::string name = config::loadStr("gdxsv", "VideoRoutingName", "");
	// Spout's sender-name buffer also includes a terminating zero.
	if (name.size() > 255) {
		WARN_LOG(RENDERER, "VideoRoutingName exceeds 255 bytes; using the default sender name");
		return {};
	}
	return name;
}

void gdxsv_video_term() {
	gdxsv_multi_pov_viewer_term();
	std::fill(std::begin(viewer.connected), std::end(viewer.connected), false);
}

void gdxsv_multi_pov_viewer_tick() {
	std::string requested;
	{
		std::lock_guard<std::mutex> lock(viewer.mutex);
		requested = viewer.requested;
	}
	if (viewer.session != requested) {
		gdxsv_video_term();
		// VMU overlays span the whole window instead of the host's quadrant.
		if (!requested.empty())
			config::FloatVMUs.override(false);
		else if (!viewer.session.empty()) {
			config::FloatVMUs.reset();
			config::FloatVMUs.load();
		}
		viewer.session = requested;
		viewer.group = {};
		for (int screen = 1; screen < kGdxsvMultiPovScreens; ++screen) viewer.names[screen] = SenderName(requested, screen);
	}
	gdxsv_video_grid_active = !viewer.session.empty();
#ifdef USE_SDL
	SDL_Window* window = sdl_get_window();
	if (window == nullptr) return;
	if (gdxsv_video_grid_active) {
		GdxsvMultiPovHostWindow host;
		SDL_GetWindowPosition(window, &host.group.x, &host.group.y);
		SDL_GetWindowSize(window, &host.group.w, &host.group.h);
		host.rect = host.group;
		host.rect.w = std::max(1, host.group.w / 2);
		host.rect.h = std::max(1, host.group.h / 2);
		if (viewer.group != host.group) {
			viewer.group = host.group;
			++viewer.generation;
		}
		host.generation = viewer.generation;
		gdxsv_multi_pov_publish_host_window(host);
	} else if (gdxsv_video_hidden_window()) {
		// Only the host's presentation counter belongs in the combined window.
		config::ShowFPS.override(false);
		config::FloatVMUs.override(false);
		if (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) SDL_HideWindow(window);
		GdxsvMultiPovHostWindow host;
		if (gdxsv_multi_pov_read_host_window(host) && host.rect.w > 0 && host.rect.h > 0 && host.rect.w <= 8192 && host.rect.h <= 8192 &&
			viewer.group != host.group) {
			// Keep the hidden render target's aspect and display scale in step
			// with the host, including moves to a different monitor.
			SDL_SetWindowPosition(window, host.rect.x, host.rect.y);
			SDL_SetWindowSize(window, host.rect.w, host.rect.h);
			viewer.group = host.group;
		}
	}
#endif
}

void gdxsv_video_viewport(int& x, int& y, int& width, int& height, bool bottom_origin) {
	const int right = (x + width) / 2;
	const int bottom = (y + height) / 2;
	x /= 2;
	y /= 2;
	width = right - x;
	height = bottom - y;
	if (bottom_origin) y += settings.display.height - settings.display.height / 2;
}

void gdxsv_multi_pov_viewer_draw() {
	if (!gdxsv_video_grid_active) return;
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	for (int screen = 1; screen < kGdxsvMultiPovScreens; ++screen) {
		const ImVec2 start((screen % 2) * size.x / 2, (screen / 2) * size.y / 2);
		const ImVec2 end(start.x + size.x / 2, start.y + size.y / 2);
		draw->AddRectFilled(start, end, IM_COL32_BLACK);
		const auto frame = gdxsv_multi_pov_viewer_receive(screen, viewer.names[screen]);
		if (frame.texture != 0 && frame.width > 0 && frame.height > 0) {
			const float scale = std::min((end.x - start.x) / frame.width, (end.y - start.y) / frame.height);
			const ImVec2 p(start.x + (end.x - start.x - frame.width * scale) / 2, start.y + (end.y - start.y - frame.height * scale) / 2);
			if (frame.before_draw) draw->AddCallback(frame.before_draw, nullptr);
			draw->AddImage(frame.texture, p, ImVec2(p.x + frame.width * scale, p.y + frame.height * scale),
						   ImVec2(0, frame.flipped ? 1 : 0), ImVec2(1, frame.flipped ? 0 : 1));
			if (frame.before_draw) draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
			if (!viewer.connected[screen]) NOTICE_LOG(RENDERER, "multi-pov viewer: receiving %s", viewer.names[screen].c_str());
			viewer.connected[screen] = true;
		} else {
			viewer.connected[screen] = false;
			const std::string label = std::to_string(screen + 1) + "P - waiting for video";
			draw->AddText(ImVec2(start.x + 12, start.y + 12), IM_COL32_WHITE, label.c_str());
		}
	}
}

#if !defined(VIDEO_ROUTING) || (!defined(TARGET_MAC) && !defined(_WIN32)) || defined(LIBRETRO)
GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_receive(int, const std::string&) { return {}; }
void gdxsv_multi_pov_viewer_term() {}
#endif
