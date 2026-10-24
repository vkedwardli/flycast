// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_net_report.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>

#include "gdxsv.pb.h"
#include "gdxsv_ggpo_hooks.h"
#include "network/ggpo.h"

std::atomic<bool> gdxsv_net_report_on{false};

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFramePeriodUs = 16683;  // 59.94 Hz
constexpr int kOffPeriodUs = 2000;     // an interval further than this from the frame period is off period
constexpr int kDepthBuckets = 8;	   // rollbacks of 0 to 6 frames; the last bucket: 7 or more
constexpr int kMaxTimesyncFrames = 32;

// Each counter has one writer thread; the report reads them from another
// thread. A relaxed load and store compiles to plain moves, unlike fetch_add.
void Bump(std::atomic<int32_t>& c, int32_t d = 1) { c.store(c.load(std::memory_order_relaxed) + d, std::memory_order_relaxed); }
int32_t Get(const std::atomic<int32_t>& c) { return c.load(std::memory_order_relaxed); }
void Set(std::atomic<int32_t>& c, int32_t v) { c.store(v, std::memory_order_relaxed); }

int32_t ClampUs(int64_t us) { return static_cast<int32_t>(std::clamp<int64_t>(us, INT32_MIN, INT32_MAX)); }
int32_t SinceUs(Clock::time_point t, Clock::time_point now) {
	return ClampUs(std::chrono::duration_cast<std::chrono::microseconds>(now - t).count());
}

// Fixed-size log-linear histogram of signed microseconds: magnitudes below kSub
// are exact, above them each power of two has kSub buckets (width under 1/kSub
// of the value). count, sum, min, max and off_period are exact.
class Hist {
   public:
	void Reset() {
		for (auto& b : pos_) Set(b, 0);
		for (auto& b : neg_) Set(b, 0);
		Set(count_, 0);
		Set(off_, 0);
		sum_.store(0, std::memory_order_relaxed);
		Set(min_, INT32_MAX);
		Set(max_, INT32_MIN);
	}

	// periodUs 0: off_period is not counted.
	void Add(int32_t us, int periodUs) {
		const int64_t v = us;
		const uint32_t m = static_cast<uint32_t>(v < 0 ? -v : v);
		Bump(v < 0 ? neg_[Bucket(m)] : pos_[Bucket(m)]);
		Bump(count_);
		sum_.store(sum_.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
		if (us < Get(min_)) Set(min_, us);
		if (Get(max_) < us) Set(max_, us);
		if (periodUs != 0 && kOffPeriodUs < std::abs(v - periodUs)) Bump(off_);
	}

	void Fill(proto::TimingStats* out) const {
		std::array<int32_t, kBuckets> pos, neg;
		int64_t n = 0;
		for (int i = 0; i < kBuckets; i++) {
			pos[i] = Get(pos_[i]);
			neg[i] = Get(neg_[i]);
			n += pos[i] + neg[i];
		}
		if (n == 0) return;
		const int32_t min = Get(min_), max = Get(max_);
		out->set_count(Get(count_));
		out->set_mean_us(static_cast<int32_t>(sum_.load(std::memory_order_relaxed) / std::max(1, Get(count_))));
		out->set_min_us(min);
		out->set_max_us(max);
		out->set_off_period(Get(off_));
		// Nearest rank, the bucket's middle, within [min, max].
		auto percentile = [&](double p) {
			const int64_t k = std::min<int64_t>(n - 1, std::max<int64_t>(0, static_cast<int64_t>(std::ceil(p * n)) - 1));
			int64_t seen = 0;
			for (int i = kBuckets - 1; 0 <= i; i--) {
				seen += neg[i];
				if (k < seen) return std::clamp(ClampUs(-Middle(i)), min, max);
			}
			for (int i = 0; i < kBuckets; i++) {
				seen += pos[i];
				if (k < seen) return std::clamp(ClampUs(Middle(i)), min, max);
			}
			return max;
		};
		out->set_p1_us(percentile(0.01));
		out->set_p5_us(percentile(0.05));
		out->set_p50_us(percentile(0.50));
		out->set_p99_us(percentile(0.99));
	}

   private:
	static constexpr int kSubBits = 5;
	static constexpr int kSub = 1 << kSubBits;
	static constexpr int kBuckets = kSub * (32 - kSubBits + 1);

	static int Bucket(uint32_t m) {
		if (m < kSub) return static_cast<int>(m);
		int e = kSubBits;  // the highest set bit
		while (e < 31 && (m >> (e + 1))) e++;
		return (e - kSubBits + 1) * kSub + static_cast<int>((m >> (e - kSubBits)) - kSub);
	}
	static int64_t Middle(int bucket) {
		if (bucket < kSub) return bucket;
		const int shift = bucket / kSub - 1;
		const int64_t lo = static_cast<int64_t>(kSub + bucket % kSub) << shift;
		return lo + ((int64_t{1} << shift) >> 1);
	}

	std::array<std::atomic<int32_t>, kBuckets> pos_{};
	std::array<std::atomic<int32_t>, kBuckets> neg_{};
	std::atomic<int32_t> count_{0};
	std::atomic<int32_t> off_{0};
	std::atomic<int64_t> sum_{0};
	std::atomic<int32_t> min_{INT32_MAX};
	std::atomic<int32_t> max_{INT32_MIN};
};

// An interval series: the time between two events.
struct Intervals {
	Clock::time_point last{};
	Hist hist;

	void Reset() {
		last = {};
		hist.Reset();
	}
	// Returns the interval, -1 for the first event.
	int32_t Add(Clock::time_point now) {
		int32_t us = -1;
		if (last != Clock::time_point{}) {
			us = SinceUs(last, now);
			hist.Add(us, kFramePeriodUs);
		}
		last = now;
		return us;
	}
};

// Writers: the emulation thread, except present (the UI thread with threaded
// rendering).
struct NetReport {
	Intervals inputRead;
	Intervals inputSend;
	Intervals present;
	Hist budget;		 // limiter slack per frame
	Hist rollbackFrame;	 // per rollback: its time / the frames it re-ran
	Clock::time_point pendingRead{};
	std::atomic<int32_t> rollbackCount{0};
	std::array<std::atomic<int32_t>, kDepthBuckets> depthHist{};
	Clock::time_point rollbackStart{};
	int rollbackToFrame = -1;  // the frame a running rollback ends at; -1: none runs
	int rollbackReruns = 0;	   // frames the running rollback re-ran so far
	std::atomic<int32_t> rollbackMaxUs{0};
	int depthSinceWake = 0;	 // deepest rollback since the last limiter wake
	std::atomic<int32_t> budgetExhausted{0};
	std::array<std::atomic<int32_t>, kDepthBuckets> exhaustedDepthHist{};
	std::atomic<int32_t> late1{0}, late15{0}, late2{0}, dropped{0};
	std::atomic<int32_t> timesyncCount{0};
	std::array<std::atomic<int32_t>, kMaxTimesyncFrames> timesyncFrames{};

	void Reset() {
		inputRead.Reset();
		inputSend.Reset();
		present.Reset();
		budget.Reset();
		rollbackFrame.Reset();
		pendingRead = {};
		Set(rollbackCount, 0);
		for (auto& c : depthHist) Set(c, 0);
		rollbackStart = {};
		rollbackToFrame = -1;
		rollbackReruns = 0;
		Set(rollbackMaxUs, 0);
		depthSinceWake = 0;
		Set(budgetExhausted, 0);
		for (auto& c : exhaustedDepthHist) Set(c, 0);
		Set(late1, 0);
		Set(late15, 0);
		Set(late2, 0);
		Set(dropped, 0);
		Set(timesyncCount, 0);
		for (auto& c : timesyncFrames) Set(c, 0);
	}
};

NetReport report;

int DepthBucket(int depth) { return std::clamp(depth, 0, kDepthBuckets - 1); }

}  // namespace

void gdxsv_net_report_session_start() {
	report.Reset();
	gdxsv_net_report_on = true;
}

void gdxsv_net_report_session_stop() { gdxsv_net_report_on = false; }

void gdxsv_net_report_reset() { report.Reset(); }

void gdxsv_net_report_rollback_begin(int toFrame) {
	report.rollbackStart = Clock::now();
	report.rollbackToFrame = toFrame;
	report.rollbackReruns = 0;
}

void gdxsv_net_report_rerun_end(int frame) {
	if (report.rollbackToFrame < 0) return;
	report.rollbackReruns++;
	if (frame + 1 < report.rollbackToFrame) return;
	const int depth = report.rollbackReruns;
	Bump(report.rollbackCount);
	Bump(report.depthHist[DepthBucket(depth)]);
	report.depthSinceWake = std::max(report.depthSinceWake, depth);
	const int32_t us = SinceUs(report.rollbackStart, Clock::now());
	if (Get(report.rollbackMaxUs) < us) Set(report.rollbackMaxUs, us);
	report.rollbackFrame.Add(us / depth, 0);
	report.rollbackToFrame = -1;
}

void gdxsv_net_report_input_read() { report.pendingRead = Clock::now(); }

void gdxsv_net_report_input_sent() {
	report.inputRead.Add(report.pendingRead);
	report.inputSend.Add(Clock::now());
}

void gdxsv_net_report_timesync() {
	int frame = -1;
	ggpo::getCurrentFrame(&frame);
	const int i = Get(report.timesyncCount);
	if (i < kMaxTimesyncFrames) {
		Set(report.timesyncFrames[i], frame);
		Set(report.timesyncCount, i + 1);
	}
}

void gdxsv_net_report_present() {
	if (!gdxsv_net_report_on) return;
	const int32_t us = report.present.Add(Clock::now());
	if (us <= kFramePeriodUs) return;
	Bump(report.late1);
	if (kFramePeriodUs * 3 / 2 < us) Bump(report.late15);
	if (kFramePeriodUs * 2 < us) Bump(report.late2);
	// An interval of n periods shows the last frame for n - 1 more vsyncs.
	Bump(report.dropped, std::max(0, (us + kFramePeriodUs / 2) / kFramePeriodUs - 1));
}

void gdxsv_net_report_wake(int64_t remainingUs) {
	// Before the first input, the wait includes the session setup.
	if (!gdxsv_net_report_on || report.pendingRead == Clock::time_point{}) return;
	report.budget.Add(ClampUs(remainingUs), 0);
	if (remainingUs <= 0) {
		Bump(report.budgetExhausted);
		Bump(report.exhaustedDepthHist[DepthBucket(report.depthSinceWake)]);
	}
	report.depthSinceWake = 0;
}

void gdxsv_net_report_fill(proto::P2PMatchingReport* out) {
	const NetReport& r = report;
	out->set_rollback_count(Get(r.rollbackCount));
	for (const auto& c : r.depthHist) out->add_rollback_depth_hist(Get(c));
	out->set_rollback_max_us(Get(r.rollbackMaxUs));
	r.rollbackFrame.Fill(out->mutable_rollback_frame_time());

	out->set_late_frames_1(Get(r.late1));
	out->set_late_frames_1_5(Get(r.late15));
	out->set_late_frames_2(Get(r.late2));
	out->set_dropped_frames(Get(r.dropped));

	const int timesyncs = std::min(Get(r.timesyncCount), kMaxTimesyncFrames);
	for (int i = 0; i < timesyncs; i++) out->add_timesync_frames(Get(r.timesyncFrames[i]));
	r.inputRead.hist.Fill(out->mutable_input_read_interval());
	r.inputSend.hist.Fill(out->mutable_input_send_interval());
	r.present.hist.Fill(out->mutable_present_interval());
	r.budget.Fill(out->mutable_rollback_budget());
	out->set_budget_exhausted_frames(Get(r.budgetExhausted));
	for (const auto& c : r.exhaustedDepthHist) out->add_budget_exhausted_depth_hist(Get(c));
}
