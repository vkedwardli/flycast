// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>

#include "imgui.h"

// Session requests can come from the emulator thread. Graphics resources and
// window changes are applied by Tick/Draw on the UI thread.
inline constexpr const char* kGdxsvMultiPovRendererUnavailable = "Select OpenGL, DirectX 11 or Vulkan for four-screen playback";
inline constexpr const char* kGdxsvMultiPovWaitingForVideo = "Waiting for guest video";
// Cheap renderer check. Device import support is tested before loading a replay.
const char* gdxsv_multi_pov_viewer_unavailable();
// Empty on success; GPU checks run on the UI thread, with a bounded loader wait.
std::string gdxsv_multi_pov_viewer_prepare();
void gdxsv_multi_pov_viewer_begin(const std::string& session);
void gdxsv_multi_pov_viewer_end();
// Returns a reason to end an active session when video sharing cannot recover.
const char* gdxsv_multi_pov_viewer_tick();
void gdxsv_multi_pov_viewer_draw();


struct GdxsvMultiPovVideoFrame {
	ImTextureID texture = 0;
	int width = 0;
	int height = 0;
	bool flipped = false;
	// Backends without an RGB texture view can disable blending for this image.
	// TODO: Remove this field and its draw dispatch together with DrawOpaqueDX.
	ImDrawCallback before_draw = nullptr;
	// Receiver-owned message, valid until its next receive or teardown.
	const char* error = nullptr;
};

// Platform receivers own their GPU resources until term, which runs before
// the graphics context is destroyed. Screen is 1..3 (2P..4P).
GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_receive(int screen, const std::string& name);
// Session changes keep a tested graphics device; graphics teardown releases it.
void gdxsv_multi_pov_viewer_term(bool reset_device = true);

#if defined(TARGET_MAC) && defined(USE_VULKAN)
// Keep the C++ renderer headers separate from AppKit's conflicting type names.
void* gdxsv_multi_pov_viewer_metal_device();
GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_metal_receive(int screen, const std::shared_ptr<void>& texture, int width, int height);
void gdxsv_multi_pov_viewer_metal_release(int screen);
void gdxsv_multi_pov_viewer_metal_term();
#endif

#if defined(_WIN32) && defined(VIDEO_ROUTING)
// These platform checks run only on the UI thread.
// The capability check does not create a receiving device.
const char* gdxsv_multi_pov_viewer_win_unavailable();
bool gdxsv_multi_pov_viewer_win_needs_probe();
// During playback, pending device-reset retries keep the grid and return no failure.
const char* gdxsv_multi_pov_viewer_win_prepare(bool starting = false);
#ifdef USE_VULKAN
const char* gdxsv_multi_pov_viewer_vulkan_unavailable();
const char* gdxsv_multi_pov_viewer_vulkan_prepare(bool starting);
GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_vulkan_receive(int screen, const std::string& name);
void gdxsv_multi_pov_viewer_vulkan_term(bool reset_device);
#endif
#endif
