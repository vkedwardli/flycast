// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <wininet.h>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct GdxsvHttpsLatencyTrace {
	using Clock = std::chrono::steady_clock;
	struct Event {
		long long elapsed_us;
		DWORD status;
		DWORD bytes;
	};

	Clock::time_point started = Clock::now();
	Clock::time_point send_started;
	Clock::time_point headers_ready;
	Clock::time_point query_done;
	Clock::time_point close_started;
	Clock::time_point closed;
	std::string host;
	std::string path;
	int attempt = 0;
	DWORD callback_error = 0;
	DWORD receive_timeout_error = 0;
	DWORD send_timeout_error = 0;
	DWORD protocol_flags = 0;
	DWORD protocol_error = 0;
	DWORD close_error = 0;
	std::string http_version;
	std::string content_length;
	std::string connection_header;
	std::string server;
	int result_status = 0;
	std::string result_error;

	void Attach(HINTERNET request, const std::shared_ptr<GdxsvHttpsLatencyTrace>& self);
	void ReadHeaders(HINTERNET request);
	void Log();

   private:
	std::mutex mutex;
	std::array<Event, 128> events{};
	size_t event_count = 0;
	size_t dropped_events = 0;
	static void CALLBACK Callback(HINTERNET request, DWORD_PTR context, DWORD status, void* information, DWORD length);
};

struct GdxsvHttpsLatencyTraceBatch {
	std::vector<std::shared_ptr<GdxsvHttpsLatencyTrace>> requests;
	~GdxsvHttpsLatencyTraceBatch();
};
