#pragma once

#include <array>
#include <string>
#include "gdxsv_stats32.h"

namespace gdxsv_player_info32 {

struct Record {
	u32 battles;
	u32 wins;
	u32 losses;
};

inline bool IsRequest(const LbsMessage& msg) {
	return gdxsv_stats32::IsRequest(msg, LbsMessage::lbsAskPlayerInfo) && msg.body[0] >= 1 && msg.body[0] <= 4;
}

// At most one outstanding player-info request per card. No timeout fallback:
// live servers must implement the new command before this client is released.
class Requests {
	std::array<gdxsv_stats32::PendingRequest, 4> pending_{};

public:
	void Clear() { pending_ = {}; }
	bool Rewrite(LbsMessage& msg) {
		if (!IsRequest(msg))
			return false;
		const size_t player = msg.body[0] - 1;
		for (size_t p = 0; p < pending_.size(); ++p)
			if (p != player && pending_[p].Matches(msg.seq))
				return false;
		pending_[player] = {true, msg.seq};
		msg.command = LbsMessage::lbsAskPlayerInfo32;
		return true;
	}
	u8 TakeReply(const LbsMessage& msg) {
		for (size_t p = 0; p < pending_.size(); ++p) {
			if (pending_[p].TakeReply(msg, LbsMessage::lbsAskPlayerInfo32)) {
				return u8(p + 1);
			}
		}
		return 0;
	}
};

inline LbsMessage ErrorReply(const LbsMessage& reply) {
	return gdxsv_stats32::ErrorReply(reply, LbsMessage::lbsAskPlayerInfo);
}

// Reply body: original lbsAskPlayerInfo body, followed by battles/wins/losses
// as three big-endian u32s. The original fields remain bounded legacy copies.
inline LbsMessage Encode(LbsMessage legacy, const Record& record) {
	legacy.command = LbsMessage::lbsAskPlayerInfo32;
	legacy.Write32(record.battles)->Write32(record.wins)->Write32(record.losses);
	return legacy;
}

inline bool Decode(LbsMessage reply, LbsMessage& legacy, Record& record, int expected_player = 0) {
	if (!gdxsv_stats32::IsReply(reply, LbsMessage::lbsAskPlayerInfo32) ||
		reply.status != LbsMessage::StatusSuccess || reply.body.size() < 35)
		return false;
	if (reply.body[0] < 1 || reply.body[0] > 4)
		return false;
	if (expected_player != 0 && reply.body[0] != expected_player)
		return false;
	const size_t legacy_size = reply.body.size() - 12;
	size_t cursor = 1;
	for (int field = 0; field < 3; ++field) {
		if (cursor + 2 > legacy_size)
			return false;
		const size_t length = (size_t(reply.body[cursor]) << 8) | reply.body[cursor + 1];
		cursor += 2;
		if (length > legacy_size - cursor)
			return false;
		cursor += length;
	}
	if (cursor + 16 != legacy_size)
		return false;
	reply.reading = int(legacy_size);
	Record decoded{reply.Read32(), reply.Read32(), reply.Read32()};
	reply.body.resize(legacy_size);
	reply.body_size = u16(legacy_size);
	reply.reading = 0;
	reply.command = LbsMessage::lbsAskPlayerInfo;
	legacy = std::move(reply);
	record = decoded;
	return true;
}

// The ROM's translated format is three %4d fields plus single-byte English or
// double-byte Shift-JIS labels. Do not feed unsigned counters to signed printf.
inline std::string Format(const std::string& format, const Record& record) {
	const std::array<u32, 3> values{record.battles, record.wins, record.losses};
	std::string result;
	size_t field = 0;
	for (size_t i = 0; i < format.size();) {
		if (format[i] == '%') {
			if (format.compare(i, 3, "%4d") != 0 || field == values.size())
				return {};
			const std::string digits = std::to_string(values[field++]);
			if (digits.size() < 4)
				result.append(4 - digits.size(), ' ');
			result += digits;
			i += 3;
		} else {
			result += format[i++];
		}
	}
	return field == values.size() && result.size() < 40 ? result : std::string{};
}

inline float HorizontalScale(const std::string& text) {
	return gdxsv_stats32::HorizontalScale(text, 18.f);
}

} // namespace gdxsv_player_info32
