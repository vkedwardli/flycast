// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_perf_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

bool gdxsv_perf_trace_on = false;

namespace {
struct Row {
	long long qpc;
	unsigned tid;
	int frame;
	const char* event;
	long long arg;
};

struct Trace {
	std::mutex mutex;
	std::vector<Row> rows;
	FILE* file = nullptr;
};

Trace& trace() {
	static Trace t;
	return t;
}

long long now() {
#ifdef _WIN32
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
#else
	return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

long long ticksPerSecond() {
#ifdef _WIN32
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return f.QuadPart;
#else
	return std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num;
#endif
}

unsigned threadId() {
#ifdef _WIN32
	return GetCurrentThreadId();
#else
	return (unsigned)std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

// The rigs kill the process: writing every 100 ms loses only the last rows.
void writerLoop() {
	Trace& t = trace();
	std::vector<Row> batch;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		{
			std::lock_guard<std::mutex> lock(t.mutex);
			batch.swap(t.rows);
		}
		for (const Row& r : batch) fprintf(t.file, "%lld,%u,%d,%s,%lld\n", r.qpc, r.tid, r.frame, r.event, r.arg);
		fflush(t.file);
		batch.clear();
	}
}
}  // namespace

void gdxsv_perf_trace_init() {
	static bool done = false;
	if (done) return;
	done = true;
	const char* path = getenv("GDXSV_PERF_TRACE");
	if (path == nullptr || *path == '\0') return;
	Trace& t = trace();
	t.file = fopen(path, "w");
	if (t.file == nullptr) return;
	t.rows.reserve(1 << 16);
	gdxsv_perf_trace_on = true;
	gdxsv_perf_trace_event("freq", -1, ticksPerSecond());
	std::thread(writerLoop).detach();
}

void gdxsv_perf_trace_event(const char* event, int frame, long long arg) {
	const Row row{now(), threadId(), frame, event, arg};
	Trace& t = trace();
	std::lock_guard<std::mutex> lock(t.mutex);
	t.rows.push_back(row);
}
