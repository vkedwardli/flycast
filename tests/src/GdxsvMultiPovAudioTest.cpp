#include <tuple>
#include "gdxsv/gdxsv_multi_pov_audio.h"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
GdxsvMultiPovAudioPacket impulse(uint64_t epoch, int64_t position, int16_t amplitude) {
	GdxsvMultiPovAudioPacket p {};
	p.epoch = epoch;
	p.position = position;
	p.count = 736;
	p.samples[200] = {amplitude, static_cast<int16_t>(-amplitude)};
	return p;
}
}

TEST(GdxsvMultiPovAudio, EarlyBriefingRequestsWaitForEveryActiveViewpoint) {
	GdxsvMultiPovInputBarrier barrier;
	barrier.publish(2, 947, 0, 3);
	barrier.publish(3, 947, 0, 3);
	EXPECT_FALSE(barrier.ready(947, 0, 3, 15));
	barrier.publish(0, 947, 0, 3);
	EXPECT_FALSE(barrier.ready(947, 0, 3, 15));
	barrier.publish(1, 947, 0, 3);
	EXPECT_TRUE(barrier.ready(947, 0, 3, 15));
	// A screen that already delivered the batch still counts as ready.
	barrier.publish(1, 948, 0, 3);
	EXPECT_TRUE(barrier.ready(947, 0, 3, 15));
}

TEST(GdxsvMultiPovAudio, BriefingRequestsCannotReuseOldSeeksOrOtherRounds) {
	GdxsvMultiPovInputBarrier barrier;
	for (int screen = 0; screen < 4; ++screen) barrier.publish(screen, 8000, 1, 5);
	barrier.publish(0, 947, 2, 3);
	EXPECT_FALSE(barrier.ready(947, 2, 3, 15));
	for (int screen = 1; screen < 4; ++screen) barrier.publish(screen, 947, 2, 3);
	EXPECT_TRUE(barrier.ready(947, 2, 3, 15));
	barrier.publish(3, 947, 2, 4);
	EXPECT_FALSE(barrier.ready(947, 2, 3, 15));
	// Headless or exited screens cannot hold the remaining participants.
	EXPECT_TRUE(barrier.ready(947, 2, 3, 7));
}

TEST(GdxsvMultiPovAudio, DiscardsLateAudioAndPreservesFutureFrames) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	ASSERT_TRUE(q->push(impulse(1, 50, 100)));
	ASSERT_TRUE(q->push(impulse(1, 51, 200)));
	ASSERT_TRUE(q->push(impulse(1, 53, 300)));
	GdxsvMultiPovAudioPacket out;
	bool future;
	ASSERT_TRUE(q->take(1, 51, out, future));
	EXPECT_EQ(out.samples[200].l, 200);
	EXPECT_FALSE(q->take(1, 52, out, future));
	EXPECT_TRUE(future);
	ASSERT_TRUE(q->take(1, 53, out, future));
	EXPECT_EQ(out.samples[200].l, 300);
	EXPECT_FALSE(q->take(1, 54, out, future));
	EXPECT_FALSE(future);
}

TEST(GdxsvMultiPovAudio, SeekCanMoveBackWithoutReplayingOldAudio) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	ASSERT_TRUE(q->push(impulse(1, 5000, 100)));
	ASSERT_TRUE(q->push(impulse(2, 10, 200)));
	GdxsvMultiPovAudioPacket out;
	bool future;
	ASSERT_TRUE(q->take(2, 10, out, future));
	EXPECT_EQ(out.samples[200].l, 200);
}

TEST(GdxsvMultiPovAudio, GuestLocalResetDiscardsAheadPacketsWithinTheSameHostEpoch) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	for (uint32_t reset = 1; reset <= 3; ++reset) {
		ASSERT_TRUE(q->push(impulse(1, 5000, 100)));
		const uint32_t read = q->read.load();
		q->resetProducer();
		EXPECT_EQ(q->read.load(), read); // the producer never releases slots
		ASSERT_TRUE(q->push(impulse(1, 10, 200)));
		GdxsvMultiPovAudioPacket out;
		bool future;
		ASSERT_TRUE(q->take(1, 10, out, future));
		EXPECT_FALSE(future);
		EXPECT_EQ(out.samples[200].l, 200);
		EXPECT_EQ(out.producer_reset, reset);
	}
}

TEST(GdxsvMultiPovAudio, GuestLocalResetUnblocksAFullQueueWithoutAHandshake) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	for (uint32_t i = 0; i < kMultiPovAudioPackets; ++i)
		ASSERT_TRUE(q->push(impulse(1, 5000 + i, 100)));
	q->resetProducer();
	ASSERT_FALSE(q->push(impulse(1, 10, 200)));
	GdxsvMultiPovAudioPacket out;
	bool future;
	EXPECT_FALSE(q->take(1, 10, out, future));
	EXPECT_FALSE(future);
	EXPECT_EQ(q->read.load(), q->write.load());
	ASSERT_TRUE(q->push(impulse(1, 10, 200)));
	ASSERT_TRUE(q->take(1, 10, out, future));
	EXPECT_EQ(out.samples[200].l, 200);
}

TEST(GdxsvMultiPovAudio, BothDiscardHelpersReleaseResetPacketsAheadOfTheirTarget) {
	for (bool inclusive : {false, true}) {
		auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
		ASSERT_TRUE(q->push(impulse(1, 5000, 100)));
		q->resetProducer();
		ASSERT_TRUE(q->push(impulse(1, 10, 200)));
		ASSERT_TRUE(q->push(impulse(1, 11, 300)));
		if (inclusive) q->discardThrough(1, 10);
		else q->discardBefore(1, 10);
		GdxsvMultiPovAudioPacket out;
		bool future;
		ASSERT_TRUE(q->take(1, inclusive ? 11 : 10, out, future));
		EXPECT_EQ(out.samples[200].l, inclusive ? 300 : 200);
	}
}

TEST(GdxsvMultiPovAudio, LongPauseKeepsDistinctAudioFramesBeyondVisualSyncLimit) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	const int64_t position = (50ll << 32) + 2000;
	ASSERT_TRUE(q->push(impulse(1, position - 1, 100)));
	ASSERT_TRUE(q->push(impulse(1, position, 200)));
	GdxsvMultiPovAudioPacket out;
	bool future;
	ASSERT_TRUE(q->take(1, position, out, future));
	EXPECT_EQ(out.samples[200].l, 200);
}

TEST(GdxsvMultiPovAudio, RoundAndStartBarrierDiscardOldAheadPackets) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	for (uint32_t timeline : {2u, 3u, 4u}) {
		auto old = impulse(1, (100ll << 32) + 900, 111);
		old.timeline = timeline - 1;
		auto current = impulse(1, (100ll << 32) + 1, 222);
		current.timeline = timeline;
		ASSERT_TRUE(q->push(old));
		ASSERT_TRUE(q->push(current));
		GdxsvMultiPovAudioPacket out;
		bool future;
		ASSERT_TRUE(q->take(1, current.position, out, future, timeline));
		EXPECT_EQ(out.samples[200].l, 222);
		EXPECT_EQ(q->read.load(), q->write.load());
	}
}

TEST(GdxsvMultiPovAudio, CopiesOnlyValidSamplesAndRetainsLongFrames) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	auto packet = impulse(1, 1, 222);
	packet.count = 4 * 736;
	packet.samples[packet.count - 1] = {1234, -1234};
	GdxsvMultiPovAudioPacket out;
	out.samples[packet.count] = {4321, -4321};
	ASSERT_TRUE(q->push(packet));
	bool future;
	ASSERT_TRUE(q->take(1, 1, out, future));
	EXPECT_EQ(out.count, 4 * 736u);
	EXPECT_EQ(out.samples[out.count - 1].l, 1234);
	EXPECT_EQ(out.samples[out.count].l, 4321); // the unused tail was not copied
	packet.count = kMultiPovAudioSamples + 1;
	EXPECT_FALSE(q->push(packet));
}

TEST(GdxsvMultiPovAudio, FullQueueNeverOverwritesUnreadSoundAndWrapsSafely) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	q->read.store(UINT32_MAX - 3);
	q->write.store(UINT32_MAX - 3);
	for (uint32_t i = 0; i < kMultiPovAudioPackets; ++i) ASSERT_TRUE(q->push(impulse(0, i, 100 + i)));
	EXPECT_FALSE(q->push(impulse(0, kMultiPovAudioPackets, 999)));
	GdxsvMultiPovAudioPacket out;
	bool future;
	for (uint32_t i = 0; i < kMultiPovAudioPackets; ++i) {
		ASSERT_TRUE(q->take(0, i, out, future));
		EXPECT_EQ(out.samples[200].l, 100 + i);
	}
	ASSERT_TRUE(q->push(impulse(0, kMultiPovAudioPackets, 999)));
	ASSERT_TRUE(q->take(0, kMultiPovAudioPackets, out, future));
}

TEST(GdxsvMultiPovAudio, KeepsAllViewpointEffectsWithFixedFourStreamHeadroom) {
	GdxsvMultiPovAudioPacket packets[4] {};
	GdxsvMultiPovAudioSample out[736];
	for (int i = 0; i < 4; ++i) {
		packets[i] = impulse(0, 10, 20000);
		packets[i].samples[100 + i] = {static_cast<int16_t>(1000 * (i + 1)), 0};
	}
	packets[2].samples[735] = {123, 456};
	gdxsv_multi_pov_mix_audio(packets, 15, out, 736);
	EXPECT_EQ(out[200].l, 20000);
	EXPECT_EQ(out[200].r, -20000);
	for (int i = 0; i < 4; ++i) EXPECT_EQ(out[100 + i].l, 250 * (i + 1));
	EXPECT_EQ(out[735].l, 30);
	EXPECT_EQ(out[735].r, 114);
	// A missing screen leaves the other viewpoints audible.
	gdxsv_multi_pov_mix_audio(packets, 5, out, 736);
	EXPECT_EQ(out[101].l, 0);
	EXPECT_EQ(out[102].l, 750);
}

TEST(GdxsvMultiPovAudio, FourFullScaleStreamsDoNotClip) {
	GdxsvMultiPovAudioPacket packets[4] {};
	for (auto& packet : packets) {
		packet.count = 2;
		packet.samples[0] = {32767, -32768};
		packet.samples[1] = {12000, -12000};
	}
	GdxsvMultiPovAudioSample out[2];
	gdxsv_multi_pov_mix_audio(packets, 15, out, 2);
	EXPECT_EQ(out[0].l, 32767);
	EXPECT_EQ(out[0].r, -32768);
	EXPECT_EQ(out[1].l, 12000);
	EXPECT_EQ(out[1].r, -12000);
}

TEST(GdxsvMultiPovAudio, NormalizesActiveParticipantsWithoutBoostingAnIncompleteFrame) {
	GdxsvMultiPovAudioPacket packets[4] {};
	for (auto& packet : packets) {
		packet.count = 1;
		packet.samples[0] = {12000, -12000};
	}
	GdxsvMultiPovAudioSample out[1];
	for (uint32_t active : {15u, 7u, 3u, 1u}) {
		gdxsv_multi_pov_mix_audio(packets, active, out, 1, active);
		EXPECT_NEAR(out[0].l, 12000, 1);
		EXPECT_NEAR(out[0].r, -12000, 1);
	}
	GdxsvMultiPovAudioGain gain;
	gdxsv_multi_pov_mix_audio(packets, 15, out, 1, 15, &gain);
	const auto before = gain.current;
	gdxsv_multi_pov_mix_audio(packets, 7, out, 1, 15, &gain);
	EXPECT_EQ(out[0].l, 9000);
	EXPECT_EQ(gain.current, before);
	EXPECT_EQ(gain.remaining, 0u);
	gdxsv_multi_pov_mix_audio(packets, 15, out, 1, 15, &gain);
	EXPECT_EQ(out[0].l, 12000);
}

TEST(GdxsvMultiPovAudio, ParticipantChangesRampOver75MillisecondsAcrossFrameBoundaries) {
	GdxsvMultiPovAudioPacket packets[4] {};
	for (auto& packet : packets) {
		packet.count = 1;
		packet.samples[0] = {12000, -12000};
	}
	GdxsvMultiPovAudioSample out[1];
	GdxsvMultiPovAudioGain gain;
	gdxsv_multi_pov_mix_audio(packets, 15, out, 1, 15, &gain);
	int previous = 3000;
	for (uint32_t sample = 0; sample < GdxsvMultiPovAudioGain::ramp_samples; ++sample) {
		gdxsv_multi_pov_mix_audio(packets, 1, out, 1, 1, &gain);
		EXPECT_GE(out[0].l, previous);
		EXPECT_LE(out[0].l - previous, 4);
		EXPECT_EQ(out[0].l, -out[0].r);
		previous = out[0].l;
		if (sample == 0) EXPECT_LT(out[0].l, 3010);
	}
	EXPECT_EQ(out[0].l, 12000);
	EXPECT_EQ(gain.remaining, 0u);
	// Rejoining full-scale streams must saturate instead of overflowing while
	// the gain ramps back down, then converge to the four-stream level.
	for (auto& packet : packets) packet.samples[0] = {32767, -32768};
	for (uint32_t sample = 0; sample < GdxsvMultiPovAudioGain::ramp_samples; ++sample) {
		gdxsv_multi_pov_mix_audio(packets, 15, out, 1, 15, &gain);
		EXPECT_EQ(out[0].l, 32767);
		EXPECT_EQ(out[0].r, -32768);
	}
	EXPECT_EQ(gain.current, GdxsvMultiPovAudioGain::unity / 4);
}

TEST(GdxsvMultiPovAudio, DifferentFrameLengthsKeepContinuousSoundAtBothEnds) {
	GdxsvMultiPovAudioPacket packets[4] {};
	for (int screen = 0; screen < 4; ++screen) {
		packets[screen].count = screen & 1 ? 735 : 737;
		for (uint32_t i = 0; i < packets[screen].count; ++i)
			packets[screen].samples[i] = {12000, -12000};
	}
	GdxsvMultiPovAudioSample out[736];
	gdxsv_multi_pov_mix_audio(packets, 15, out, 736);
	for (const auto& sample : out) {
		EXPECT_EQ(sample.l, 12000);
		EXPECT_EQ(sample.r, -12000);
	}
	packets[1].samples[734] = {16000, -16000};
	gdxsv_multi_pov_mix_audio(packets, 15, out, 736);
	EXPECT_EQ(out[735].l, 13000); // last sample participates instead of being padded
	EXPECT_EQ(out[735].r, -13000);
}

TEST(GdxsvMultiPovAudio, SilentPacketAcknowledgesTheFrameWithoutReplayingSamples) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	auto packet = impulse(2, 20, 10000);
	packet.count = 0;
	ASSERT_TRUE(q->push(packet));
	GdxsvMultiPovAudioPacket packets[4] {};
	bool future;
	ASSERT_TRUE(q->take(2, 20, packets[1], future));
	EXPECT_EQ(packets[1].count, 0u);
	GdxsvMultiPovAudioSample out[736];
	gdxsv_multi_pov_mix_audio(packets, 2, out, 736);
	for (const auto& sample : out) {
		EXPECT_EQ(sample.l, 0);
		EXPECT_EQ(sample.r, 0);
	}
}

TEST(GdxsvMultiPovAudio, SilentHostReleasesAFullQueueIncludingFutureFrames) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	for (uint32_t i = 0; i < kMultiPovAudioPackets; ++i) ASSERT_TRUE(q->push(impulse(1, 500 + i, 123)));
	EXPECT_FALSE(q->push(impulse(1, 500 + kMultiPovAudioPackets, 123)));
	q->discard();
	ASSERT_TRUE(q->push(impulse(2, 10, 456)));
	GdxsvMultiPovAudioPacket out;
	bool future;
	ASSERT_TRUE(q->take(2, 10, out, future));
	EXPECT_EQ(out.samples[200].l, 456);
}

TEST(GdxsvMultiPovAudio, SilentFlagDoesNotDropTheFirstFrameAfterASeek) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(4);
	GdxsvMultiPovAudioPacket packets[4] {};
	uint32_t pending = 14, mask = 1;
	ASSERT_TRUE(queues[1].push(impulse(1, 20, 1000)));
	auto silent = impulse(1, 20, 999);
	silent.count = 0;
	ASSERT_TRUE(queues[2].push(silent));
	gdxsv_multi_pov_collect_audio(queues.get(), 1, 20, packets, 15, pending, mask);
	EXPECT_EQ(mask, 7u); // both an audible frame and an explicit silent frame
	ASSERT_EQ(pending, 8u); // only the late, audible guest needs a wait
	gdxsv_multi_pov_collect_audio(queues.get(), 1, 20, packets, 15, pending, mask);
	EXPECT_EQ(pending, 8u); // wait for this frame's packet, not the stale flag
	ASSERT_TRUE(queues[3].push(impulse(1, 20, 2000)));
	gdxsv_multi_pov_collect_audio(queues.get(), 1, 20, packets, 15, pending, mask);
	EXPECT_EQ(pending, 0u);
	EXPECT_EQ(mask, 15u);
}

TEST(GdxsvMultiPovAudio, DelayedMixIncludesGuestsTwoFramesBehindBeforeTheyPublishAgain) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(4);
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	GdxsvMultiPovAudioPacket packets[4] {};
	GdxsvMultiPovAudioSample out[736];
	for (int frame = 0; frame < 300; ++frame) {
		const auto* played = delay->push(impulse(1, frame, 1000 + frame));
		if (played != nullptr) {
			packets[0].copyFrom(*played);
			uint32_t pending = 14, mask = 1;
			gdxsv_multi_pov_collect_audio(queues.get(), 1, played->position, packets, 15, pending, mask);
			ASSERT_EQ(mask, 15u) << "frame " << frame;
			gdxsv_multi_pov_mix_audio(packets, mask, out, played->count);
			EXPECT_EQ(out[200].l, 1000 + played->position);
		}
		// Publish AFTER collection. The third guest stays two complete video
		// frames behind and publishes its PCM after reporting its position.
		for (int guest = 1; guest < 4; ++guest) {
			const int position = frame - (guest - 1);
			if (position >= 0) ASSERT_TRUE(queues[guest].push(impulse(1, position, 1000 + position)));
		}
	}
}

TEST(GdxsvMultiPovAudio, FasterPlaybackCoversTwoDisplayedFramesOfGuestLagAndLead) {
	for (int speed : {0, 1, 2}) {
		SCOPED_TRACE(speed);
		const int burst = 1 + speed;
		auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(4);
		auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
		GdxsvMultiPovAudioPacket packets[4] {};
		// One guest leads by two displayed frames; the others trail by one
		// and two. Publish in whole bursts AFTER the host's collection.
		for (int frame = 0; frame < 2 * burst; ++frame)
			ASSERT_TRUE(queues[1].push(impulse(1, frame, 1000 + frame)));
		for (int display = 0; display < 100; ++display) {
			for (int part = 0; part < burst; ++part) {
				const int frame = display * burst + part;
				const auto* played = delay->push(impulse(1, frame, 1000 + frame), speed);
				if (display < 3) { EXPECT_EQ(played, nullptr); continue; }
				ASSERT_NE(played, nullptr);
				EXPECT_EQ(played->position, frame - 3 * burst);
				packets[0].copyFrom(*played);
				uint32_t pending = 14, mask = 1;
				gdxsv_multi_pov_collect_audio(queues.get(), 1, played->position, packets, 15, pending, mask);
				ASSERT_EQ(mask, 15u) << "display " << display << " part " << part;
				for (const auto& packet : packets) EXPECT_EQ(packet.samples[200].l, 1000 + played->position);
			}
			for (int guest = 1; guest < 4; ++guest) {
				const int guest_display = display + (guest == 1 ? 2 : 1 - guest);
				if (guest_display < 0) continue;
				for (int part = 0; part < burst; ++part) {
					const int frame = guest_display * burst + part;
					ASSERT_TRUE(queues[guest].push(impulse(1, frame, 1000 + frame))) << "guest " << guest;
				}
			}
		}
	}
}

TEST(GdxsvMultiPovAudio, SpeedChangesPreserveBufferedAudioWithoutGapsOrDuplicates) {
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	int display = 0, frame = 0;
	for (int speed : {0, 1, 2, 1, 0, 2, 0}) {
		for (int step = 0; step < 10; ++step, ++display) {
			int audible = 0;
			for (int part = 0; part <= speed; ++part) {
				auto packet = impulse(1, frame++, 1000 + display);
				packet.count = part == 0 ? 735 + display % 2 : 0;
				const auto* played = delay->push(packet, speed);
				if (played == nullptr || played->count == 0) continue;
				++audible;
				EXPECT_EQ(played->samples[200].l, 1000 + display - 3) << "display " << display;
				EXPECT_EQ(played->count, 735u + (display - 3) % 2);
			}
			EXPECT_EQ(audible, display < 3 ? 0 : 1) << "display " << display << " speed " << speed;
			EXPECT_EQ(delay->primed, display >= 3);
		}
	}
}

TEST(GdxsvMultiPovAudio, HitchDropsOnlyUnbufferedFramesAndGuestRejoinsWithoutASeek) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(4);
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	GdxsvMultiPovAudioPacket packets[4] {};
	int incomplete = 0;
	for (int frame = 0; frame < 300; ++frame) {
		const auto* played = delay->push(impulse(1, frame, 1000));
		if (played != nullptr) {
			packets[0].copyFrom(*played);
			uint32_t pending = 14, mask = 1;
			gdxsv_multi_pov_collect_audio(queues.get(), 1, played->position, packets, 15, pending, mask);
			if (played->position >= 80 && played->position < 100) {
				EXPECT_EQ(mask, 7u);
				++incomplete;
			} else ASSERT_EQ(mask, 15u) << "frame " << frame;
		}
		for (int guest = 1; guest < 4; ++guest) {
			const int position = frame - 2;
			if (position >= 0 && !(guest == 3 && position >= 80 && position < 100))
				ASSERT_TRUE(queues[guest].push(impulse(1, position, 1000)));
		}
	}
	EXPECT_EQ(incomplete, 20);
}

TEST(GdxsvMultiPovAudio, DelayedOutputKeepsActualSampleCountDuringSilentSpeedBursts) {
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	for (int frame = 0; frame < 10; ++frame) {
		auto packet = impulse(1, frame, 1000 + frame);
		packet.count = frame % 3 == 0 ? 735 + frame % 2 : 0;
		const auto* played = delay->push(packet);
		if (frame < int(kMultiPovAudioDelayFrames)) EXPECT_EQ(played, nullptr);
		else {
			ASSERT_NE(played, nullptr);
			const int position = frame - kMultiPovAudioDelayFrames;
			EXPECT_EQ(played->position, position);
			EXPECT_EQ(played->count, position % 3 == 0 ? 735u + position % 2 : 0u);
			if (played->count) EXPECT_EQ(played->samples[200].l, 1000 + position);
		}
	}
}

TEST(GdxsvMultiPovAudio, DiscontinuitiesDiscardBufferedSoundAndPrimeTheNewTimeline) {
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	for (const auto [epoch, timeline, start] : {
		std::tuple<uint64_t, uint32_t, int>{1, 0, 500}, {2, 1, 10}, {2, 0, 500}, {2, 0, 10}}) {
		for (int frame = 0; frame <= int(kMultiPovAudioDelayFrames); ++frame) {
			auto packet = impulse(epoch, start + frame, 1000);
			packet.timeline = timeline;
			const auto* played = delay->push(packet);
			if (frame < int(kMultiPovAudioDelayFrames)) EXPECT_EQ(played, nullptr);
			else {
				ASSERT_NE(played, nullptr);
				EXPECT_EQ(played->position, start);
				EXPECT_EQ(played->epoch, epoch);
				EXPECT_EQ(played->timeline, timeline);
			}
		}
	}
	delay->reset();
	EXPECT_EQ(delay->oldest(), nullptr);
	EXPECT_EQ(delay->push(impulse(3, 0, 1000)), nullptr);
}

TEST(GdxsvMultiPovAudio, VideoDrainPreservesAudioStillNeededByTheDelayedMix) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	for (int frame = 2000; frame < 2010; ++frame) {
		const int64_t position = gdxsv_multi_pov_audio_position(20, frame);
		if (delay->oldest()) q->discardBefore(1, delay->oldest()->position);
		const auto* played = delay->push(impulse(1, position, 1000));
		if (played) {
			GdxsvMultiPovAudioPacket out;
			bool future;
			ASSERT_TRUE(q->take(1, played->position, out, future));
		}
		ASSERT_TRUE(q->push(impulse(1, position, 1000)));
	}
}

TEST(GdxsvMultiPovAudio, NaturalRoundChangePlaysTheBufferedTailWithoutPrimingAnotherGap) {
	auto delay = std::make_unique<GdxsvMultiPovAudioDelay>();
	for (int frame = 0; frame < 20; ++frame) {
		// The next round can start at the same input with a reset subframe.
		auto packet = impulse(1, frame < 10 ? 100 + frame : frame - 10, 1000 + frame);
		packet.timeline = frame < 10 ? 1 : 2;
		const auto* played = delay->push(packet);
		if (frame < int(kMultiPovAudioDelayFrames)) EXPECT_EQ(played, nullptr);
		else {
			ASSERT_NE(played, nullptr) << "frame " << frame;
			const int actual = frame - kMultiPovAudioDelayFrames;
			EXPECT_EQ(played->samples[200].l, 1000 + actual);
			EXPECT_EQ(played->timeline, actual < 10 ? 1u : 2u);
		}
	}
}

TEST(GdxsvMultiPovAudio, SpeedBurstDiscardsSkippedSoundButKeepsTheNextAudibleFrame) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	ASSERT_TRUE(q->push(impulse(1, 20, 1000)));
	ASSERT_TRUE(q->push(impulse(1, 21, 2000)));
	ASSERT_TRUE(q->push(impulse(1, 22, 3000)));
	ASSERT_TRUE(q->push(impulse(1, 23, 4000)));
	q->discardThrough(1, 22);
	GdxsvMultiPovAudioPacket out;
	bool future;
	ASSERT_TRUE(q->take(1, 23, out, future));
	EXPECT_EQ(out.samples[200].l, 4000);
	EXPECT_EQ(q->read.load(), q->write.load());
}

TEST(GdxsvMultiPovAudio, VideoWaitReleasesOldAudioWithoutLosingTheCurrentOrFutureMix) {
	auto q = std::make_unique<GdxsvMultiPovAudioQueue>();
	const int next = 20 + kMultiPovAudioPackets;
	for (uint32_t i = 0; i < kMultiPovAudioPackets; ++i) ASSERT_TRUE(q->push(impulse(1, 20 + i, 1000 + i)));
	ASSERT_FALSE(q->push(impulse(1, next, 2000)));
	q->discardBefore(1, next - 2);
	ASSERT_TRUE(q->push(impulse(1, next, 2000)));
	GdxsvMultiPovAudioPacket packet;
	bool future;
	ASSERT_TRUE(q->take(1, next - 2, packet, future));
	EXPECT_EQ(packet.samples[200].l, 1000 + kMultiPovAudioPackets - 2);
	ASSERT_TRUE(q->take(1, next - 1, packet, future));
	EXPECT_EQ(packet.samples[200].l, 1000 + kMultiPovAudioPackets - 1);
	ASSERT_TRUE(q->take(1, next, packet, future));
	EXPECT_EQ(packet.samples[200].l, 2000);
}

TEST(GdxsvMultiPovAudio, CatchUpDropsOnlyAudioTheHostHasAlreadyPlayed) {
	const uint64_t epoch = (3ull << 32) | 2;
	const auto played = gdxsv_multi_pov_audio_position(20, 100);
	EXPECT_TRUE(gdxsv_multi_pov_audio_is_late(epoch, gdxsv_multi_pov_audio_position(10, 1), epoch, played));
	EXPECT_TRUE(gdxsv_multi_pov_audio_is_late(epoch, played, epoch, played));
	EXPECT_FALSE(gdxsv_multi_pov_audio_is_late(epoch, played + 1, epoch, played));
	EXPECT_FALSE(gdxsv_multi_pov_audio_is_late(epoch, gdxsv_multi_pov_audio_position(21, 1), epoch, played));
	EXPECT_TRUE(gdxsv_multi_pov_audio_is_late(epoch - 1, gdxsv_multi_pov_audio_position(1000, 1), epoch, played));
	EXPECT_FALSE(gdxsv_multi_pov_audio_is_late(epoch + 1, 0, epoch, played));
	EXPECT_FALSE(gdxsv_multi_pov_audio_is_late(epoch, 0, epoch, -1)); // delay is priming
	// Audio counters continue past the visual subframe saturation at 1023.
	EXPECT_FALSE(gdxsv_multi_pov_audio_is_late(epoch, gdxsv_multi_pov_audio_position(20, 2000), epoch, played));
	EXPECT_TRUE(gdxsv_multi_pov_audio_is_late(epoch, gdxsv_multi_pov_audio_position(1000, 0), epoch, played, 2, 3));
	EXPECT_FALSE(gdxsv_multi_pov_audio_is_late(epoch, 0, epoch, played, 4, 3));
}

TEST(GdxsvMultiPovAudio, LongSeekKeepsWaitingWhileEveryGuestMakesProgress) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(kGdxsvMultiPovScreens);
	GdxsvMultiPovSeekProgress progress;
	for (int elapsed = 0; elapsed <= 12000; elapsed += 500) {
		for (int i = 1; i < kGdxsvMultiPovScreens; ++i)
			queues[i].frame_progress.fetch_add(1);
		EXPECT_EQ(progress.stalled(queues.get(), 0xe, elapsed, 2000), 0u);
	}
	EXPECT_EQ(progress.stalled(queues.get(), 0xe, 13999, 2000), 0u);
	EXPECT_EQ(progress.stalled(queues.get(), 0xe, 14000, 2000), 0xeu);
}

TEST(GdxsvMultiPovAudio, ProgressCannotExtendRecoveryBeyondItsOverallLimit) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(4);
	GdxsvMultiPovSeekRecovery recovery;
	recovery.start = std::chrono::steady_clock::now();
	for (int elapsed = 0; elapsed <= 30000; elapsed += 500) {
		for (int i = 1; i < 4; ++i) queues[i].frame_progress.fetch_add(1);
		EXPECT_EQ(recovery.progress.stalled(queues.get(), 14, elapsed, 2000), 0u);
		EXPECT_EQ(recovery.expired(recovery.start + std::chrono::milliseconds(elapsed)), elapsed >= 30000);
	}
}

TEST(GdxsvMultiPovAudio, OtherGuestsProgressCannotHideAStuckGuest) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(kGdxsvMultiPovScreens);
	GdxsvMultiPovSeekProgress progress;
	for (int elapsed = 0; elapsed <= 2000; elapsed += 500) {
		queues[1].frame_progress.fetch_add(1);
		queues[3].frame_progress.fetch_add(1);
		EXPECT_EQ(progress.stalled(queues.get(), 0xe, elapsed, 2000), elapsed == 2000 ? 4u : 0u);
	}
	// A completed or disconnected guest no longer belongs to the pending mask.
	EXPECT_EQ(progress.stalled(queues.get(), 0xa, 2000, 2000), 0u);
}

TEST(GdxsvMultiPovAudio, SeekProgressWrapStillRefreshesItsOwnDeadline) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(kGdxsvMultiPovScreens);
	GdxsvMultiPovSeekProgress progress;
	queues[1].frame_progress.store(UINT32_MAX);
	EXPECT_EQ(progress.stalled(queues.get(), 2, 0, 2000), 0u);
	queues[1].frame_progress.fetch_add(1);
	EXPECT_EQ(progress.stalled(queues.get(), 2, 1900, 2000), 0u);
	EXPECT_EQ(progress.stalled(queues.get(), 2, 3899, 2000), 0u);
	EXPECT_EQ(progress.stalled(queues.get(), 2, 3900, 2000), 2u);
}

TEST(GdxsvMultiPovAudio, GuestStoppingDuringTheWaitDoesNotUseTheDeadline) {
	auto queues = std::make_unique<GdxsvMultiPovAudioQueue[]>(4);
	GdxsvMultiPovAudioPacket packets[4] {};
	uint32_t pending = 2, mask = 1;
	gdxsv_multi_pov_collect_audio(queues.get(), 1, 20, packets, 3, pending, mask);
	ASSERT_EQ(pending, 2u);
	gdxsv_multi_pov_collect_audio(queues.get(), 1, 20, packets, 1, pending, mask);
	EXPECT_EQ(pending, 0u);
	EXPECT_EQ(mask, 1u);
}

#ifndef _WIN32
TEST(GdxsvMultiPovAudio, ThreeProcessesArrivingAtDifferentTimesMixIntoOneAlignedImpulse) {
	const size_t size = 3 * sizeof(GdxsvMultiPovAudioQueue);
	void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	auto* queues = static_cast<GdxsvMultiPovAudioQueue*>(mapping);
	for (int i = 0; i < 3; ++i) new (&queues[i]) GdxsvMultiPovAudioQueue();
	pid_t children[3];
	for (int i = 0; i < 3; ++i) {
		children[i] = fork();
		ASSERT_GE(children[i], 0);
		if (children[i] == 0) {
			usleep((i + 1) * 11000); // independent producer scheduling
			for (int frame = 0; frame < 6; ++frame) {
				if (!queues[i].push(impulse(1, frame, 1000 * (i + 2)))) _exit(1);
			}
			_exit(0);
		}
	}
	for (int frame = 0; frame < 6; ++frame) {
		GdxsvMultiPovAudioPacket packets[4];
		packets[0] = impulse(1, frame, 1000);
		for (int i = 0; i < 3; ++i) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			bool future = false, found = false;
			while (!(found = queues[i].take(1, frame, packets[i + 1], future)) && !future &&
					std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			EXPECT_TRUE(found);
			if (!found) packets[i + 1].count = 0;
		}
		GdxsvMultiPovAudioSample out[736];
		gdxsv_multi_pov_mix_audio(packets, 15, out, 736);
		for (int sample = 0; sample < 736; ++sample) {
			EXPECT_EQ(out[sample].l, sample == 200 ? 2500 : 0);
			EXPECT_EQ(out[sample].r, sample == 200 ? -2500 : 0);
		}
	}
	for (pid_t child : children) {
		int status;
		ASSERT_EQ(waitpid(child, &status, 0), child);
		EXPECT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), 0);
	}
	munmap(mapping, size);
}
#endif
