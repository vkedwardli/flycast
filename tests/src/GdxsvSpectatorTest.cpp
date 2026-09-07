#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <thread>

#include "gtest/gtest.h"
#include "gdxsv/gdxsv_network.h"
#include "gdxsv/gdxsv_spectator_downlink.h"
#include "gdxsv/gdxsv_spectator_uplink.h"

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// A loopback LBS that can deliberately drop/reorder packets and ACKs. The
// clients use their real UDP workers, protobuf parsing and application path.
class SpectatorSocket {
public:
	~SpectatorSocket() {
		if (socket_ != INVALID_SOCKET) closesocket(socket_);
#ifdef _WIN32
		if (winsock_started_) WSACleanup();
#endif
	}
	bool Open() {
#ifdef _WIN32
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
		winsock_started_ = true;
#endif
		socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (socket_ == INVALID_SOCKET) return false;
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(socket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) return false;
		socklen_t size = sizeof(addr);
		if (getsockname(socket_, reinterpret_cast<sockaddr *>(&addr), &size) != 0) return false;
		port = ntohs(addr.sin_port);
		set_non_blocking(socket_);
		return true;
	}
	bool Receive(proto::MessageType type, proto::Packet *packet, int timeout_ms = 3000) {
		const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
		while (Clock::now() < deadline) {
			char buf[8192];
			peer_size = sizeof(peer);
			int n = static_cast<int>(recvfrom(socket_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&peer), &peer_size));
			if (n > 0 && packet->ParseFromArray(buf, n) && packet->type() == type) return true;
			std::this_thread::sleep_for(1ms);
		}
		return false;
	}
	void Send(const proto::Packet &packet) {
		const auto bytes = packet.SerializeAsString();
		ASSERT_EQ(static_cast<int>(bytes.size()), sendto(socket_, bytes.data(), static_cast<int>(bytes.size()), 0,
			reinterpret_cast<sockaddr *>(&peer), peer_size));
	}
	void Push(const proto::SpectatorInputPush &push) {
		proto::Packet packet;
		packet.set_type(proto::SpectatorInputPushType);
		*packet.mutable_spectator_input_push_data() = push;
		Send(packet);
	}
	void Ack(const proto::SpectatorInputAck &ack) {
		proto::Packet packet;
		packet.set_type(proto::SpectatorInputAckType);
		*packet.mutable_spectator_input_ack_data() = ack;
		Send(packet);
	}
	bool WaitAck(int frame, int round) {
		const auto deadline = Clock::now() + 3s;
		proto::Packet packet;
		while (Clock::now() < deadline) {
			if (!Receive(proto::SpectatorInputAckType, &packet, 100)) continue;
			const auto &ack = packet.spectator_input_ack_data();
			if (ack.ack_frame() == frame && ack.round_ack() == round) return true;
		}
		return false;
	}
	int port = 0;
	sockaddr_storage peer{};
	socklen_t peer_size = 0;
private:
	sock_t socket_ = INVALID_SOCKET;
#ifdef _WIN32
	bool winsock_started_ = false;
#endif
};

template<class Predicate>
bool DrainUntil(GdxsvSpectatorDownlink &client, proto::BattleLogFile &log, Predicate done) {
	const auto deadline = Clock::now() + 3s;
	while (Clock::now() < deadline) {
		client.DrainInto(&log);
		client.ReportAcked(log.inputs_size());
		if (done()) return true;
		std::this_thread::sleep_for(1ms);
	}
	return false;
}

TEST(GdxsvSpectator, AppliesResultOnlyUpdatesAndAcksAfterApplication) {
	SpectatorSocket server;
	ASSERT_TRUE(server.Open());
	GdxsvSpectatorDownlink client;
	client.Start("127.0.0.1", server.port, "rounds", 0);
	proto::Packet packet;
	ASSERT_TRUE(server.Receive(proto::SpectatorSubscribeType, &packet));
	proto::SpectatorInputPush header;
	header.set_battle_code("rounds");
	header.mutable_header()->set_battle_code("rounds");
	server.Push(header); // Zero patches must complete bootstrap.
	proto::BattleLogFile log;
	ASSERT_TRUE(client.WaitForBootstrap(&log, 1000));
	ASSERT_TRUE(server.WaitAck(0, 0));

	proto::SpectatorInputPush start;
	start.set_battle_code("rounds");
	start.set_round_state_version(1);
	start.add_start_msg_indexes(0);
	start.add_start_msg_randoms(123);
	start.add_round_data();
	server.Push(start);
	ASSERT_TRUE(server.WaitAck(0, 0)); // Received, but not yet applied.
	ASSERT_TRUE(DrainUntil(client, log, [&] { return log.start_msg_indexes_size() == 1; }));
	ASSERT_TRUE(server.WaitAck(0, 1));

	auto result = start;
	result.set_round_state_version(2);
	result.mutable_round_data(0)->set_win_team(2);
	result.mutable_round_data(0)->add_used_ms(3);
	server.Push(result);
	ASSERT_TRUE(server.WaitAck(0, 1)); // Queuing a final result must not ACK version 2.
	server.Push(start); // Delayed older snapshot must not erase the result.
	ASSERT_TRUE(DrainUntil(client, log, [&] { return log.round_data(0).win_team() == 2; }));
	ASSERT_TRUE(server.WaitAck(0, 2));
	server.Push(result); // Lost ACK: a duplicate should trigger another ACK.
	ASSERT_TRUE(server.WaitAck(0, 2));
	client.DrainInto(&log);
	EXPECT_EQ(2, log.round_data(0).win_team());
	EXPECT_EQ(3, log.round_data(0).used_ms(0));

	proto::SpectatorInputPush close;
	close.set_battle_code("rounds");
	close.set_close_reason("game_end");
	close.set_round_state_version(3); // Premature close must not discard missing round state.
	server.Push(close);
	ASSERT_TRUE(server.WaitAck(0, 2));
	client.DrainInto(&log);
	EXPECT_TRUE(log.close_reason().empty());
	close.set_round_state_version(2);
	server.Push(close);
	ASSERT_TRUE(DrainUntil(client, log, [&] { return !log.close_reason().empty(); }));
}

TEST(GdxsvSpectator, ClosedBootstrapReceivesCompleteFiveRoundRecording) {
	proto::BattleLogFile recording;
	if (const char *path = std::getenv("GDXSV_SPECTATOR_TEST_REPLAY")) {
		std::ifstream file(path, std::ios::binary);
		ASSERT_TRUE(recording.ParseFromIstream(&file));
	} else {
		recording.set_battle_code("closed");
		for (int i = 0; i < 4; ++i) recording.add_users()->set_user_id(std::to_string(i));
		for (int i = 0; i < 33499; ++i) recording.add_inputs(i + 1);
		for (int i = 0; i < 5; ++i) {
			recording.add_start_msg_indexes(i * 6000);
			recording.add_start_msg_randoms(i + 123);
			recording.add_round_data()->set_win_team(i % 2 + 1);
		}
		recording.set_close_reason("game_end");
	}
	ASSERT_EQ(4, recording.users_size());
	ASSERT_EQ(5, recording.round_data_size());
	ASSERT_EQ(33499, recording.inputs_size());
	SpectatorSocket server;
	ASSERT_TRUE(server.Open());
	GdxsvSpectatorDownlink client;
	client.Start("127.0.0.1", server.port, recording.battle_code(), 0);
	proto::Packet packet;
	ASSERT_TRUE(server.Receive(proto::SpectatorSubscribeType, &packet));
	proto::SpectatorInputPush push;
	push.set_battle_code(recording.battle_code());
	*push.mutable_header() = recording;
	push.mutable_header()->clear_inputs();
	push.mutable_header()->clear_start_msg_indexes();
	push.mutable_header()->clear_start_msg_randoms();
	push.mutable_header()->clear_round_data();
	push.mutable_header()->clear_patches();
	push.set_patch_total(recording.patches_size());
	server.Push(push); // Also verifies defensive stripping of header close fields.
	push.clear_header();
	for (int i = 0; i < recording.patches_size(); ++i) {
		push.set_patch_start(i);
		*push.add_patches() = recording.patches(i);
		server.Push(push);
		push.clear_patches();
	}
	proto::BattleLogFile log;
	ASSERT_TRUE(client.WaitForBootstrap(&log, 1000));
	EXPECT_TRUE(log.close_reason().empty());
	push.set_round_state_version(10);
	*push.mutable_start_msg_indexes() = recording.start_msg_indexes();
	*push.mutable_start_msg_randoms() = recording.start_msg_randoms();
	*push.mutable_round_data() = recording.round_data();
	for (int frame = 0; frame < recording.inputs_size();) {
		push.set_start_frame(frame);
		push.clear_inputs();
		for (int i = 0; i < 128 && frame < recording.inputs_size(); ++i, ++frame) {
			push.add_inputs(recording.inputs(frame));
		}
		server.Push(push);
		ASSERT_TRUE(DrainUntil(client, log, [&] { return log.inputs_size() == frame; }));
		ASSERT_TRUE(server.WaitAck(frame, 10));
		ASSERT_TRUE(log.close_reason().empty());
	}
	push.clear_inputs();
	push.clear_start_msg_indexes();
	push.clear_start_msg_randoms();
	push.clear_round_data();
	push.set_start_frame(recording.inputs_size());
	push.set_close_reason(recording.close_reason());
	push.set_disconnect_user_index(recording.disconnect_user_index());
	server.Push(push);
	ASSERT_TRUE(DrainUntil(client, log, [&] { return !log.close_reason().empty(); }));
	EXPECT_EQ(recording.SerializeAsString(), log.SerializeAsString());
}

TEST(GdxsvSpectator, UplinkRetriesRoundsInOrderAndDrainsFinalResult) {
	SpectatorSocket server, wrong_sender;
	ASSERT_TRUE(server.Open());
	ASSERT_TRUE(wrong_sender.Open());
	GdxsvSpectatorUplink client;
	client.Start("127.0.0.1", server.port, "uplink", 42, false);
	client.PushRoundEvent(0, 123);
	client.PushRoundEvent(500, 456);
	proto::Packet packet;
	for (int attempt = 0; attempt < 2; ++attempt) {
		ASSERT_TRUE(server.Receive(proto::SpectatorRoundEventType, &packet));
		EXPECT_EQ(0, packet.spectator_round_event_data().frame());
		EXPECT_EQ(123u, packet.spectator_round_event_data().random_value());
	}
	proto::SpectatorInputAck ack;
	ack.set_battle_code("uplink");
	ack.add_round_event_ack(0);
	wrong_sender.peer = server.peer;
	wrong_sender.peer_size = server.peer_size;
	wrong_sender.Ack(ack); // A different UDP sender must not retire the first start.
	ASSERT_TRUE(server.Receive(proto::SpectatorRoundEventType, &packet));
	EXPECT_EQ(0, packet.spectator_round_event_data().frame());
	server.Ack(ack);
	ASSERT_TRUE(server.Receive(proto::SpectatorRoundEventType, &packet));
	EXPECT_EQ(500, packet.spectator_round_event_data().frame());
	ack.set_round_event_ack(0, 500);
	server.Ack(ack);

	client.PushInput(0, 999);
	client.PushRoundResult(0, 2, {1, 2, 3, 4});
	ASSERT_TRUE(server.Receive(proto::SpectatorRoundResultType, &packet)); // Drop this result.
	auto stopped = std::async(std::launch::async, [&] { client.Stop(true); });
	ASSERT_TRUE(server.Receive(proto::SpectatorRoundResultType, &packet));
	EXPECT_EQ(2, packet.spectator_round_result_data().round().win_team());
	// Drop its ACK as well: final drain must keep retrying without new input.
	ASSERT_TRUE(server.Receive(proto::SpectatorRoundResultType, &packet));
	ack.clear_round_event_ack();
	ack.add_round_result_ack(0);
	ack.set_ack_frame(100); // The shared log may be ahead via another participant.
	server.Ack(ack);
	ASSERT_EQ(std::future_status::ready, stopped.wait_for(1s));
	stopped.get();
}

TEST(GdxsvSpectator, FinalDrainIsBoundedWhenServerDoesNotAck) {
	SpectatorSocket server;
	ASSERT_TRUE(server.Open());
	GdxsvSpectatorUplink client;
	client.Start("127.0.0.1", server.port, "timeout", 42, false);
	client.PushRoundResult(0, 1, {});
	proto::Packet packet;
	ASSERT_TRUE(server.Receive(proto::SpectatorRoundResultType, &packet));
	const auto before = Clock::now();
	client.Stop(true);
	EXPECT_GE(Clock::now() - before, 1900ms);
	EXPECT_LT(Clock::now() - before, 3s);
}

TEST(GdxsvSpectator, WorkersCanStopAndRestartOnAnotherBattle) {
	SpectatorSocket server;
	ASSERT_TRUE(server.Open());
	GdxsvSpectatorDownlink downlink;
	GdxsvSpectatorUplink uplink;
	proto::Packet packet;
	for (int i = 0; i < 20; ++i) {
		const auto battle = "restart-" + std::to_string(i);
		downlink.Start("127.0.0.1", server.port, battle, 0);
		ASSERT_TRUE(server.Receive(proto::SpectatorSubscribeType, &packet));
		EXPECT_EQ(battle, packet.spectator_subscribe_data().battle_code());
		downlink.Stop();
		uplink.Start("127.0.0.1", server.port, battle, 42, false);
		uplink.PushRoundEvent(0, i);
		ASSERT_TRUE(server.Receive(proto::SpectatorRoundEventType, &packet));
		EXPECT_EQ(battle, packet.spectator_round_event_data().battle_code());
		uplink.Stop();
	}
}
} // namespace
