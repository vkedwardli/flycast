// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// UI/render thread only. Ordinary rendering pays only the flag check.
extern bool gdxsv_video_grid_active;
void gdxsv_video_viewport(int& x, int& y, int& width, int& height, bool bottom_origin = false);
void gdxsv_video_term();
// DXGI frame latency for the DX11 device (gdxsv:MaxFrameLatency); 0 keeps the driver default.
int gdxsv_video_max_frame_latency();
// The FixedFrequency limiter paces the game, so vsync swaps every vblank: no swap interval or
// duplicated presents from the refresh rate read at startup, which can be another display's.
bool gdxsv_video_swap_every_vblank();

// A background replay worker still renders, but never shows its SDL window.
bool gdxsv_video_hidden_window();
// Optional stable name for video routing; empty keeps the ordinary sender name.
std::string gdxsv_video_sender_name();

#if defined(_WIN32) && defined(VIDEO_ROUTING) && defined(USE_VULKAN) && !defined(LIBRETRO)
#include <cstdint>
#include <set>
#include <vector>
// Windows Vulkan hosts import textures received from DirectX guests.
void gdxsv_video_vulkan_extensions(uint32_t device_version, const std::set<std::string>& supported, std::vector<const char*>& enabled);
#endif
