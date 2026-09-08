#pragma once

#include <array>
#include <string>
#include "gdxsv_stats32.h"

namespace gdxsv_win_lose32 {

struct Record {
	u32 wins;
	u32 losses;
	u32 draws;
	u32 invalid;
};

inline bool IsRequest(const LbsMessage& msg) {
	return gdxsv_stats32::IsRequest(msg, LbsMessage::lbsWinLose);
}

// The personal panel has one outstanding request. A new category invalidates
// the previous reply; categories other than zero keep the legacy path.
class Requests {
	gdxsv_stats32::PendingRequest pending_;

public:
	void Clear() { pending_ = {}; }
	bool Rewrite(LbsMessage& msg) {
		if (!IsRequest(msg))
			return false;
		Clear();
		if (msg.body[0] != 0)
			return false;
		pending_ = {true, msg.seq};
		msg.command = LbsMessage::lbsWinLose32;
		return true;
	}

	bool TakeReply(const LbsMessage& msg) {
		return pending_.TakeReply(msg, LbsMessage::lbsWinLose32);
	}
};

inline LbsMessage ErrorReply(const LbsMessage& reply) {
	return gdxsv_stats32::ErrorReply(reply, LbsMessage::lbsWinLose);
}

// Original 18-byte body followed by wins/losses/draws/invalid, all BE u32.
inline bool Decode(LbsMessage reply, LbsMessage& legacy, Record& record) {
	if (!gdxsv_stats32::IsReply(reply, LbsMessage::lbsWinLose32) ||
		reply.status != LbsMessage::StatusSuccess || reply.body.size() != 34)
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
	return gdxsv_stats32::HorizontalScale(text, 5.f);
}

} // namespace gdxsv_win_lose32
