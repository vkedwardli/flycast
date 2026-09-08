#pragma once

#include <algorithm>
#include <array>
#include <string>
#include "lbs_message.h"

namespace gdxsv_win_lose32 {

struct Record {
	u32 wins;
	u32 losses;
	u32 draws;
	u32 invalid;
};

inline bool IsRequest(const LbsMessage& msg) {
	return msg.direction == LbsMessage::ClientToServer && msg.category == LbsMessage::CategoryQuestion &&
		msg.command == LbsMessage::lbsWinLose && msg.body.size() == 1;
}

// The personal panel has one outstanding request. A new category invalidates
// the previous reply; categories other than zero keep the legacy path.
class Requests {
	bool active_ = false;
	u16 seq_ = 0;

public:
	void Clear() { active_ = false; }
	bool Rewrite(LbsMessage& msg) {
		if (!IsRequest(msg))
			return false;
		Clear();
		if (msg.body[0] != 0)
			return false;
		active_ = true;
		seq_ = msg.seq;
		msg.command = LbsMessage::lbsWinLose32;
		return true;
	}

	bool TakeReply(const LbsMessage& msg) {
		if (!active_ || msg.seq != seq_ || msg.direction != LbsMessage::ServerToClient ||
			msg.category != LbsMessage::CategoryAnswer || msg.command != LbsMessage::lbsWinLose32)
			return false;
		Clear();
		return true;
	}
};

inline LbsMessage ErrorReply(const LbsMessage& reply) {
	auto legacy = LbsMessage::SvAnswer(reply);
	legacy.command = LbsMessage::lbsWinLose;
	legacy.status = LbsMessage::StatusError;
	return legacy;
}

// Original 18-byte body followed by wins/losses/draws/invalid, all BE u32.
inline bool Decode(LbsMessage reply, LbsMessage& legacy, Record& record) {
	if (reply.command != LbsMessage::lbsWinLose32 || reply.direction != LbsMessage::ServerToClient ||
		reply.category != LbsMessage::CategoryAnswer || reply.status != LbsMessage::StatusSuccess || reply.body.size() != 34)
		return false;
	reply.reading = 18;
	Record decoded{reply.Read32(), reply.Read32(), reply.Read32(), reply.Read32()};
	reply.body.resize(18);
	reply.body_size = 18;
	reply.reading = 0;
	reply.command = LbsMessage::lbsWinLose;
	legacy = std::move(reply);
	record = decoded;
	return true;
}

inline std::array<u64, 4> DisplayValues(const Record& record) {
	return {u64(record.wins) + record.losses + record.draws, record.wins,
		u64(record.losses) + record.draws, record.invalid};
}

inline std::string Format(u64 value) {
	std::string text = std::to_string(value);
	if (text.size() < 5)
		text.insert(0, 5 - text.size(), ' ');
	return text;
}

inline float HorizontalScale(const std::string& text) {
	return text.empty() ? 1.f : std::min(1.f, 5.f / float(text.size()));
}

} // namespace gdxsv_win_lose32
