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
