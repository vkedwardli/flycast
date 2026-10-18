#include "gtest/gtest.h"
#include "gdxsv/gdxsv_network.h"

namespace {
constexpr int N = UdpPingPong::N;
constexpr int R = UdpPingPong::MAX_RELAYS;
}  // namespace

// Peer 1 is behind a symmetric NAT: peer 0 never hears from it, only from peer 3, which carries peer 1's row.
TEST(GdxsvPingPongTest, MergeRelayRttTakesRowsOfPeersNotReachedDirectly) {
	uint8_t mine[N][R] = {{18, 67}};
	const uint8_t from3[N][R] = {{17, 66}, {4, 55}, {60, 5}, {53, 4}};
	UdpPingPong::MergeRelayRtt(mine, from3, 0, 3);
	EXPECT_EQ(18, mine[0][0]);  // our own row stays as we measured it
	EXPECT_EQ(67, mine[0][1]);
	EXPECT_EQ(4, mine[1][0]);
	EXPECT_EQ(55, mine[1][1]);
	EXPECT_EQ(60, mine[2][0]);
	EXPECT_EQ(53, mine[3][0]);
}

TEST(GdxsvPingPongTest, MergeRelayRttKeepsKnownRowsAgainstUnknown) {
	uint8_t mine[N][R] = {{18}, {4}, {60}, {53}};
	const uint8_t from2[N][R] = {{0}, {0}, {61}, {0}};
	UdpPingPong::MergeRelayRtt(mine, from2, 0, 2);
	EXPECT_EQ(4, mine[1][0]);  // peer 2 does not know peer 1: keep what peer 3 told us
	EXPECT_EQ(61, mine[2][0]);
	EXPECT_EQ(53, mine[3][0]);
}

TEST(GdxsvPingPongTest, MergeRelayRttTakesSendersOwnRowAsIs) {
	uint8_t mine[N][R] = {{18}, {0}, {0}, {53}};
	const uint8_t from3[N][R] = {{0}, {0}, {0}, {0}};
	UdpPingPong::MergeRelayRtt(mine, from3, 0, 3);
	EXPECT_EQ(0, mine[3][0]);  // the sender lost its relay
	EXPECT_EQ(18, mine[0][0]);
}

// An ack past the last pushed message (a stale or bogus packet) must drop only what is buffered.
TEST(GdxsvMessageBufferTest, AckPastLastMessageClearsBuffer) {
	MessageBuffer empty;
	empty.ApplySeqAck(0, 5);
	EXPECT_EQ(0, empty.Packet().battle_data_size());

	MessageBuffer buf;
	u8 body[] = {1, 2, 3};
	ASSERT_TRUE(buf.PushBattleMessage("u", body, sizeof(body)));
	ASSERT_TRUE(buf.PushBattleMessage("u", body, sizeof(body)));
	buf.ApplySeqAck(0, 10);
	EXPECT_EQ(0, buf.Packet().battle_data_size());

	ASSERT_TRUE(buf.PushBattleMessage("u", body, sizeof(body)));
	ASSERT_EQ(1, buf.Packet().battle_data_size());
	EXPECT_EQ(3u, buf.Packet().battle_data(0).seq());
	buf.ApplySeqAck(0, 3);
	EXPECT_EQ(0, buf.Packet().battle_data_size());
}

TEST(GdxsvMessageBufferTest, AckDropsOnlyAckedMessages) {
	MessageBuffer buf;
	u8 body[] = {1};
	for (int i = 0; i < 3; i++) ASSERT_TRUE(buf.PushBattleMessage("u", body, sizeof(body)));
	buf.ApplySeqAck(7, 2);
	ASSERT_EQ(1, buf.Packet().battle_data_size());
	EXPECT_EQ(3u, buf.Packet().battle_data(0).seq());
	EXPECT_EQ(7u, buf.Packet().ack());
	buf.ApplySeqAck(0, 1);  // an old ack changes nothing
	EXPECT_EQ(1, buf.Packet().battle_data_size());
}

#ifdef _WIN32
// A failed connect must not leak its socket: the lobby client reconnects in a loop.
TEST(GdxsvTcpClientTest, FailedConnectClosesSocket) {
	WSADATA wsa;
	ASSERT_EQ(0, WSAStartup(MAKEWORD(2, 2), &wsa));
	// A port nobody listens on: bind it, then close it.
	sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(0, ::bind(s, (sockaddr *)&addr, sizeof(addr)));
	socklen_t len = sizeof(addr);
	ASSERT_EQ(0, getsockname(s, (sockaddr *)&addr, &len));
	closesocket(s);
	const int port = ntohs(addr.sin_port);

	TcpClient client;
	ASSERT_FALSE(client.Connect("127.0.0.1", port));  // warm up winsock's own handles
	DWORD before = 0, after = 0;
	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (int i = 0; i < 2; i++) EXPECT_FALSE(client.Connect("127.0.0.1", port));
	GetProcessHandleCount(GetCurrentProcess(), &after);
	EXPECT_EQ(before, after);
	WSACleanup();
}
#endif
