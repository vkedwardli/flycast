#include "gdxsv/gdxsv_backend_replay.h"

#include "cfg/option.h"
#include "gtest/gtest.h"

class GdxsvReplayInputTest : public ::testing::Test {
 protected:
	void SetUp() override {
		for (int player = 0; player < 4; ++player)
			replay_.log_file_.add_users();
		replay_.state_ = GdxsvBackendReplay::State::McsInBattle;
		replay_.live_mode_ = true;
		// Exercise input delivery without rendering or starting an emulator.
		replay_.seeking_ = true;
	}

	void TearDown() override {
		config::ThreadedRendering = threaded_rendering_;
#ifdef _WIN32
		config::JoystickPolling = joystick_polling_;
#endif
		config::GdxMinDelay = input_delay_;
		settings.gdxsv.replayModeActive = replay_mode_active_;
		settings.aica.muteAudio = mute_audio_;
		settings.aica.audioFade = audio_fade_;
	}

	void AddInput(u64 input) { replay_.log_file_.add_inputs(input); }
	void SetOffline() { replay_.live_mode_ = false; }
	int InputIndex() const { return replay_.key_msg_count_; }
	std::deque<u8>& Reply() { return replay_.recv_buf_; }
	void DeliverDirectly() { replay_.DeliverKeyMsgBatch(); }

	void EnableTakeover(u16 input) {
		replay_.takeover_ = true;
		replay_.pov_ = 2;
		replay_.takeover_input_buf_.push_back(input);
		// Supply one delayed controller sample without polling host devices.
		config::GdxMinDelay = 1;
		config::ThreadedRendering = true;
#ifdef _WIN32
		config::JoystickPolling = false;
#endif
	}

	void PrepareTakeoverAlignment(bool retry, u16 target) {
		SetOffline();
		// Model the states left by TakeOver and RetryTakeover without loading
		// emulator save states. Retry deliberately keeps takeover_ true.
		replay_.takeover_ = retry;
		replay_.pause_menu_opend_ = true;
		replay_.BeginTakeoverAlignment(target);
		replay_.ctrl_commands_.clear();
	}

	void SendTakeoverInput(u16 input) {
		replay_.ui_commands_.emplace_back(GdxsvBackendReplay::ReplayCtrlCommand::TakeoverInput, input);
		replay_.ProcessUiCommands();
	}

	void SkipTakeoverAlignment() {
		replay_.ui_commands_.emplace_back(GdxsvBackendReplay::ReplayCtrlCommand::SkipTakeoverAlignment);
		replay_.ProcessUiCommands();
	}

	void CancelTakeover() {
		replay_.ui_commands_.emplace_back(GdxsvBackendReplay::ReplayCtrlCommand::CancelTakeover);
		replay_.ProcessUiCommands();
	}

	void ResetReplay() { replay_.Reset(); }
	void SetReplayState(GdxsvBackendReplay::State state) { replay_.state_ = state; }
	void SetLive() { replay_.live_mode_ = true; }
	void CloseTakeoverMenu() { replay_.pause_menu_opend_ = false; }
	bool IsTakingOver() const { return replay_.takeover_; }
	bool IsAligningTakeover() const { return replay_.takeover_aligning_; }
	bool IsSkippingTakeoverAlignment() const { return replay_.takeover_skip_input_matching_; }
	int TakeoverCountdown() const { return replay_.takeover_countdown_; }
	const std::deque<u16>& TakeoverInputs() const { return replay_.takeover_input_buf_; }
	size_t PendingControlCommands() { return replay_.ctrl_commands_.size(); }
	bool StartTakeoverQueued() {
		return replay_.ctrl_commands_.contains(GdxsvBackendReplay::ReplayCtrlCommand::StartTakeover);
	}

	void Send(McsMessage::MsgType type) {
		replay_.ProcessMcsMessage(McsMessage::Create(type, 0));
	}

	void CloseStream() {
		replay_.log_file_.set_close_reason("battle ended");
		replay_.CheckLiveUpdate();
		ASSERT_FALSE(replay_.live_mode_);
	}

	void ExpectBatch(u64 input) {
		auto bytes = Reply();
		ASSERT_EQ(40u, bytes.size());
		for (int player = 0; player < 4; ++player) {
			McsMessage msg;
			ASSERT_EQ(10, msg.Deserialize(bytes));
			ASSERT_EQ(McsMessage::KeyMsg1, msg.Type());
			EXPECT_EQ(player, msg.Sender());
			EXPECT_EQ(static_cast<u16>(input >> (player * 16)), msg.FirstInput());
			for (int byte = 0; byte < 10; ++byte)
				bytes.pop_front();
		}
		EXPECT_TRUE(bytes.empty());
	}

	static constexpr u64 FirstInput = 0x0008000400020001ULL;
	static constexpr u64 SecondInput = 0x0080004000200010ULL;

 private:
	GdxsvBackendReplay replay_;
	const bool threaded_rendering_ = config::ThreadedRendering;
#ifdef _WIN32
	const bool joystick_polling_ = config::JoystickPolling;
#endif
	const int input_delay_ = config::GdxMinDelay;
	const bool replay_mode_active_ = settings.gdxsv.replayModeActive;
	const bool mute_audio_ = settings.aica.muteAudio;
	const float audio_fade_ = settings.aica.audioFade;
};

TEST_F(GdxsvReplayInputTest, LiveStarvationDoesNotQueueNeutralInput) {
	for (int poll = 0; poll < 10; ++poll) {
		Send(McsMessage::KeyMsg1);
		Send(McsMessage::ForceMsg);
		EXPECT_EQ(0, InputIndex());
		EXPECT_TRUE(Reply().empty());
	}
}

TEST_F(GdxsvReplayInputTest, DeliveryFunctionOwnsTheLiveAvailabilityCheck) {
	DeliverDirectly();
	EXPECT_EQ(0, InputIndex());
	EXPECT_TRUE(Reply().empty());
}

TEST_F(GdxsvReplayInputTest, ForceResumesWithOneAllPlayerBatch) {
	Send(McsMessage::KeyMsg1);
	ASSERT_TRUE(Reply().empty());

	AddInput(FirstInput);
	AddInput(SecondInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);

	Reply().clear();
	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	EXPECT_TRUE(Reply().empty());
}

TEST_F(GdxsvReplayInputTest, PendingReplyBlocksBothMessageTypes) {
	AddInput(FirstInput);
	AddInput(SecondInput);
	Send(McsMessage::KeyMsg1);
	ASSERT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);

	for (bool partial : {false, true}) {
		if (partial)
			Reply().pop_front();
		const auto pending = Reply();
		for (int poll = 0; poll < 10; ++poll) {
			Send(McsMessage::ForceMsg);
			Send(McsMessage::KeyMsg1);
			EXPECT_EQ(1, InputIndex());
			EXPECT_EQ(pending, Reply());
		}
	}

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);
}

TEST_F(GdxsvReplayInputTest, ForceRecoversAfterStreamBecomesOffline) {
	Send(McsMessage::KeyMsg1);
	ASSERT_TRUE(Reply().empty());

	// Final inputs and close can arrive together while the game is stalled.
	AddInput(FirstInput);
	AddInput(SecondInput);
	CloseStream();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);
}

TEST_F(GdxsvReplayInputTest, OfflineKeyAndForceUseTheSameDeliveryPolicy) {
	SetOffline();
	AddInput(FirstInput);
	AddInput(SecondInput);
	// Leave EOF handling to the emulator; this test only exercises delivery.
	AddInput(0);

	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);
}

TEST_F(GdxsvReplayInputTest, TakeoverForceUsesControllerInputOnlyForThePovPlayer) {
	SetOffline();
	AddInput(FirstInput);
	AddInput(SecondInput);
	EnableTakeover(McsKeyCode::A);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	const u64 expected = (FirstInput & ~(0xffffULL << 32)) | (u64(McsKeyCode::A) << 32);
	ExpectBatch(expected);

	Send(McsMessage::ForceMsg);
	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(expected);
}

TEST_F(GdxsvReplayInputTest, TakeoverAlignmentStartsAndCompletesOnFirstAndRepeatedAttempts) {
	const u16 target = McsKeyCode::A;
	for (int attempt = 0; attempt < 3; ++attempt) {
		SCOPED_TRACE(attempt);
		PrepareTakeoverAlignment(attempt > 0, target);

		SendTakeoverInput(0);
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());

		SendTakeoverInput(target);
		ASSERT_FALSE(IsAligningTakeover());
		ASSERT_EQ(60, TakeoverCountdown());
		EXPECT_EQ(attempt > 0, IsTakingOver());
		EXPECT_TRUE(TakeoverInputs().empty());

		for (int frame = 1; frame < 60; ++frame) {
			SendTakeoverInput(target);
			EXPECT_EQ(60 - frame, TakeoverCountdown());
			EXPECT_EQ(static_cast<size_t>(frame), TakeoverInputs().size());
			EXPECT_EQ(0u, PendingControlCommands());
		}
		SendTakeoverInput(target);
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_FALSE(IsAligningTakeover());
		EXPECT_EQ(std::deque<u16>(60, target), TakeoverInputs());
		EXPECT_TRUE(StartTakeoverQueued());
		EXPECT_EQ(1u, PendingControlCommands());

		// Late UI samples must not add inputs or queue another start after
		// the countdown has finished, even before the start command executes.
		SendTakeoverInput(target);
		SendTakeoverInput(0);
		EXPECT_FALSE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_EQ(std::deque<u16>(60, target), TakeoverInputs());
		EXPECT_EQ(1u, PendingControlCommands());
		EXPECT_EQ(attempt > 0, IsTakingOver());
	}
}

TEST_F(GdxsvReplayInputTest, TakeoverCountdownMismatchRequiresMatchingAgainOnFirstAndRetry) {
	for (bool retry : {false, true}) {
		SCOPED_TRACE(retry);
		PrepareTakeoverAlignment(retry, McsKeyCode::A);
		SendTakeoverInput(McsKeyCode::A);
		ASSERT_EQ(60, TakeoverCountdown());
		SendTakeoverInput(McsKeyCode::A);
		ASSERT_EQ(59, TakeoverCountdown());
		ASSERT_EQ(1u, TakeoverInputs().size());

		SendTakeoverInput(0);
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());

		SendTakeoverInput(McsKeyCode::A);
		EXPECT_FALSE(IsAligningTakeover());
		EXPECT_EQ(60, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(retry, IsTakingOver());
	}
}

TEST_F(GdxsvReplayInputTest, TakeoverInputIsIgnoredAfterTheMenuCloses) {
	for (bool retry : {false, true}) {
		SCOPED_TRACE(retry);
		PrepareTakeoverAlignment(retry, McsKeyCode::A);
		CloseTakeoverMenu();
		SendTakeoverInput(McsKeyCode::A);
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}

TEST_F(GdxsvReplayInputTest, SkipTakeoverCountsChangingInputsOnFirstAndRepeatedAttempts) {
	for (int attempt = 0; attempt < 3; ++attempt) {
		SCOPED_TRACE(attempt);
		PrepareTakeoverAlignment(attempt > 0, McsKeyCode::A);
		SkipTakeoverAlignment();
		ASSERT_TRUE(IsSkippingTakeoverAlignment());
		ASSERT_FALSE(IsAligningTakeover());
		ASSERT_EQ(60, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(attempt > 0, IsTakingOver());

		std::deque<u16> expected;
		for (int frame = 1; frame <= 60; ++frame) {
			const u16 input = frame % 2 ? McsKeyCode::B : 0;
			expected.push_back(input);
			SendTakeoverInput(input);
			// Duplicate clicks must not restart or clear the countdown.
			SkipTakeoverAlignment();
			EXPECT_FALSE(IsAligningTakeover());
			EXPECT_EQ(60 - frame, TakeoverCountdown());
			EXPECT_EQ(expected, TakeoverInputs());
			EXPECT_EQ(frame == 60 ? 1u : 0u, PendingControlCommands());
		}
		EXPECT_TRUE(StartTakeoverQueued());
		SendTakeoverInput(McsKeyCode::A);
		SendTakeoverInput(0);
		EXPECT_EQ(expected, TakeoverInputs());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_EQ(1u, PendingControlCommands());
	}
}

TEST_F(GdxsvReplayInputTest, SkipTakeoverAcceptsInputThatAlreadyStartedTheCountdown) {
	for (bool retry : {false, true}) {
		SCOPED_TRACE(retry);
		PrepareTakeoverAlignment(retry, McsKeyCode::A);
		// The UI queues its input sample before the clicked Skip command.
		SendTakeoverInput(McsKeyCode::A);
		ASSERT_EQ(60, TakeoverCountdown());
		SkipTakeoverAlignment();
		ASSERT_TRUE(IsSkippingTakeoverAlignment());
		SendTakeoverInput(0);
		EXPECT_FALSE(IsAligningTakeover());
		EXPECT_EQ(59, TakeoverCountdown());
		EXPECT_EQ(std::deque<u16>{0}, TakeoverInputs());
	}
}

TEST_F(GdxsvReplayInputTest, NewTakeoverAttemptRequiresMatchingAfterSkip) {
	PrepareTakeoverAlignment(false, McsKeyCode::A);
	for (int attempt = 0; attempt < 3; ++attempt) {
		SCOPED_TRACE(attempt);
		SkipTakeoverAlignment();
		SendTakeoverInput(0);
		ASSERT_TRUE(IsSkippingTakeoverAlignment());
		ASSERT_EQ(59, TakeoverCountdown());

		PrepareTakeoverAlignment(true, McsKeyCode::B);
		EXPECT_FALSE(IsSkippingTakeoverAlignment());
		EXPECT_TRUE(TakeoverInputs().empty());
		SendTakeoverInput(0);
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		SendTakeoverInput(McsKeyCode::B);
		EXPECT_EQ(60, TakeoverCountdown());
		SendTakeoverInput(0);
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}

TEST_F(GdxsvReplayInputTest, SkipTakeoverIsIgnoredAfterTheMenuCloses) {
	for (bool retry : {false, true}) {
		SCOPED_TRACE(retry);
		PrepareTakeoverAlignment(retry, McsKeyCode::A);
		CloseTakeoverMenu();
		SkipTakeoverAlignment();
		EXPECT_FALSE(IsSkippingTakeoverAlignment());
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}

TEST_F(GdxsvReplayInputTest, SkipTakeoverIsIgnoredAfterTheMatchingCountdownFinishes) {
	PrepareTakeoverAlignment(false, McsKeyCode::A);
	for (int frame = 0; frame <= 60; ++frame)
		SendTakeoverInput(McsKeyCode::A);
	ASSERT_TRUE(StartTakeoverQueued());
	SkipTakeoverAlignment();
	EXPECT_FALSE(IsSkippingTakeoverAlignment());
	EXPECT_FALSE(IsAligningTakeover());
	EXPECT_EQ(0, TakeoverCountdown());
	EXPECT_EQ(std::deque<u16>(60, McsKeyCode::A), TakeoverInputs());
	EXPECT_EQ(1u, PendingControlCommands());
}

TEST_F(GdxsvReplayInputTest, CancelAndResetClearSkippedTakeover) {
	PrepareTakeoverAlignment(false, McsKeyCode::A);
	SkipTakeoverAlignment();
	SendTakeoverInput(0);
	CancelTakeover();
	SkipTakeoverAlignment();
	EXPECT_FALSE(IsSkippingTakeoverAlignment());
	EXPECT_FALSE(IsAligningTakeover());
	EXPECT_EQ(0, TakeoverCountdown());
	EXPECT_TRUE(TakeoverInputs().empty());
	EXPECT_EQ(0u, PendingControlCommands());

	PrepareTakeoverAlignment(false, McsKeyCode::B);
	SkipTakeoverAlignment();
	SendTakeoverInput(0);
	ResetReplay();
	SkipTakeoverAlignment();
	EXPECT_FALSE(IsSkippingTakeoverAlignment());
	EXPECT_FALSE(IsAligningTakeover());
	EXPECT_EQ(0, TakeoverCountdown());
	EXPECT_TRUE(TakeoverInputs().empty());
	EXPECT_EQ(0u, PendingControlCommands());
}

TEST_F(GdxsvReplayInputTest, SkipTakeoverIsIgnoredInLiveOrInactiveReplay) {
	using State = GdxsvBackendReplay::State;
	for (State state : {State::McsInBattle, State::None, State::End}) {
		SCOPED_TRACE(static_cast<int>(state));
		PrepareTakeoverAlignment(false, McsKeyCode::A);
		SetReplayState(state);
		if (state == State::McsInBattle)
			SetLive();
		SkipTakeoverAlignment();
		EXPECT_FALSE(IsSkippingTakeoverAlignment());
		EXPECT_TRUE(IsAligningTakeover());
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}
