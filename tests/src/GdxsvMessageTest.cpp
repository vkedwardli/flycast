#include "gtest/gtest.h"
#include "gdxsv/lbs_message.h"
#include "gdxsv/mcs_message.h"

TEST(GdxsvLbsMessageTest, ReadsBigEndianFields) {
	LbsMessage msg;
	msg.Write8(0x12)->Write16(0x3456)->Write32(0x789abcde);
	EXPECT_EQ(0x12, msg.Read8());
	EXPECT_EQ(0x3456, msg.Read16());
	EXPECT_EQ(0x789abcdeu, msg.Read32());
}

TEST(GdxsvLbsMessageTest, MessageSizeOfBodyWithHighBitLengthBytes) {
	LbsMessage msg;
	msg.body.assign(0x80ff, 0);
	std::vector<u8> buf;
	msg.Serialize(buf);
	ASSERT_EQ(LbsMessage::HeaderSize + 0x80ff, (int)buf.size());
	EXPECT_EQ((int)buf.size(), LbsMessage::MessageSize(reinterpret_cast<const char *>(buf.data()), buf.size()));
	EXPECT_EQ(0, LbsMessage::MessageSize(reinterpret_cast<const char *>(buf.data()), buf.size() - 1));
}

TEST(GdxsvLbsMessageTest, ClientNoticeGoesToTheServer) {
	EXPECT_EQ(LbsMessage::ClientToServer, LbsMessage::ClNotice(LbsMessage::lbsP2PMatchingReport).direction);
	EXPECT_EQ(LbsMessage::ServerToClient, LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).direction);
}

TEST(GdxsvMcsMessageTest, ConnectionIdWaitsForAllTwentyBytes) {
	McsMessageReader r;
	McsMessage msg;
	const char head[] = {'\x82', '\x02', 0, 0, 0, 0, 0, 0};
	r.Write(head, sizeof(head));
	EXPECT_FALSE(r.Read(msg));
	const char rest[12] = {};
	r.Write(rest, sizeof(rest));
	ASSERT_TRUE(r.Read(msg));
	EXPECT_EQ(McsMessage::ConnectionIdMsg, msg.Type());
	EXPECT_EQ(20u, msg.body.size());
}

TEST(GdxsvMcsMessageTest, ZeroLengthByteDoesNotStallTheStream) {
	McsMessageReader r;
	McsMessage msg;
	r.Write("\0", 1);
	const auto start = McsMessage::Create(McsMessage::StartMsg, 2);
	r.Write(reinterpret_cast<const char *>(start.body.data()), start.body.size());
	ASSERT_TRUE(r.Read(msg));
	EXPECT_EQ(McsMessage::UnknownMsg, msg.Type());
	ASSERT_TRUE(r.Read(msg));
	EXPECT_EQ(McsMessage::StartMsg, msg.Type());
	EXPECT_EQ(2, msg.Sender());
	EXPECT_FALSE(r.Read(msg));
}
