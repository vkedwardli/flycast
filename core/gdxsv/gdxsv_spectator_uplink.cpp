#include "gdxsv_spectator_uplink.h"

#include <thread>
#include <utility>

#include "gdxsv.pb.h"
#include "gdxsv_network.h"
#include "sleep.h"
#include "types.h"

namespace {
// Mirrors LBS's own maxSpectatorPushFrames (see lbs_spectator.go in the
// gdxsv server repo): keeps a maximally-backed-up push well under a safe
// single-UDP-datagram size, and keeps it within what LBS will accept.
constexpr size_t kMaxBacklogFrames = 128;
}  // namespace

void GdxsvSpectatorUplink::Start(const std::string &lbs_host, int lbs_port, const std::string &battle_code,
								  int32_t session_id, bool is_training_game) {
	if (running_ || is_training_game || lbs_host.empty() || lbs_port == 0 || battle_code.empty()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mtx_);
		backlog_start_frame_ = 0;
		backlog_.clear();
		dirty_ = false;
		pending_round_events_.clear();
		pending_round_results_.clear();
	}

	running_ = true;
	std::thread([this, lbs_host, lbs_port, battle_code, session_id]() {
		ThreadMain(lbs_host, lbs_port, battle_code, session_id);
	}).detach();
}

void GdxsvSpectatorUplink::Stop() { running_ = false; }

void GdxsvSpectatorUplink::PushInput(int32_t frame, uint64_t packed_input) {
	if (!running_) return;

	std::lock_guard<std::mutex> lock(mtx_);
	int32_t next_frame = backlog_start_frame_ + static_cast<int32_t>(backlog_.size());

	// Mirror input_logs_'s own tail-rewrite semantics (see
	// GdxsvBackendRollback::appendKeyMsg1Inputs): a re-confirmation at an
	// equal-or-earlier frame replaces the tail instead of being treated
	// as a new frame.
	while (!backlog_.empty() && frame <= next_frame - 1) {
		backlog_.pop_back();
		next_frame--;
	}

	if (frame < backlog_start_frame_) {
		return;	 // already acked and evicted; nothing to do
	}
	if (backlog_.empty() || frame > next_frame) {
		// First push since Start(), or a gap ahead of what's been pushed
		// so far (shouldn't happen - GGPO confirms frames one at a time -
		// but re-anchoring here is safe and avoids ever mis-attributing a
		// value to the wrong frame index).
		backlog_.clear();
		backlog_start_frame_ = frame;
	}

	backlog_.push_back(packed_input);
	dirty_ = true;

	while (backlog_.size() > kMaxBacklogFrames) {
		backlog_.pop_front();
		backlog_start_frame_++;
	}
}

void GdxsvSpectatorUplink::PushRoundEvent(int32_t frame, uint64_t random_value) {
	if (!running_) return;

	std::lock_guard<std::mutex> lock(mtx_);
	pending_round_events_.push_back(RoundEvent{frame, random_value});
}

void GdxsvSpectatorUplink::PushRoundResult(int32_t round_index, int32_t win_team, const std::vector<int32_t> &used_ms) {
	if (!running_) return;

	std::lock_guard<std::mutex> lock(mtx_);
	pending_round_results_.push_back(RoundResult{round_index, win_team, used_ms});
}

void GdxsvSpectatorUplink::ThreadMain(std::string lbs_host, int lbs_port, std::string battle_code, int32_t session_id) {
	NOTICE_LOG(COMMON, "Start GdxsvSpectatorUplink Thread battle_code=%s", battle_code.c_str());

	UdpClient client;
	if (!client.Bind(0)) {
		WARN_LOG(COMMON, "GdxsvSpectatorUplink client.Bind failed");
		running_ = false;
		return;
	}

	UdpRemote remote;
	if (!remote.Open(lbs_host.c_str(), lbs_port)) {
		WARN_LOG(COMMON, "GdxsvSpectatorUplink remote.Open failed");
		running_ = false;
		return;
	}

	while (running_) {
		// Drain any pending acks (non-blocking - see UdpClient::Bind).
		while (true) {
			char buf[256];
			sockaddr_storage sender{};
			socklen_t addrlen = sizeof(sender);
			int n = client.RecvFrom(buf, sizeof(buf), &sender, &addrlen);
			if (n <= 0) break;

			proto::Packet pkt;
			if (!pkt.ParseFromArray(buf, n)) continue;
			if (pkt.type() != proto::MessageType::SpectatorInputAckType) continue;
			if (pkt.spectator_input_ack_data().battle_code() != battle_code) continue;

			std::lock_guard<std::mutex> lock(mtx_);
			const int32_t ack_frame = pkt.spectator_input_ack_data().ack_frame();
			while (backlog_start_frame_ < ack_frame && !backlog_.empty()) {
				backlog_.pop_front();
				backlog_start_frame_++;
			}
		}

		bool should_send_inputs = false;
		int32_t start_frame = 0;
		std::vector<uint64_t> inputs;
		std::vector<RoundEvent> round_events;
		std::vector<RoundResult> round_results;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			if (dirty_ && !backlog_.empty()) {
				should_send_inputs = true;
				start_frame = backlog_start_frame_;
				inputs.assign(backlog_.begin(), backlog_.end());
				dirty_ = false;
			}
			round_events.swap(pending_round_events_);
			round_results.swap(pending_round_results_);
		}

		if (should_send_inputs) {
			proto::Packet pkt;
			pkt.set_type(proto::MessageType::SpectatorInputPushType);
			auto *push = pkt.mutable_spectator_input_push_data();
			push->set_battle_code(battle_code);
			push->set_session_id(session_id);
			push->set_start_frame(start_frame);
			for (uint64_t v : inputs) push->add_inputs(v);

			char buf[1200];
			if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
				client.SendTo(buf, pkt.GetCachedSize(), remote);
			} else {
				WARN_LOG(COMMON, "GdxsvSpectatorUplink input push too large, dropping (backlog=%zu)", inputs.size());
			}
		}

		for (const auto &ev : round_events) {
			proto::Packet pkt;
			pkt.set_type(proto::MessageType::SpectatorRoundEventType);
			auto *e = pkt.mutable_spectator_round_event_data();
			e->set_battle_code(battle_code);
			e->set_session_id(session_id);
			e->set_frame(ev.frame);
			e->set_random_value(ev.random_value);

			char buf[256];
			if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
				client.SendTo(buf, pkt.GetCachedSize(), remote);
			}
		}

		for (const auto &r : round_results) {
			proto::Packet pkt;
			pkt.set_type(proto::MessageType::SpectatorRoundResultType);
			auto *rr = pkt.mutable_spectator_round_result_data();
			rr->set_battle_code(battle_code);
			rr->set_session_id(session_id);
			rr->set_round_index(r.round_index);
			rr->mutable_round()->set_win_team(r.win_team);
			for (int32_t ms : r.used_ms) rr->mutable_round()->add_used_ms(ms);

			char buf[256];
			if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
				client.SendTo(buf, pkt.GetCachedSize(), remote);
			}
		}

		sleep_us(1000);
	}

	client.Close();
	NOTICE_LOG(COMMON, "End GdxsvSpectatorUplink Thread");
}
