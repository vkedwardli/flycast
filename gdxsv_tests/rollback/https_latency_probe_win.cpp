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
			result =
				test.baseline ? gdxsv_measure_https_latency_baseline(host, test.path, 3) : measureGdxsvHttpsLatency(host, test.path, 3);
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
		std::printf("No game or credentials required.\n");
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
		std::printf("New: WinINet with HTTP/2 enabled, timed through HttpSendRequest. Old: unmodified baseline helper.\n");
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
