// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// FixedFrequency frame limiter for the main UI loop (mainui_loop).
#include "gdxsv_frame_limiter.h"

#include <algorithm>
#include <chrono>

#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_net_report.h"
#include "gdxsv_perf_trace.h"
#include "gdxsv_sleep.h"
#include "log/Log.h"
#include "ui/gui.h"

// When the last frame woke.
static std::chrono::time_point<std::chrono::steady_clock> start;
static bool currentDupeFrames;

static int64_t gdxsv_frame_period_us() {
	const auto mode = config::FixedFrequency.get();
	// Native NTSC/VGA
	if (mode == 2 ||
		(mode == 1 && (config::Cable == 0 || config::Cable == 1)) ||
		(mode == 1 && config::Cable == 3 && (config::Broadcast == 0 || config::Broadcast == 4)))
		return 16683; // 1/59.94
	// Approximate VGA
	if (mode == 3) return 16666; // 1/60
	// PAL
	if (mode == 4 || (mode == 1 && config::Cable == 3))
		return 20000; // 1/50
	// Half Native NTSC/VGA
	if (mode == 5) return 33333; // 1/30
	return 16683;
}

// The nominal period, plus whatever trim replay/Live Spectate is asking for.
// The trim is zero outside those modes.
static int64_t gdxsv_trimmed_frame_period_us() {
	const int64_t trim = gdxsv_frame_period_trim_us;
	return std::max<int64_t>(1000, gdxsv_frame_period_us() + trim);
}

void gdxsv_emu_mainui_loop_start() {
	currentDupeFrames = config::DupeFrames;
	gdxsv_set_timer_resolution();
	start = {};
}

void gdxsv_emu_mainui_loop_end() {
	gdxsv_reset_timer_resolution();
}

bool gdxsv_emu_mainui_reinit_needed() {
	if (currentDupeFrames == config::DupeFrames)
		return false;
	currentDupeFrames = config::DupeFrames;
	return true;
}

void gdxsv_frame_limiter_wait() {
	// Without audio sync (LimitFPS) and a fixed rate, vsync used to hold the emulation thread through the render
	// thread; the native rate stands in for it. Headless has no display: it runs unthrottled, as it did.
	const bool on = config::FixedFrequency || (!config::LimitFPS && !gdxsv_headless());
	if (!on || gui_is_open() || settings.input.fastForwardMode) {
		start = {};
		gdxsv_net_report_limiter_off();
		return;
	}

	const auto period = std::chrono::microseconds(gdxsv_trimmed_frame_period_us());
	int64_t overSlept = 0;

	// A period from the last wake: a late frame is not paid back by shorter ones.
	const auto called = std::chrono::steady_clock::now();
	const auto remaining = start == decltype(start){} ? 0 : (period - std::chrono::duration_cast<std::chrono::microseconds>(called - start)).count();
	if (remaining > 0)
		overSlept = gdxsv_sleep_and_busy_wait(remaining);

	const auto now = std::chrono::steady_clock::now();
	start = now;
	// arg: the slack waited before this wake, us; negative: the frame was late
	GDXSV_PERF_TRACE("wake", -1, remaining);
	gdxsv_net_report_wake(called, now, period.count());
	if (1000 <= overSlept)
		WARN_LOG(RENDERER, "FixedFrequency: Over slept %d [us]", (int)overSlept);

	gdxsv_sleep_benchmark_periodic_report();
}
