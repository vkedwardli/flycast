#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

// One packet per emulated frame. The host mixes packets with the same replay
// position, so scheduling or audio-device latency cannot delay a guest's sound.
// runInternal can span several vblanks during loading or a slow frame.
constexpr uint32_t kMultiPovAudioSamples = 16384;
// Nine delayed packets at 3x, plus a guest's lead and publication headroom.
// Keep the capacity a power of two so uint32_t cursor wrap preserves slots.
constexpr uint32_t kMultiPovAudioPackets = 16;
// Delay three displayed frames: 3/6/9 emulated packets at 1x/2x/3x. One frame
// beyond video's two-frame tolerance covers publication of the guest's PCM.
constexpr uint32_t kMultiPovAudioDelayFrames = 3;
constexpr int kGdxsvMultiPovScreens = 4;
constexpr uint32_t kMultiPovAudioMask = (1u << kGdxsvMultiPovScreens) - 1;

// Each screen owns its request slot. Briefing can resume input thirteen
// vblanks earlier on some POVs; delivery waits for requests from the same
// seek and round rather than relabeling their differently timed audio.
struct GdxsvMultiPovInputBarrier {
	struct Request {
		std::atomic<uint32_t> sequence {0};
		std::atomic<uint32_t> generation {0};
		std::atomic<uint64_t> position {0};
	} requests[kGdxsvMultiPovScreens];

	void publish(int screen, int input, uint32_t generation, uint32_t timeline) {
		auto& request = requests[screen];
		request.sequence.fetch_add(1, std::memory_order_acq_rel);
		request.position.store((uint64_t(timeline) << 32) | uint32_t(input), std::memory_order_relaxed);
		request.generation.store(generation, std::memory_order_relaxed);
		request.sequence.fetch_add(1, std::memory_order_release);
	}

	bool ready(int input, uint32_t generation, uint32_t timeline, uint32_t active) const {
		for (int screen = 0; screen < kGdxsvMultiPovScreens; ++screen) {
			if (!(active & (1u << screen))) continue;
			const auto& request = requests[screen];
			const uint32_t before = request.sequence.load(std::memory_order_acquire);
			if (before & 1) return false;
			const uint64_t position = request.position.load(std::memory_order_relaxed);
			const uint32_t requested_generation = request.generation.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if (request.sequence.load(std::memory_order_relaxed) != before ||
					requested_generation != generation || uint32_t(position >> 32) != timeline ||
					uint32_t(position) < uint32_t(input)) return false;
		}
		return true;
	}
};

struct GdxsvMultiPovAudioSample {
	int16_t l = 0;
	int16_t r = 0;
};

struct GdxsvMultiPovAudioPacket {
	uint64_t epoch = 0; // live battle generation and host seek generation
	int64_t position = 0;
	uint32_t count = 0;
	uint32_t timeline = 0; // round and first-start barrier, within the seek epoch
	uint32_t producer_reset = 0; // guest-local position discontinuity
	GdxsvMultiPovAudioSample samples[kMultiPovAudioSamples];

	void copyFrom(const GdxsvMultiPovAudioPacket& other) {
		epoch = other.epoch;
		position = other.position;
		count = other.count;
		timeline = other.timeline;
		producer_reset = other.producer_reset;
		if (count != 0) std::memcpy(samples, other.samples, count * sizeof(*samples));
	}

	int compare(uint64_t requested_epoch, int64_t requested_position, uint32_t requested_timeline) const {
		if (epoch != requested_epoch) return epoch < requested_epoch ? -1 : 1;
		if (timeline != requested_timeline) return timeline < requested_timeline ? -1 : 1;
		if (position != requested_position) return position < requested_position ? -1 : 1;
		return 0;
	}
};

// Single producer (guest), single consumer (host). Neither side overwrites
// memory until the other has released it. Atomics must also work across mmap.
struct GdxsvMultiPovAudioQueue {
	std::atomic<uint32_t> read {0};
	std::atomic<uint32_t> write {0};
	std::atomic<uint32_t> audible {0}; // host only: guests need not block while its PCM is silent
	std::atomic<uint32_t> frame_progress {0}; // advances even for silent, discarded seek frames
	std::atomic<uint64_t> ready_epoch {0}; // real guest seek has landed
	std::atomic<uint32_t> producer_reset {0}; // producer owns resets; consumer owns read
	GdxsvMultiPovAudioPacket packets[kMultiPovAudioPackets];

	void resetProducer() {
		producer_reset.fetch_add(1, std::memory_order_release);
	}

	bool push(const GdxsvMultiPovAudioPacket& packet) {
		if (packet.count > kMultiPovAudioSamples) return false;
		const uint32_t w = write.load(std::memory_order_relaxed);
		if (w - read.load(std::memory_order_acquire) >= kMultiPovAudioPackets)
			return false;
		packets[w % kMultiPovAudioPackets].copyFrom(packet);
		packets[w % kMultiPovAudioPackets].producer_reset = producer_reset.load(std::memory_order_relaxed);
		write.store(w + 1, std::memory_order_release);
		return true;
	}

	// Discard sounds from positions already played, including a previous seek.
	// Leave future packets queued, and never play a late sound on the next frame.
	bool take(uint64_t epoch, int64_t position, GdxsvMultiPovAudioPacket& out, bool& future, uint32_t timeline = 0) {
		future = false;
		uint32_t r = read.load(std::memory_order_relaxed);
		while (r != write.load(std::memory_order_acquire)) {
			const auto& packet = packets[r % kMultiPovAudioPackets];
			if (packet.producer_reset != producer_reset.load(std::memory_order_acquire)) {
				read.store(++r, std::memory_order_release);
				continue;
			}
			const int order = packet.compare(epoch, position, timeline);
			if (order > 0) {
				future = true;
				return false;
			}
			bool matches = order == 0;
			if (matches) out.copyFrom(packet);
			matches = matches && packet.producer_reset == producer_reset.load(std::memory_order_acquire);
			read.store(++r, std::memory_order_release);
			if (matches) return true;
		}
		return false;
	}

	// Consumer only. A session reset releases all queued PCM, including future
	// packets from the previous battle, so producers can start the next one.
	void discard() {
		read.store(write.load(std::memory_order_acquire), std::memory_order_release);
	}

	void discardThrough(uint64_t epoch, int64_t position, uint32_t timeline = 0) {
		discardUntil(epoch, position, timeline, true);
	}

	// Keep this frame for the mix, but release older PCM while the host waits
	// for video synchronization. Otherwise a full queue slows the lagging guest.
	void discardBefore(uint64_t epoch, int64_t position, uint32_t timeline = 0) {
		discardUntil(epoch, position, timeline, false);
	}

private:
	void discardUntil(uint64_t epoch, int64_t position, uint32_t timeline, bool inclusive) {
		uint32_t r = read.load(std::memory_order_relaxed);
		while (r != write.load(std::memory_order_acquire)) {
			const auto& packet = packets[r % kMultiPovAudioPackets];
			if (packet.producer_reset != producer_reset.load(std::memory_order_acquire)) {
				read.store(++r, std::memory_order_release);
				continue;
			}
			const int order = packet.compare(epoch, position, timeline);
			if (order > 0 || (order == 0 && !inclusive)) break;
			read.store(++r, std::memory_order_release);
		}
	}
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared audio requires lock-free atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared audio epochs require lock-free atomics");

// Each pending guest has its own progress deadline. A slow but advancing seek
// remains synchronized; progress by another guest cannot hide a stuck one.
struct GdxsvMultiPovSeekProgress {
	uint32_t observed[kGdxsvMultiPovScreens] {};
	int64_t last_progress_ms[kGdxsvMultiPovScreens] {};

	uint32_t stalled(const GdxsvMultiPovAudioQueue* queues, uint32_t pending, int64_t elapsed_ms, int stall_ms) {
		uint32_t mask = 0;
		for (int i = 1; i < kGdxsvMultiPovScreens; ++i) {
			if (!(pending & (1u << i))) continue;
			const uint32_t progress = queues[i].frame_progress.load(std::memory_order_acquire);
			if (progress != observed[i]) {
				observed[i] = progress;
				last_progress_ms[i] = elapsed_ms;
			} else if (elapsed_ms - last_progress_ms[i] >= stall_ms) {
				mask |= 1u << i;
			}
		}
		return mask;
	}
};

// Owned by the emulation thread, polled between frames without blocking the UI.
struct GdxsvMultiPovSeekRecovery {
	std::chrono::steady_clock::time_point start {};
	std::chrono::steady_clock::time_point next_process_check {};
	GdxsvMultiPovSeekProgress progress;
	bool expired(std::chrono::steady_clock::time_point now) const {
		return start != std::chrono::steady_clock::time_point{} &&
				now - start >= std::chrono::seconds(30);
	}
};

// Host-owned delay includes silent frames, so faster playback can emit a
// buffered audible frame during a silent speed burst. Natural round changes
// keep the old tail playing while the next round fills the same delay.
struct GdxsvMultiPovAudioDelay {
	// Six clock units per displayed frame represent 1x, 2x and 3x exactly.
	// Packet timestamps retain their age across a speed change; resetting or
	// immediately resizing the queue would discard or pause buffered sound.
	static constexpr uint64_t clock_units = 6;
	GdxsvMultiPovAudioPacket packets[kMultiPovAudioPackets];
	uint64_t captured_at[kMultiPovAudioPackets] {};
	uint64_t clock = 0;
	uint32_t next = 0, queued = 0;
	bool primed = false;

	void reset() { next = queued = 0; clock = 0; primed = false; }
	const GdxsvMultiPovAudioPacket* oldest() const {
		return queued == 0 ? nullptr : &packets[(next + kMultiPovAudioPackets - queued) % kMultiPovAudioPackets];
	}
	const GdxsvMultiPovAudioPacket* push(const GdxsvMultiPovAudioPacket& packet, int speed = 0) {
		if (queued != 0) {
			const auto& last = packets[(next + kMultiPovAudioPackets - 1) % kMultiPovAudioPackets];
			if (packet.epoch != last.epoch || packet.timeline < last.timeline ||
					(packet.timeline == last.timeline && packet.position <= last.position))
				reset();
		}
		clock += clock_units / (1 + std::clamp(speed, 0, 2));
		packets[next].copyFrom(packet);
		captured_at[next] = clock;
		next = (next + 1) % kMultiPovAudioPackets;
		++queued;
		const GdxsvMultiPovAudioPacket* out = nullptr;
		while (queued != 0) {
			const uint32_t first = (next + kMultiPovAudioPackets - queued) % kMultiPovAudioPackets;
			if (clock - captured_at[first] < clock_units * kMultiPovAudioDelayFrames) break;
			out = &packets[first];
			--queued;
			primed = true;
			// When slowing down, drain due silent burst packets together, so
			// they cannot spread one displayed frame's audio across three.
			if (out->count != 0) break;
		}
		return out;
	}
};

inline uint64_t gdxsv_multi_pov_audio_epoch(uint32_t battle, uint32_t seek) {
	return (uint64_t(battle) << 32) | seek;
}

inline int64_t gdxsv_multi_pov_audio_position(int input, uint32_t subframe) {
	return int64_t(input) * (int64_t(1) << 32) + subframe;
}

// The cutoff is the last mixed audio frame, rather than the host's current
// video position. Guests within the delay can still contribute their PCM.
inline bool gdxsv_multi_pov_audio_is_late(uint64_t epoch, int64_t position,
		uint64_t host_epoch, int64_t played_position, uint32_t timeline = 0, uint32_t host_timeline = 0) {
	return epoch < host_epoch || (epoch == host_epoch &&
			(timeline < host_timeline || (timeline == host_timeline && position <= played_position)));
}

// Empty packets acknowledge silence. The previous frame's audible flag cannot
// predict this frame: it remains false until the first PCM after a seek arrives.
inline void gdxsv_multi_pov_collect_audio(GdxsvMultiPovAudioQueue* queues,
		uint64_t epoch, int64_t position, GdxsvMultiPovAudioPacket* packets,
		uint32_t active, uint32_t& pending, uint32_t& mask, uint32_t timeline = 0) {
	for (int i = 1; i < kGdxsvMultiPovScreens; ++i) {
		if (!(pending & (1u << i))) continue;
		bool future;
		if (queues[i].take(epoch, position, packets[i], future, timeline)) {
			mask |= 1u << i;
			pending &= ~(1u << i);
		} else if (future || !(active & (1u << i))) {
			pending &= ~(1u << i);
		}
	}
}

// Gain follows active participants, never the current frame's arrival mask.
// Ramp over 75 ms at the AICA output rate so removal/rejoining does not click.
struct GdxsvMultiPovAudioGain {
	static constexpr int64_t unity = 1ll << 24;
	static constexpr uint32_t ramp_samples = 44100 * 75 / 1000;
	int64_t current = 0, target = 0;
	uint32_t remaining = 0;

	void update(uint32_t active) {
		uint32_t screens = 0;
		for (uint32_t bits = active & kMultiPovAudioMask; bits; bits >>= 1) screens += bits & 1;
		const int64_t next = unity / std::max(1u, screens);
		if (target == 0) current = next;
		else if (next != target) remaining = ramp_samples;
		target = next;
	}
	int64_t next() {
		if (remaining != 0) {
			current += (target - current) / remaining;
			--remaining;
		}
		return current;
	}
};

// The frame boundaries can differ by a sample because 44.1 kHz is not an
// integer number of samples per video frame. Interpolate differing lengths
// across the same frame interval instead of inserting a silent tail or dropping
// a sample at every boundary. Reserve headroom for every active participant;
// temporary missing packets do not change the gain. Clamp during a gain ramp
// if full-scale participants rejoin before the gain has reached its new level.
inline void gdxsv_multi_pov_mix_audio(const GdxsvMultiPovAudioPacket* packets,
		uint32_t mask, GdxsvMultiPovAudioSample* out, uint32_t count,
		uint32_t active = kMultiPovAudioMask, GdxsvMultiPovAudioGain* gain = nullptr) {
	GdxsvMultiPovAudioGain immediate;
	if (gain == nullptr) gain = &immediate;
	gain->update(active);
	for (uint32_t i = 0; i < count; ++i) {
		int32_t l = 0, r = 0;
		for (int screen = 0; screen < kGdxsvMultiPovScreens; ++screen) {
			const auto& packet = packets[screen];
			if (!(mask & (1u << screen)) || packet.count == 0) continue;
			if (packet.count == count) {
				l += packet.samples[i].l;
				r += packet.samples[i].r;
				continue;
			}
			const uint32_t span = count > 1 ? count - 1 : 1;
			const uint32_t position = i * (packet.count - 1);
			const uint32_t index = position / span;
			const uint32_t fraction = position % span;
			const auto& a = packet.samples[index];
			const auto& b = packet.samples[std::min(index + 1, packet.count - 1)];
			l += a.l + (int32_t(b.l) - a.l) * int32_t(fraction) / int32_t(span);
			r += a.r + (int32_t(b.r) - a.r) * int32_t(fraction) / int32_t(span);
		}
		const int64_t scale = gain->next();
		out[i].l = static_cast<int16_t>(std::clamp(int64_t(l) * scale / GdxsvMultiPovAudioGain::unity, int64_t(-32768), int64_t(32767)));
		out[i].r = static_cast<int16_t>(std::clamp(int64_t(r) * scale / GdxsvMultiPovAudioGain::unity, int64_t(-32768), int64_t(32767)));
	}
}
