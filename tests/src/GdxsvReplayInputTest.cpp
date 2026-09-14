#include "gdxsv/gdxsv_backend_replay.h"

#include "cfg/option.h"
#include "emulator.h"
#include "gdxsv/gdxsv_emu_hooks.h"
#include "gdxsv/libs.h"
#include "gtest/gtest.h"

class GdxsvReplayInputTest : public ::testing::Test {
 protected:
	using Command = GdxsvBackendReplay::ReplayCtrlCommand;

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
		gdxsv_frame_period_trim_us = frame_period_trim_;
	}

	void AddInput(u64 input) { replay_.log_file_.add_inputs(input); }
	void SetOffline() { replay_.live_mode_ = false; }
	int InputIndex() const { return replay_.key_msg_count_; }
	std::deque<u8>& Reply() { return replay_.recv_buf_; }
	void DeliverDirectly() { replay_.DeliverKeyMsgBatch(); }
	void SetPlaybackFrame(int frame) { replay_.key_msg_count_ = frame; }
	void SetPlaybackSpeed(int speed) { replay_.ctrl_play_speed_ = speed; }
	void SetPaused(bool paused) { replay_.ctrl_pause_ = paused; }
	void SetFollowing(bool following) { replay_.live_following_ = following; }
	void SetRoundCount(int round) { replay_.start_msg_count_ = round; }
	void SetLiveBuffer(int frames) { replay_.live_buffer_frames_ = frames; }
	void SetDownloadedInputs(int count) { replay_.log_file_.mutable_inputs()->Resize(count, FirstInput); }
	void BeginLoading() { replay_.BeginLoadingHud(); }
	bool IsControlLoading() const { return replay_.ctrl_loading_; }
	GdxsvReplayUiState PublishedUi() {
		replay_.PublishUiState();
		return replay_.ui_snapshot_.Read();
	}
	void SetSteadyPlayback() {
		replay_.seeking_ = false;
		replay_.live_catching_up_ = false;
	}
	void NewDeliveryFrame() { replay_.deliver_last_mainui_ = 0xffffffffu; }
	bool IsFollowing() const { return replay_.live_following_; }
	bool IsAtLiveEdge() const { return replay_.live_at_edge_; }
	bool IsCatchingUp() const { return replay_.live_catching_up_; }
	bool IsPaused() const { return replay_.ctrl_pause_; }
	int PlaybackSpeed() const { return replay_.ctrl_play_speed_; }
	const char* DisplayedSpeed() { return GdxsvBackendReplay::SpeedText(PublishedUi().playSpeed); }
	void UpdatePacing() { replay_.UpdateFramePacing(); }
	void QueueUi(Command::Command cmd, int arg = 0) {
		replay_.ui_commands_.emplace_back(cmd, arg);
		replay_.ProcessUiCommands();
	}
	void PrepareAutoRecovery() {
		replay_.ctrl_commands_.clear();
		replay_.live_following_ = true;
		replay_.live_catching_up_ = true;
		replay_.live_at_edge_ = true;
		replay_.ctrl_play_speed_ = 2;
		replay_.ctrl_loading_ = true;
		gdxsv_frame_period_trim_us = -4000;
	}
	void PrepareFrameControl() {
		ASSERT_TRUE(addrspace::reserve());
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		gdxsv_save_state.Reset();
		gdxsv_WriteMem8(0x0c3d16d4, 2);
		gdxsv_WriteMem8(0x0c3d16d5, 7);
		for (int i = 0; i < 40; ++i)
			AddInput(FirstInput);
		SetPlaybackFrame(10); // Below the automatic save interval.
		SetSteadyPlayback();
		replay_.start_msg_count_ = 1;
	}
	void RunFrameControl() {
		replay_.end_of_frame_ = true;
		replay_.OnNextFrameInternal();
	}
	void QueueAutoSeek() { replay_.ctrl_commands_.emplace_back(Command::SeekForward, 1000, 1); }

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
	const int frame_period_trim_ = gdxsv_frame_period_trim_us;
};

TEST_F(GdxsvReplayInputTest, ManualActionsDetachAndCancelAutomaticSpeed) {
	for (auto cmd : {Command::SeekBackward, Command::StepFrameBackward, Command::JumpToKeyMsg,
		Command::SetRound, Command::NextRound, Command::TogglePause, Command::TogglePauseMenu,
		Command::SetSpeed, Command::NextSpeed}) {
		SCOPED_TRACE(cmd);
		PrepareAutoRecovery();
		QueueUi(cmd, 1);
		EXPECT_FALSE(IsFollowing());
		EXPECT_FALSE(IsCatchingUp());
		EXPECT_FALSE(IsAtLiveEdge());
		EXPECT_EQ(0, PlaybackSpeed());
		UpdatePacing();
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
}

TEST_F(GdxsvReplayInputTest, RewindingPreservesManualSpeedWithoutLivePacing) {
	SetFollowing(false);
	for (int speed : {-2, -1, 0, 1, 2}) {
		SetPlaybackSpeed(speed);
		QueueUi(Command::SeekBackward);
		EXPECT_EQ(speed, PlaybackSpeed());
		gdxsv_frame_period_trim_us = -4000;
		UpdatePacing();
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
}

TEST_F(GdxsvReplayInputTest, RewoundPlaybackDeliversBufferedInputsWithoutWaitingForRendering) {
	for (int i = 0; i < 12000; ++i)
		AddInput(i % 2 == 0 ? FirstInput : SecondInput);
	SetSteadyPlayback();
	for (auto cmd : {Command::SeekBackward, Command::JumpToKeyMsg}) {
		SCOPED_TRACE(cmd);
		SetFollowing(true);
		SetPlaybackFrame(9000);
		QueueUi(cmd, 9000);
		ASSERT_FALSE(IsFollowing());
		ASSERT_FALSE(IsCatchingUp());
		// The game may request another batch before the host renders again.
		// Once the previous reply drains, buffered inputs must remain usable.
		for (int i = 0; i < 3; ++i) {
			Send(i % 2 == 0 ? McsMessage::KeyMsg1 : McsMessage::ForceMsg);
			EXPECT_EQ(9001 + i, InputIndex());
			ExpectBatch(i % 2 == 0 ? FirstInput : SecondInput);
			Reply().clear();
		}
		EXPECT_FALSE(IsFollowing());
		EXPECT_EQ(0, PlaybackSpeed());
		UpdatePacing();
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
}

TEST_F(GdxsvReplayInputTest, DetachedPlaybackStillChecksPendingRepliesAndInputAvailability) {
	SetSteadyPlayback();
	SetFollowing(false);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(0, InputIndex());
	EXPECT_TRUE(Reply().empty());

	AddInput(FirstInput);
	AddInput(SecondInput);
	Send(McsMessage::KeyMsg1);
	ExpectBatch(FirstInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);
	Reply().pop_front();
	const auto partial = Reply();
	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(1, InputIndex());
	EXPECT_EQ(partial, Reply());

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);
	Reply().clear();
	Send(McsMessage::KeyMsg1);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	EXPECT_TRUE(Reply().empty());

	AddInput(FirstInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(3, InputIndex());
	ExpectBatch(FirstInput);
	EXPECT_FALSE(IsFollowing());
}

TEST_F(GdxsvReplayInputTest, ResumingLiveRestoresTheInputRecoveryCap) {
	for (int i = 0; i < 128; ++i)
		AddInput(FirstInput);
	SetSteadyPlayback();
	SetFollowing(false);
	QueueUi(Command::FollowLive);
	ASSERT_TRUE(IsFollowing());
	NewDeliveryFrame();
	DeliverDirectly();
	Reply().clear();
	DeliverDirectly();
	EXPECT_EQ(2, InputIndex());
	Reply().clear();
	DeliverDirectly();
	EXPECT_EQ(2, InputIndex());
	EXPECT_TRUE(Reply().empty());

	// Close to the live edge, the normal one-input allowance still applies.
	SetPlaybackFrame(100);
	NewDeliveryFrame();
	DeliverDirectly();
	EXPECT_EQ(101, InputIndex());
	Reply().clear();
	DeliverDirectly();
	EXPECT_EQ(101, InputIndex());
	EXPECT_TRUE(Reply().empty());
}

TEST_F(GdxsvReplayInputTest, ReachingTheEdgeDoesNotResumeFollowing) {
	PrepareFrameControl();
	SetFollowing(false);
	for (int frame : {10, 30, 40}) {
		SetPlaybackFrame(frame);
		RunFrameControl();
		EXPECT_FALSE(IsFollowing());
		EXPECT_FALSE(IsAtLiveEdge());
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
	SetPaused(true);
	SetPlaybackSpeed(-1);
	QueueUi(Command::FollowLive);
	EXPECT_TRUE(IsFollowing());
	EXPECT_FALSE(IsPaused());
	EXPECT_EQ(0, PlaybackSpeed());
	RunFrameControl();
	EXPECT_TRUE(IsAtLiveEdge());
}

TEST_F(GdxsvReplayInputTest, LiveIndicatorUsesSeparateEnterAndLeaveThresholds) {
	PrepareFrameControl();
	// Exercise the indicator without launching an automatic emulation seek.
	SetRoundCount(0);
	for (int buffer : {10, 30, 120}) {
		SCOPED_TRACE(buffer);
		SetLiveBuffer(buffer);
		SetFollowing(false);
		RunFrameControl();
		SetFollowing(true);
		auto check = [&](int gap, bool at_edge) {
			SCOPED_TRACE(gap);
			SetDownloadedInputs(InputIndex() + gap);
			RunFrameControl();
			EXPECT_EQ(at_edge, IsAtLiveEdge());
			EXPECT_TRUE(IsFollowing());
			EXPECT_EQ(0, PlaybackSpeed());
			EXPECT_EQ(0u, PendingControlCommands());
		};
		check(buffer + 61, false);
		check(buffer + 60, true);
		check(buffer + 61, true);
		check(buffer + 60 + 128, true); // A full input packet must not undo red.
		check(buffer + 269, true);
		check(buffer + 270, false);
		check(buffer + 269, false);
		check(buffer + 61, false);
		check(buffer + 60, true);
	}
}

TEST_F(GdxsvReplayInputTest, ClickingLiveAcceptsAShortGapImmediately) {
	PrepareFrameControl();
	SetRoundCount(0); // Test feedback without running an emulation seek.
	for (int buffer : {10, 30, 120}) {
		SCOPED_TRACE(buffer);
		SetLiveBuffer(buffer);
		for (int gap : {0, buffer + 60, buffer + 120, buffer + 269}) {
			SCOPED_TRACE(gap);
			SetFollowing(false);
			SetDownloadedInputs(InputIndex() + gap);
			RunFrameControl();
			ASSERT_FALSE(IsAtLiveEdge());
			SetPaused(true);
			SetPlaybackSpeed(-1);

			QueueUi(Command::FollowLive);
			EXPECT_TRUE(IsFollowing());
			EXPECT_FALSE(IsPaused());
			EXPECT_EQ(0, PlaybackSpeed());
			EXPECT_TRUE(PublishedUi().liveAtEdge);
			RunFrameControl();
			EXPECT_TRUE(IsAtLiveEdge());
			EXPECT_EQ(0u, PendingControlCommands());
		}
	}
}

TEST_F(GdxsvReplayInputTest, ClickingLiveKeepsLargeOrUndownloadedGapsGrey) {
	PrepareFrameControl();
	SetRoundCount(0);
	for (int buffer : {10, 30, 120}) {
		SCOPED_TRACE(buffer);
		SetLiveBuffer(buffer);
		for (int gap : {-1, buffer + 270, buffer + 1000}) {
			SCOPED_TRACE(gap);
			SetFollowing(false);
			SetDownloadedInputs(InputIndex() + gap);
			RunFrameControl();

			QueueUi(Command::FollowLive);
			EXPECT_TRUE(IsFollowing());
			EXPECT_FALSE(PublishedUi().liveAtEdge);
			RunFrameControl();
			EXPECT_FALSE(IsAtLiveEdge());
		}
	}
}

TEST_F(GdxsvReplayInputTest, LiveIndicatorDoesNotTreatMissingDownloadedInputsAsTheLiveEdge) {
	PrepareFrameControl();
	SetRoundCount(0);
	SetDownloadedInputs(InputIndex() - 1);
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
	SetDownloadedInputs(InputIndex() + 30);
	RunFrameControl();
	ASSERT_TRUE(IsAtLiveEdge());

	// A round jump ahead of the download must also clear an already-red pill.
	SetPlaybackFrame(100);
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
	EXPECT_TRUE(IsFollowing());
	SetDownloadedInputs(InputIndex() + 30);
	RunFrameControl();
	ASSERT_TRUE(IsAtLiveEdge());
	SetOffline();
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
}

TEST_F(GdxsvReplayInputTest, PausingCancelsAnAlreadyQueuedAutomaticSeek) {
	PrepareFrameControl();
	PrepareAutoRecovery();
	QueueAutoSeek();
	QueueUi(Command::TogglePause);
	RunFrameControl();
	EXPECT_FALSE(IsFollowing());
	EXPECT_TRUE(IsPaused());
	EXPECT_EQ(10, InputIndex());
	EXPECT_EQ(0u, PendingControlCommands());
}

TEST_F(GdxsvReplayInputTest, LiveFollowingDisplaysNominalSpeedWithoutChangingRecoverySpeed) {
	PrepareFrameControl();
	for (int speed : {0, 1, 2}) {
		SetPlaybackSpeed(speed);
		EXPECT_STREQ("100%", DisplayedSpeed());
		EXPECT_EQ(speed, PlaybackSpeed());
	}
	// Manual live and offline replay speeds retain their fixed labels.
	const char* labels[] = {"33%", "50%", "100%", "200%", "300%"};
	SetFollowing(false);
	for (int speed = -2; speed <= 2; ++speed) {
		SetPlaybackSpeed(speed);
		EXPECT_STREQ(labels[speed + 2], DisplayedSpeed());
	}
	SetOffline();
	SetFollowing(true);
	for (int speed = -2; speed <= 2; ++speed) {
		SetPlaybackSpeed(speed);
		EXPECT_STREQ(labels[speed + 2], DisplayedSpeed());
	}
}

TEST_F(GdxsvReplayInputTest, LiveRoundJumpShowsLoadingUntilTheDownloadReachesTheCursor) {
	PrepareFrameControl(); // Starts with 40 downloaded inputs.
	for (int i = 40; i < 30848; ++i)
		AddInput(FirstInput);
	SetPlaybackFrame(34226);
	const auto buffering = PublishedUi();
	EXPECT_EQ(34226, buffering.playbackFrame);
	EXPECT_EQ(30848, buffering.inputCount);
	EXPECT_TRUE(buffering.loading);
	EXPECT_FALSE(IsControlLoading());
	EXPECT_EQ(0u, PendingControlCommands());

	for (int i = 30848; i < 34226; ++i)
		AddInput(FirstInput);
	// Waiting for the next live input must not flash the loading HUD.
	EXPECT_FALSE(PublishedUi().loading);
	AddInput(FirstInput);
	EXPECT_FALSE(PublishedUi().loading);
	DeliverDirectly();
	EXPECT_EQ(34227, InputIndex());
	EXPECT_FALSE(PublishedUi().loading);
	EXPECT_TRUE(buffering.loading); // Published snapshots stay independent.
}

TEST_F(GdxsvReplayInputTest, DownloadLoadingDoesNotAffectOfflineReplayOrExplicitSeekLoading) {
	PrepareFrameControl();
	SetPlaybackFrame(100);
	ASSERT_TRUE(PublishedUi().loading);
	SetOffline();
	EXPECT_FALSE(PublishedUi().loading);
	SetLive();
	SetReplayState(GdxsvBackendReplay::State::End);
	EXPECT_FALSE(PublishedUi().loading);

	SetReplayState(GdxsvBackendReplay::State::McsInBattle);
	SetPlaybackFrame(10);
	BeginLoading();
	EXPECT_TRUE(PublishedUi().loading);
	EXPECT_TRUE(IsControlLoading());
	SetOffline();
	EXPECT_TRUE(PublishedUi().loading);
}

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
