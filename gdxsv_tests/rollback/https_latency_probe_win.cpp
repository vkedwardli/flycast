// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "gdxsv/gdxsv_https_latency.h"
#include "log/Log.h"

GdxsvHttpsLatencyResult gdxsv_measure_https_latency_baseline(const std::string&, const std::string&, int);

namespace {

struct GdxsvProbeRegion {
	const char* id;
	const char* location;
};

#include "latency_probe_regions.h"

struct GdxsvProbeOutput {
	std::mutex mutex;
};

GdxsvProbeOutput& gdxsv_probe_output() {
	static GdxsvProbeOutput value;
	return value;
}

struct GdxsvProbeVariant {
	const char* name;
	const char* path;
	bool baseline;
	int expected_status;
};

constexpr std::array<GdxsvProbeVariant, 4> variants{{
	{"old_root", "/", true, 400},
	{"new_root", "/", false, 400},
	{"old_api", "/storage/v1", true, 404},
	{"new_api", "/storage/v1", false, 404},
}};

struct GdxsvProbeResults {
	std::array<std::array<GdxsvHttpsLatencyResult, 4>, 2> rounds;
};

bool gdxsv_probe_valid(const GdxsvHttpsLatencyResult& result, size_t variant) {
	return result.ok && result.status == variants[variant].expected_status && result.attempts_ms.size() == 3 && result.min_ms >= 0 &&
		   result.min_ms == *std::min_element(result.attempts_ms.begin(), result.attempts_ms.end());
}

bool gdxsv_probe_valid(const GdxsvProbeResults& results) {
	for (const auto& round : results.rounds)
		for (size_t variant = 0; variant < variants.size(); ++variant)
			if (!gdxsv_probe_valid(round[variant], variant)) return false;
	return true;
}

struct GdxsvProbeChild {
	HANDLE log = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process{};
	std::wstring path;
	std::vector<unsigned char> attributes;
	LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;

	~GdxsvProbeChild() {
		if (process.hThread) CloseHandle(process.hThread);
		if (process.hProcess) CloseHandle(process.hProcess);
		if (attribute_list) DeleteProcThreadAttributeList(attribute_list);
		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
		if (!path.empty()) DeleteFileW(path.c_str());
	}
};

GdxsvHttpsLatencyResult gdxsv_probe_sample(const GdxsvProbeRegion& region, size_t variant) {
	GdxsvHttpsLatencyResult result;
	GdxsvProbeChild child;
	wchar_t directory[MAX_PATH + 1]{};
	wchar_t temporary[MAX_PATH + 1]{};
	const DWORD directory_length = GetTempPathW(MAX_PATH + 1, directory);
	if (!directory_length || directory_length > MAX_PATH || !GetTempFileNameW(directory, L"gdx", 0, temporary))
		throw std::runtime_error("cannot create child log path");
	child.path = temporary;
	SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
	child.log =
		CreateFileW(temporary, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (child.log == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open child log");

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = child.log;
	startup.StartupInfo.hStdOutput = child.log;
	startup.StartupInfo.hStdError = child.log;
	SIZE_T attribute_bytes = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
	child.attributes.resize(attribute_bytes);
	auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(child.attributes.data());
	if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
		throw std::runtime_error("cannot initialize child handle list");
	child.attribute_list = attributes;
	if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &child.log, sizeof(child.log), nullptr, nullptr))
		throw std::runtime_error("cannot set child handle list");
	startup.lpAttributeList = attributes;

	std::vector<wchar_t> executable(32768);
	const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
	if (!length || length >= executable.size()) throw std::runtime_error("cannot locate probe executable");
	const std::string arguments = std::string(" --sample ") + region.id + " " + variants[variant].name;
	std::wstring command = L"\"" + std::wstring(executable.data()) + L"\"" + std::wstring(arguments.begin(), arguments.end());
	if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
						nullptr, &startup.StartupInfo, &child.process))
		throw std::runtime_error("cannot start probe child: " + std::to_string(GetLastError()));
	const DWORD wait = WaitForSingleObject(child.process.hProcess, 120000);
	if (wait != WAIT_OBJECT_0) {
		TerminateProcess(child.process.hProcess, 1);
		WaitForSingleObject(child.process.hProcess, 5000);
		result.error = "probe child exceeded its 120-second deadline or wait failed";
		return result;
	}
	DWORD exit_code = 1;
	GetExitCodeProcess(child.process.hProcess, &exit_code);
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(child.log, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
		throw std::runtime_error("invalid child log size");
	LARGE_INTEGER beginning{};
	if (!SetFilePointerEx(child.log, beginning, nullptr, FILE_BEGIN)) throw std::runtime_error("cannot rewind child log");
	std::string output(static_cast<size_t>(size.QuadPart), '\0');
	DWORD bytes = 0;
	if (!ReadFile(child.log, output.data(), static_cast<DWORD>(output.size()), &bytes, nullptr) || bytes != output.size())
		throw std::runtime_error("cannot read child log");
	{
		std::lock_guard<std::mutex> lock(gdxsv_probe_output().mutex);
		std::fwrite(output.data(), 1, output.size(), stdout);
		std::fflush(stdout);
	}
	const auto marker = output.rfind("CHILD_RESULT ");
	if (marker == std::string::npos) {
		result.error = "child produced no result";
		return result;
	}
	std::istringstream fields(output.substr(marker + std::string("CHILD_RESULT ").size()));
	int ok = 0;
	size_t count = 0;
	if (!(fields >> ok >> result.status >> result.min_ms >> count) || count > 3) {
		result.error = "malformed child result";
		return result;
	}
	for (size_t i = 0; i < count; ++i) {
		int sample;
		if (!(fields >> sample) || sample < 0) {
			result.error = "malformed child sample";
			return result;
		}
		result.attempts_ms.push_back(sample);
	}
	result.ok = exit_code == 0 && ok == 1;
	if (!result.ok) result.error = "child failed; see CHILD_ERROR in log";
	return result;
}

int gdxsv_probe_child(const char* region_id, const char* variant_name) {
	const auto region = std::find_if(std::begin(gdxsv_probe_regions), std::end(gdxsv_probe_regions),
									 [&](const auto& value) { return std::string(value.id) == region_id; });
	const auto variant =
		std::find_if(variants.begin(), variants.end(), [&](const auto& value) { return std::string(value.name) == variant_name; });
	if (region == std::end(gdxsv_probe_regions) || variant == variants.end()) throw std::runtime_error("unknown child sample");
	const std::string host = std::string("storage.") + region->id + ".rep.googleapis.com";
	const auto result =
		variant->baseline ? gdxsv_measure_https_latency_baseline(host, variant->path, 3) : measureGdxsvHttpsLatency(host, variant->path, 3);
	const bool valid = gdxsv_probe_valid(result, static_cast<size_t>(variant - variants.begin()));
	if (!valid) std::printf("CHILD_ERROR region=%s variant=%s error=%s\n", region_id, variant_name, result.error.c_str());
	std::printf("CHILD_RESULT %d %d %d %zu", valid, result.status, result.min_ms, result.attempts_ms.size());
	for (int sample : result.attempts_ms) std::printf(" %d", sample);
	std::printf("\n");
	return valid ? 0 : 1;
}

GdxsvProbeResults gdxsv_probe_region(const GdxsvProbeRegion& region, const char* phase) {
	GdxsvProbeResults results;
	const std::string host = std::string("storage.") + region.id + ".rep.googleapis.com";
	for (size_t round = 0; round < results.rounds.size(); ++round) {
		for (size_t step = 0; step < variants.size(); ++step) {
			const size_t variant = round == 0 ? step : variants.size() - 1 - step;
			const auto& test = variants[variant];
			{
				std::lock_guard<std::mutex> lock(gdxsv_probe_output().mutex);
				std::printf("BEGIN phase=%s region=%s round=%zu variant=%s host=%s path=%s\n", phase, region.id, round + 1, test.name,
							host.c_str(), test.path);
				std::fflush(stdout);
			}
			auto& result = results.rounds[round][variant];
			result = gdxsv_probe_sample(region, variant);
			std::lock_guard<std::mutex> lock(gdxsv_probe_output().mutex);
			std::printf("RESULT phase=%s region=%s round=%zu variant=%s ok=%d status=%d min_ms=%d samples_ms=", phase, region.id, round + 1,
						test.name, gdxsv_probe_valid(result, variant), result.status, result.min_ms);
			for (size_t i = 0; i < result.attempts_ms.size(); ++i) std::printf("%s%d", i ? "," : "", result.attempts_ms[i]);
			std::printf(" error=%s\n", result.error.c_str());
			std::fflush(stdout);
		}
	}
	return results;
}

std::string gdxsv_probe_cell(const GdxsvProbeResults& results, size_t variant) {
	if (!gdxsv_probe_valid(results.rounds[0][variant], variant) || !gdxsv_probe_valid(results.rounds[1][variant], variant)) return "FAILED";
	const int a = results.rounds[0][variant].min_ms;
	const int b = results.rounds[1][variant].min_ms;
	if (a == b) return std::to_string(a);
	return std::to_string(std::min(a, b)) + "-" + std::to_string(std::max(a, b));
}

}  // namespace

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char*, int, const char* format, ...) {
	std::lock_guard<std::mutex> lock(gdxsv_probe_output().mutex);
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

int main(int argc, char** argv) {
	try {
		SetConsoleOutputCP(CP_UTF8);
		if (argc == 4 && std::string(argv[1]) == "--sample") return gdxsv_probe_child(argv[2], argv[3]);
		std::vector<GdxsvProbeRegion> regions(std::begin(gdxsv_probe_regions), std::end(gdxsv_probe_regions));
		if (argc == 3 && std::string(argv[1]) == "--region") {
			regions.erase(std::remove_if(regions.begin(), regions.end(), [&](const auto& r) { return std::string(r.id) != argv[2]; }),
						  regions.end());
			if (regions.empty()) throw std::runtime_error("unknown region");
		} else if (argc != 1) {
			throw std::runtime_error("usage: windows-https-latency.exe [--region REGION]");
		}
		std::printf("Windows HTTPS latency comparison: commit=%s baseline=%s\n", gdxsv_probe_commit, gdxsv_probe_baseline);
		std::printf(
			"Four variants; two rounds in opposite orders; one warmup and three measured attempts per variant; six region workers.\n");
		std::printf("New timing ends when HttpSendRequest returns and allows HTTP/2; the baseline keeps its protocol defaults.\n");
		std::printf("Each sample group runs in a fresh process to isolate WinINet connection pools. No game or credentials required.\n");
		std::fflush(stdout);
		std::vector<GdxsvProbeResults> results(regions.size());
		std::atomic<size_t> next{0};
		std::vector<std::future<void>> workers;
		for (size_t i = 0; i < std::min<size_t>(6, regions.size()); ++i)
			workers.push_back(std::async(std::launch::async, [&] {
				for (;;) {
					const size_t index = next.fetch_add(1);
					if (index >= regions.size()) return;
					results[index] = gdxsv_probe_region(regions[index], "parallel");
				}
			}));
		for (auto& worker : workers) worker.get();
		size_t retried = 0;
		for (size_t i = 0; i < regions.size(); ++i) {
			if (!gdxsv_probe_valid(results[i])) {
				++retried;
				results[i] = gdxsv_probe_region(regions[i], "serial_retry");
			}
		}
		std::printf("\nSUMMARY commit=%s baseline=%s serial_retries=%zu\n", gdxsv_probe_commit, gdxsv_probe_baseline, retried);
		std::printf(
			"New: WinINet with HTTP/2 enabled, timed through HttpSendRequest. Old: unmodified baseline helper. Each group has its own "
			"process.\n");
		std::printf("Milliseconds: range of the two per-round minima. FAILED means incomplete measurements, not a timing regression.\n\n");
		std::printf("| Region | Location | Old / (400) | New / (400) | Old /storage/v1 (404) | New /storage/v1 (404) |\n");
		std::printf("|---|---|---:|---:|---:|---:|\n");
		bool ok = true;
		for (size_t i = 0; i < regions.size(); ++i) {
			std::printf("| %s | %s | %s | %s | %s | %s |\n", regions[i].id, regions[i].location, gdxsv_probe_cell(results[i], 0).c_str(),
						gdxsv_probe_cell(results[i], 1).c_str(), gdxsv_probe_cell(results[i], 2).c_str(),
						gdxsv_probe_cell(results[i], 3).c_str());
			ok = ok && gdxsv_probe_valid(results[i]);
		}
		std::printf(
			"\n%s: %zu regions x four variants x two rounds. Successful collection does not assert that the new timing removes the "
			"delay.\n",
			ok ? "PASS" : "FAIL", regions.size());
		return ok ? 0 : 1;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
