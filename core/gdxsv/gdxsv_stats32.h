#pragma once

#include <algorithm>
#include <string>
#include "lbs_message.h"

namespace gdxsv_stats32 {

inline bool IsRequest(const LbsMessage& msg, u16 command) {
	return msg.direction == LbsMessage::ClientToServer && msg.category == LbsMessage::CategoryQuestion &&
		msg.command == command && msg.body.size() == 1;
}

inline bool IsReply(const LbsMessage& msg, u16 command) {
	return msg.direction == LbsMessage::ServerToClient && msg.category == LbsMessage::CategoryAnswer &&
		msg.command == command;
}

// Each API owns its slot/replacement policy; matching consumes one pending reply,
// including server errors. Payload validation happens after request tracking.
struct PendingRequest {
	bool active = false;
	u16 seq = 0;

	bool Matches(u16 reply_seq) const { return active && seq == reply_seq; }
	bool TakeReply(const LbsMessage& msg, u16 command) {
		if (!Matches(msg.seq) || !IsReply(msg, command))
			return false;
		active = false;
		return true;
	}
};

inline LbsMessage ErrorReply(const LbsMessage& reply, u16 legacy_command) {
	auto legacy = LbsMessage::SvAnswer(reply);
	legacy.command = legacy_command;
	legacy.status = LbsMessage::StatusError;
	return legacy;
}

inline float HorizontalScale(const std::string& text, float width) {
	return text.empty() ? 1.f : std::min(1.f, width / float(text.size()));
}

} // namespace gdxsv_stats32
