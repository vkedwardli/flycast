#pragma once
#include <cstdint>

void gdxsv_set_timer_resolution();
void gdxsv_reset_timer_resolution();
void gdxsv_sleep_us(int64_t us);
int64_t gdxsv_sleep_and_busy_wait(int64_t us);

// Sleep benchmark for measuring timing accuracy
// Enable with CMake option: -DENABLE_SLEEP_BENCHMARK=ON
void gdxsv_sleep_benchmark_periodic_report();
