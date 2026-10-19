// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// UI/render thread only. Ordinary rendering pays only the flag check.
extern bool gdxsv_video_grid_active;
void gdxsv_video_viewport(int& x, int& y, int& width, int& height, bool bottom_origin = false);
void gdxsv_video_term();

// A background replay worker still renders, but never shows its SDL window.
bool gdxsv_video_hidden_window();
// Optional stable name for video routing; empty keeps the ordinary sender name.
std::string gdxsv_video_sender_name();
