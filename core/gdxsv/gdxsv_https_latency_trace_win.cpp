// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_https_latency_trace_win.h"

#include <cstring>

#include "log/Log.h"

namespace {

const char* gdxsv_latency_event_name(DWORD status) {
	switch (status) {
		case INTERNET_STATUS_RESOLVING_NAME:
			return "resolving_name";
		case INTERNET_STATUS_NAME_RESOLVED:
			return "name_resolved";
		case INTERNET_STATUS_CONNECTING_TO_SERVER:
			return "connecting";
		case INTERNET_STATUS_CONNECTED_TO_SERVER:
			return "connected";
		case INTERNET_STATUS_SENDING_REQUEST:
			return "sending_request";
		case INTERNET_STATUS_REQUEST_SENT:
			return "request_sent";
		case INTERNET_STATUS_RECEIVING_RESPONSE:
			return "receiving_response";
		case INTERNET_STATUS_RESPONSE_RECEIVED:
			return "response_received";
		case INTERNET_STATUS_REDIRECT:
			return "redirect";
		case INTERNET_STATUS_INTERMEDIATE_RESPONSE:
			return "intermediate_response";
		case INTERNET_STATUS_CLOSING_CONNECTION:
			return "closing_connection";
		case INTERNET_STATUS_CONNECTION_CLOSED:
			return "connection_closed";
		case INTERNET_STATUS_HANDLE_CLOSING:
			return "handle_closing";
		case INTERNET_STATUS_REQUEST_COMPLETE:
			return "request_complete";
		case INTERNET_STATUS_DETECTING_PROXY:
			return "detecting_proxy";
		default:
			return "other";
	}
}

std::string gdxsv_latency_query_header(HINTERNET request, DWORD query) {
	char buffer[512]{};
	DWORD length = sizeof(buffer) - 1;
	if (!HttpQueryInfoA(request, query, buffer, &length, nullptr)) return "unavailable(" + std::to_string(GetLastError()) + ")";
	std::string value(buffer, length);
	for (char& c : value)
		if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127) c = ' ';
	return value;
}

double gdxsv_latency_milliseconds(GdxsvHttpsLatencyTrace::Clock::time_point from, GdxsvHttpsLatencyTrace::Clock::time_point to) {
	return std::chrono::duration<double, std::milli>(to - from).count();
}

}  // namespace

void GdxsvHttpsLatencyTrace::Attach(HINTERNET request, const std::shared_ptr<GdxsvHttpsLatencyTrace>& self) {
	// WinINet may deliver callbacks on another thread; HANDLE_CLOSING is its last callback.
	auto* owner = new std::shared_ptr<GdxsvHttpsLatencyTrace>(self);
	DWORD_PTR context = reinterpret_cast<DWORD_PTR>(owner);
	if (!InternetSetOptionA(request, INTERNET_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
		callback_error = GetLastError();
		delete owner;
		return;
	}
	if (InternetSetStatusCallbackA(request, Callback) == INTERNET_INVALID_STATUS_CALLBACK) {
		callback_error = GetLastError();
		delete owner;
	}
}

void CALLBACK GdxsvHttpsLatencyTrace::Callback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
	if (context == 0) return;
	auto* owner = reinterpret_cast<std::shared_ptr<GdxsvHttpsLatencyTrace>*>(context);
	auto trace = *owner;
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - trace->started).count();
	DWORD bytes = 0;
	if ((status == INTERNET_STATUS_REQUEST_SENT || status == INTERNET_STATUS_RESPONSE_RECEIVED) && information != nullptr &&
		length == sizeof(bytes))
		std::memcpy(&bytes, information, sizeof(bytes));
	{
		std::lock_guard<std::mutex> lock(trace->mutex);
		if (trace->event_count < trace->events.size())
			trace->events[trace->event_count++] = {elapsed, status, bytes};
		else
			trace->dropped_events++;
	}
	if (status == INTERNET_STATUS_HANDLE_CLOSING) delete owner;
}

void GdxsvHttpsLatencyTrace::ReadHeaders(HINTERNET request) {
	DWORD size = sizeof(protocol_flags);
	if (!InternetQueryOptionA(request, INTERNET_OPTION_HTTP_PROTOCOL_USED, &protocol_flags, &size)) protocol_error = GetLastError();
	http_version = gdxsv_latency_query_header(request, HTTP_QUERY_VERSION);
	content_length = gdxsv_latency_query_header(request, HTTP_QUERY_CONTENT_LENGTH);
	connection_header = gdxsv_latency_query_header(request, HTTP_QUERY_CONNECTION);
	server = gdxsv_latency_query_header(request, HTTP_QUERY_SERVER);
}

void GdxsvHttpsLatencyTrace::Log() {
	std::array<Event, 128> snapshot;
	size_t count;
	size_t dropped;
	{
		std::lock_guard<std::mutex> lock(mutex);
		snapshot = events;
		count = event_count;
		dropped = dropped_events;
	}
	NOTICE_LOG(COMMON,
			   "HTTPS_LATENCY host=%s path=%s attempt=%d warmup=%d status=%d headers_ms=%.3f setup_ms=%.3f "
			   "send_ms=%.3f status_query_ms=%.3f diagnostics_ms=%.3f close_ms=%.3f total_ms=%.3f error=%s",
			   host.c_str(), path.c_str(), attempt, attempt == 0, result_status, gdxsv_latency_milliseconds(started, headers_ready),
			   gdxsv_latency_milliseconds(started, send_started), gdxsv_latency_milliseconds(send_started, headers_ready),
			   gdxsv_latency_milliseconds(headers_ready, query_done), gdxsv_latency_milliseconds(query_done, close_started),
			   gdxsv_latency_milliseconds(close_started, closed), gdxsv_latency_milliseconds(started, closed), result_error.c_str());
	NOTICE_LOG(COMMON,
			   "HTTPS_LATENCY host=%s path=%s attempt=%d http_version=%s protocol_flags=%lu protocol_error=%lu "
			   "content_length=%s connection=%s server=%s callback_error=%lu receive_timeout_error=%lu send_timeout_error=%lu "
			   "close_error=%lu events=%zu dropped_events=%zu",
			   host.c_str(), path.c_str(), attempt, http_version.c_str(), static_cast<unsigned long>(protocol_flags),
			   static_cast<unsigned long>(protocol_error), content_length.c_str(), connection_header.c_str(), server.c_str(),
			   static_cast<unsigned long>(callback_error), static_cast<unsigned long>(receive_timeout_error),
			   static_cast<unsigned long>(send_timeout_error), static_cast<unsigned long>(close_error), count, dropped);
	for (size_t i = 0; i < count; ++i)
		NOTICE_LOG(COMMON, "HTTPS_LATENCY host=%s path=%s attempt=%d event=%s code=%lu elapsed_ms=%.3f bytes=%lu", host.c_str(),
				   path.c_str(), attempt, gdxsv_latency_event_name(snapshot[i].status), static_cast<unsigned long>(snapshot[i].status),
				   snapshot[i].elapsed_us / 1000.0, static_cast<unsigned long>(snapshot[i].bytes));
}

GdxsvHttpsLatencyTraceBatch::~GdxsvHttpsLatencyTraceBatch() {
	// Printing between requests would change the pacing of a reused connection.
	for (const auto& request : requests) request->Log();
}
