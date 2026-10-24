// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_net_report.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <mutex>
#include <vector>

#include "gdxsv.pb.h"
#include "gdxsv_ggpo_hooks.h"
#include "network/ggpo.h"

std::atomic<bool> gdxsv_net_report_on{false};

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFramePeriodUs = 16683;  // 59.94 Hz
constexpr int kOffPeriodUs = 2000;     // an interval further than this from the frame period is off period
constexpr size_t kMaxSamples = 1 << 17;  // per series, about 36 minutes of frames
constexpr int kDepthBuckets = 8;         // rollbacks of 0 to 6 frames; the last bucket: 7 or more
constexpr size_t kMaxTimesyncFrames = 32;

struct Series {
	Clock::time_point last{};
	std::vector<int32_t> samples;

	void Add(int64_t us) {
		if (samples.size() < kMaxSamples) samples.push_back(static_cast<int32_t>(std::clamp<int64_t>(us, INT32_MIN, INT32_MAX)));
	}
	void Interval(Clock::time_point now) {
		if (last != Clock::time_point{}) Add(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count());
		last = now;
	}
};

struct NetReport {
	Series inputRead;
	Series inputSend;
	Series present;
	Series budget;  // limiter slack per frame
	Clock::time_point pendingRead{};
	int rollbackCount = 0;
	std::array<int32_t, kDepthBuckets> depthHist{};
	Clock::time_point rollbackStart{};
	int rollbackToFrame = -1;  // the frame a running rollback ends at; -1: none runs
	int rollbackReruns = 0;    // frames the running rollback re-ran so far
	int32_t rollbackMaxUs = 0;
	int depthSinceWake = 0;  // deepest rollback since the last limiter wake
	int budgetExhausted = 0;
	std::array<int32_t, kDepthBuckets> exhaustedDepthHist{};
	std::vector<int32_t> timesyncFrames;
};

std::mutex reportMutex;
NetReport report;

int DepthBucket(int depth) { return std::clamp(depth, 0, kDepthBuckets - 1); }

// periodUs 0: off_period is not counted.
void FillStats(std::vector<int32_t> samples, int periodUs, proto::TimingStats* out) {
	if (samples.empty()) return;
	const size_t n = samples.size();
	int64_t sum = 0;
	int off = 0;
	for (int32_t s : samples) {
		sum += s;
		if (periodUs != 0 && std::abs(s - periodUs) > kOffPeriodUs) off++;
	}
	const auto [min, max] = std::minmax_element(samples.begin(), samples.end());
	out->set_count(static_cast<int32_t>(n));
	out->set_mean_us(static_cast<int32_t>(sum / static_cast<int64_t>(n)));
	out->set_min_us(*min);
	out->set_max_us(*max);
	out->set_off_period(off);
	// Nearest rank.
	auto percentile = [&samples, n](double p) {
		const size_t k = std::min(n - 1, static_cast<size_t>(std::max(0.0, std::ceil(p * n) - 1)));
		std::nth_element(samples.begin(), samples.begin() + k, samples.end());
		return samples[k];
	};
	out->set_p1_us(percentile(0.01));
	out->set_p5_us(percentile(0.05));
	out->set_p99_us(percentile(0.99));
}

}  // namespace

void gdxsv_net_report_session_start() {
	std::lock_guard<std::mutex> lock(reportMutex);
	report = NetReport{};
	gdxsv_net_report_on = true;
}

void gdxsv_net_report_session_stop() { gdxsv_net_report_on = false; }

void gdxsv_net_report_reset() {
	std::lock_guard<std::mutex> lock(reportMutex);
	report = NetReport{};
}

void gdxsv_net_report_rollback_begin(int toFrame) {
	std::lock_guard<std::mutex> lock(reportMutex);
	report.rollbackStart = Clock::now();
	report.rollbackToFrame = toFrame;
	report.rollbackReruns = 0;
}

void gdxsv_net_report_rerun_end(int frame) {
	std::lock_guard<std::mutex> lock(reportMutex);
	if (report.rollbackToFrame < 0) return;
	report.rollbackReruns++;
	if (frame + 1 < report.rollbackToFrame) return;
	const int depth = report.rollbackReruns;
	report.rollbackCount++;
	report.depthHist[DepthBucket(depth)]++;
	report.depthSinceWake = std::max(report.depthSinceWake, depth);
	const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - report.rollbackStart).count();
	report.rollbackMaxUs = std::max<int32_t>(report.rollbackMaxUs, static_cast<int32_t>(std::min<int64_t>(us, INT32_MAX)));
	report.rollbackToFrame = -1;
}

void gdxsv_net_report_input_read() {
	std::lock_guard<std::mutex> lock(reportMutex);
	report.pendingRead = Clock::now();
}

void gdxsv_net_report_input_sent() {
	std::lock_guard<std::mutex> lock(reportMutex);
	report.inputRead.Interval(report.pendingRead);
	report.inputSend.Interval(Clock::now());
}

void gdxsv_net_report_timesync() {
	int frame = -1;
	ggpo::getCurrentFrame(&frame);
	std::lock_guard<std::mutex> lock(reportMutex);
	if (report.timesyncFrames.size() < kMaxTimesyncFrames) report.timesyncFrames.push_back(frame);
}

void gdxsv_net_report_present() {
	if (!gdxsv_net_report_on) return;
	std::lock_guard<std::mutex> lock(reportMutex);
	report.present.Interval(Clock::now());
}

void gdxsv_net_report_wake(int64_t remainingUs) {
	if (!gdxsv_net_report_on) return;
	std::lock_guard<std::mutex> lock(reportMutex);
	report.budget.Add(remainingUs);
	if (remainingUs <= 0) {
		report.budgetExhausted++;
		report.exhaustedDepthHist[DepthBucket(report.depthSinceWake)]++;
	}
	report.depthSinceWake = 0;
}

void gdxsv_net_report_fill(proto::P2PMatchingReport* out) {
	NetReport r;
	{
		std::lock_guard<std::mutex> lock(reportMutex);
		r = report;
	}
	out->set_rollback_count(r.rollbackCount);
	for (int32_t c : r.depthHist) out->add_rollback_depth_hist(c);
	out->set_rollback_max_us(r.rollbackMaxUs);

	int late1 = 0, late15 = 0, late2 = 0, dropped = 0;
	for (int32_t s : r.present.samples) {
		if (s > kFramePeriodUs) late1++;
		if (s > kFramePeriodUs * 3 / 2) late15++;
		if (s > kFramePeriodUs * 2) late2++;
		// An interval of n periods shows the last frame for n - 1 more vsyncs.
		dropped += std::max(0, (s + kFramePeriodUs / 2) / kFramePeriodUs - 1);
	}
	out->set_late_frames_1(late1);
	out->set_late_frames_1_5(late15);
	out->set_late_frames_2(late2);
	out->set_dropped_frames(dropped);

	for (int32_t f : r.timesyncFrames) out->add_timesync_frames(f);
	FillStats(std::move(r.inputRead.samples), kFramePeriodUs, out->mutable_input_read_interval());
	FillStats(std::move(r.inputSend.samples), kFramePeriodUs, out->mutable_input_send_interval());
	FillStats(std::move(r.present.samples), kFramePeriodUs, out->mutable_present_interval());
	FillStats(std::move(r.budget.samples), 0, out->mutable_rollback_budget());
	out->set_budget_exhausted_frames(r.budgetExhausted);
	for (int32_t c : r.exhaustedDepthHist) out->add_budget_exhausted_depth_hist(c);
}
