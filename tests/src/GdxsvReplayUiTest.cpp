#include "gdxsv/gdxsv_replay_ui.h"

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace {
using UiState = GdxsvReplayUiState;

proto::BattleLogFile recording() {
	proto::BattleLogFile log;
	log.set_battle_code("1787426305907");
	for (int i = 0; i < 4; ++i)
		log.add_users()->set_team(i / 2 + 1);
	log.add_start_msg_indexes(0);
	log.add_start_msg_randoms(123);
	log.add_round_data();
	return log;
}
}

TEST(GdxsvReplayUi, LargeRecordingOnlyCopiesDisplayMetadata) {
	static_assert(sizeof(UiState) < 1024, "UI snapshot must remain small");
	auto log = recording();
	for (int i = 0; i < 100000; ++i)
		log.add_inputs(0x1234567812345678ULL);
	log.add_start_msg_indexes(10000);
	log.add_start_msg_randoms(456);
	log.mutable_round_data(0)->set_win_team(2);
	log.mutable_round_data(0)->add_used_ms(17);

	const auto ui = UiState::FromLog(log);
	EXPECT_EQ(100000, ui.inputCount);
	EXPECT_EQ(2, ui.roundCount);
	EXPECT_TRUE(ui.canChangeRound);
	EXPECT_EQ(10000, ui.rounds[0].endFrame);
	EXPECT_EQ(100000, ui.rounds[1].endFrame);
	EXPECT_EQ(2, ui.rounds[0].winTeam);
	EXPECT_EQ(17, ui.rounds[0].usedMs[0]);
	EXPECT_EQ((std::array<int, 4>{1, 1, 2, 2}), ui.userTeams);
	EXPECT_EQ(10u, ui.rounds.size());
	RecordProperty("snapshot_bytes", static_cast<int>(sizeof(ui)));

	// A rendered snapshot must not borrow any protobuf storage.
	log.Clear();
	EXPECT_EQ("1787426305907", ui.battleCode);
	EXPECT_EQ(100000, ui.inputCount);
	EXPECT_EQ(17, ui.rounds[0].usedMs[0]);
}

TEST(GdxsvReplayUi, ResultOnlyUpdateAndResetLeaveExistingReadersIndependent) {
	auto log = recording();
	log.add_inputs(1);
	GdxsvReplayUiSnapshot snapshot;
	snapshot.Publish(UiState::FromLog(log));
	const auto before = snapshot.Read();

	log.mutable_round_data(0)->set_win_team(1);
	log.mutable_round_data(0)->add_used_ms(7);
	snapshot.Publish(UiState::FromLog(log));
	const auto after = snapshot.Read();
	EXPECT_EQ(before.roundCount, after.roundCount);
	EXPECT_EQ(0, before.rounds[0].winTeam);
	EXPECT_EQ(1, after.rounds[0].winTeam);
	EXPECT_EQ(7, after.rounds[0].usedMs[0]);

	snapshot.Publish({});
	const auto reset = snapshot.Read();
	EXPECT_TRUE(reset.battleCode.empty());
	EXPECT_EQ(0, reset.inputCount);
	EXPECT_EQ(0, reset.roundCount);
	EXPECT_EQ(UiState::State::None, reset.state);
	EXPECT_EQ(1, after.rounds[0].winTeam);

	log.set_battle_code("next-battle");
	log.clear_round_data();
	snapshot.Publish(UiState::FromLog(log));
	EXPECT_EQ("next-battle", snapshot.Read().battleCode);
	EXPECT_FALSE(snapshot.Read().rounds[0].hasResult);
	EXPECT_EQ("1787426305907", after.battleCode);
}

TEST(GdxsvReplayUi, MalformedMetadataCannotGrowTheSnapshot) {
	proto::BattleLogFile log;
	for (int i = 0; i < 12; ++i) {
		log.add_users()->set_team(1);
		log.add_start_msg_indexes(i * 100);
		log.add_start_msg_randoms(i);
		auto* round = log.add_round_data();
		for (int j = 0; j < 12; ++j)
			round->add_used_ms(j + 1);
	}
	const auto ui = UiState::FromLog(log);
	EXPECT_EQ(10, ui.roundCount);
	EXPECT_EQ(4, ui.userCount);
	EXPECT_EQ(4, ui.rounds[9].usedMsCount);
	EXPECT_EQ(900, ui.rounds[9].startFrame);
	EXPECT_EQ(1000, ui.rounds[9].endFrame);
}

TEST(GdxsvReplayUi, RoundTimeUsesRecordingTimingOrTheExistingFallback) {
	auto log = recording();
	log.set_start_at(1000);
	for (int i = 0; i < 100; ++i)
		log.add_inputs(0);
	EXPECT_DOUBLE_EQ(0.01668335002, UiState::FromLog(log).inputSeconds);
	log.set_end_at(1002);
	EXPECT_DOUBLE_EQ(0.02, UiState::FromLog(log).inputSeconds);
	log.set_end_at(2000);
	EXPECT_DOUBLE_EQ(0.01668335002, UiState::FromLog(log).inputSeconds);
}

TEST(GdxsvReplayUi, TakeoverCountdownShowsAlignmentOnMismatchUnlessSkipped) {
	UiState ui;
	ui.takeoverTargetInput = 1;
	ui.takeoverAligning = true;
	EXPECT_TRUE(ui.NeedsTakeoverAlignment(0));
	EXPECT_TRUE(ui.NeedsTakeoverAlignment(1));

	ui.takeoverAligning = false;
	ui.takeoverCountdown = 60;
	EXPECT_TRUE(ui.NeedsTakeoverAlignment(0));
	EXPECT_FALSE(ui.NeedsTakeoverAlignment(1));

	ui.takeoverSkipInputMatching = true;
	EXPECT_FALSE(ui.NeedsTakeoverAlignment(0));
	EXPECT_FALSE(ui.NeedsTakeoverAlignment(1));
	EXPECT_FALSE(ui.NeedsTakeoverAlignment(2));

	ui.takeoverCountdown = 0;
	ui.takeoverSkipInputMatching = false;
	EXPECT_FALSE(ui.NeedsTakeoverAlignment(0));
}

TEST(GdxsvReplayUi, TakeoverSkipChoiceIsPublishedWithoutChangingOlderSnapshots) {
	GdxsvReplayUiSnapshot snapshot;
	UiState ui;
	ui.takeoverAligning = true;
	ui.takeoverTargetInput = 1;
	snapshot.Publish(ui);
	const auto matching = snapshot.Read();

	ui.takeoverAligning = false;
	ui.takeoverCountdown = 60;
	ui.takeoverSkipInputMatching = true;
	snapshot.Publish(ui);
	const auto skipped = snapshot.Read();
	EXPECT_TRUE(matching.NeedsTakeoverAlignment(0));
	EXPECT_FALSE(matching.takeoverSkipInputMatching);
	EXPECT_FALSE(skipped.NeedsTakeoverAlignment(0));
	EXPECT_TRUE(skipped.takeoverSkipInputMatching);
	EXPECT_EQ(60, skipped.takeoverCountdown);

	snapshot.Publish({});
	EXPECT_FALSE(snapshot.Read().takeoverSkipInputMatching);
	EXPECT_EQ(0, snapshot.Read().takeoverCountdown);
	EXPECT_TRUE(skipped.takeoverSkipInputMatching);
}

TEST(GdxsvReplayUi, ConcurrentReadersSeeCoherentSnapshotsDuringGrowthAndReset) {
	GdxsvReplayUiSnapshot snapshot;
	std::atomic<bool> start{false}, done{false};
	std::atomic<int> errors{0};
	std::thread writer([&] {
		auto log = recording();
		while (!start.load()) std::this_thread::yield();
		for (int frame = 1; frame <= 15000; ++frame) {
			log.add_inputs(frame);
			log.mutable_round_data(0)->set_win_team(frame % 2 + 1);
			auto ui = UiState::FromLog(log);
			ui.state = UiState::State::McsInBattle;
			ui.playbackFrame = frame;
			ui.timelineEnd = frame;
			ui.liveMode = frame % 2 != 0;
			snapshot.Publish(std::move(ui));
			if (frame % 100 == 0) snapshot.Publish({});
		}
		done.store(true);
	});
	std::vector<std::thread> readers;
	for (int i = 0; i < 4; ++i) {
		readers.emplace_back([&] {
			while (!start.load()) std::this_thread::yield();
			int reads = 0;
			do {
				const auto ui = snapshot.Read();
				if (ui.state == UiState::State::None) {
					if (!ui.battleCode.empty() || ui.inputCount != 0 || ui.roundCount != 0)
						++errors;
				} else if (ui.battleCode != "1787426305907" || ui.inputCount != ui.playbackFrame ||
					ui.timelineEnd != ui.inputCount || ui.roundCount != 1 ||
					ui.rounds[0].endFrame != ui.inputCount || ui.rounds[0].winTeam != ui.inputCount % 2 + 1 ||
					ui.liveMode != (ui.inputCount % 2 != 0)) {
					++errors;
				}
				++reads;
			} while (!done.load() || reads < 1000);
		});
	}
	start.store(true);
	writer.join();
	for (auto& reader : readers) reader.join();
	EXPECT_EQ(0, errors.load());
}
