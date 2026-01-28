#pragma once
#include <cstdint>

void set_timer_resolution();
void reset_timer_resolution();
void sleep_us(int64_t us);
int64_t sleep_and_busy_wait(int64_t us);

// Sleep benchmark for measuring timing accuracy
// Enable with #define SLEEP_BENCHMARK 1 in sleep.cpp

struct SleepBenchmarkStats {
	int64_t frameCount;           // Number of frames measured
	int64_t totalRequested;       // Total requested wait time (us)
	int64_t totalSlept;           // Total time spent in OS sleep (us)
	int64_t totalBusyWait;        // Total time spent in busy loop (us)
	int64_t totalOvershoot;       // Total overshoot time (us)
	int64_t maxOvershoot;         // Maximum single overshoot (us)
	int64_t oversleepCount;       // Number of times sleep overshot by >1ms
	int64_t frameDropCount;       // Number of frames where remaining <= 0
};

void sleep_benchmark_reset();
void sleep_benchmark_report();
const SleepBenchmarkStats& sleep_benchmark_get_stats();

