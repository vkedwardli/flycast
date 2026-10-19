// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>

#include "imgui.h"

// Session requests can come from the emulator thread. Graphics resources and
// window changes are applied by Tick/Draw on the UI thread.
// Null when the current renderer can display all four views.
const char* gdxsv_multi_pov_viewer_unavailable();
bool gdxsv_multi_pov_viewer_begin(const std::string& session);
void gdxsv_multi_pov_viewer_end();
void gdxsv_multi_pov_viewer_tick();
void gdxsv_multi_pov_viewer_draw();


struct GdxsvMultiPovVideoFrame {
	ImTextureID texture = 0;
	int width = 0;
	int height = 0;
	bool flipped = false;
	// Backends without an RGB texture view can disable blending for this image.
	// TODO: Remove this field and its draw dispatch together with DrawOpaqueDX.
	ImDrawCallback before_draw = nullptr;
};

// Platform receivers own their GPU resources until term, which runs before
// the graphics context is destroyed. Screen is 1..3 (2P..4P).
GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_receive(int screen, const std::string& name);
void gdxsv_multi_pov_viewer_term();

#if defined(TARGET_MAC) && defined(USE_VULKAN)
// Keep the C++ renderer headers separate from AppKit's conflicting type names.
void* gdxsv_multi_pov_viewer_metal_device();
GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_metal_receive(int screen, const std::shared_ptr<void>& texture, int width, int height);
void gdxsv_multi_pov_viewer_metal_release(int screen);
void gdxsv_multi_pov_viewer_metal_term();
#endif
