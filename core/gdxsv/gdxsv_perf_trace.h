// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Event trace for the pre-release performance check (gdxsv_tests/perf).
// Off unless env GDXSV_PERF_TRACE=<csv path> is set; then each event costs one clock read and
// one store, and a helper thread appends the rows to the file every 100 ms.
// Rows: qpc,tid,frame,event,arg. The first row is "freq" with the clock ticks per second in arg.
// On Windows the clock is QueryPerformanceCounter, the clock PresentMon reports in.
// frame is the GGPO frame where the caller knows it, else -1.

extern bool gdxsv_perf_trace_on;

void gdxsv_perf_trace_init();
void gdxsv_perf_trace_event(const char* event, int frame, long long arg);

#define GDXSV_PERF_TRACE(event, frame, arg)                                       \
	do {                                                                          \
		if (gdxsv_perf_trace_on) gdxsv_perf_trace_event((event), (frame), (arg)); \
	} while (0)
