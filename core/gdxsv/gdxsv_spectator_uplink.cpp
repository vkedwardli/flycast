#include "gdxsv_spectator_uplink.h"

#include <chrono>
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
constexpr auto kRetryInterval = std::chrono::milliseconds(100);
constexpr auto kFinalDrainTimeout = std::chrono::seconds(2);
}  // namespace

void GdxsvSpectatorUplink::Start(const std::string &lbs_host, int lbs_port, const std::string &battle_code,
								  int32_t session_id, bool is_training_game) {
	if (running_ || is_training_game || lbs_host.empty() || lbs_port == 0 || battle_code.empty()) {
		return;
	}
	Stop(); // Join any previous worker before reusing its queues.

	{
		std::lock_guard<std::mutex> lock(mtx_);
		backlog_start_frame_ = 0;
		backlog_.clear();
		dirty_ = false;
		pending_round_events_.clear();
		pending_round_results_.clear();
	}

	running_ = true;
	thread_ = std::thread([this, lbs_host, lbs_port, battle_code, session_id]() {
		ThreadMain(lbs_host, lbs_port, battle_code, session_id);
	});
}

void GdxsvSpectatorUplink::Stop(bool drain_pending) {
	drain_pending_ = drain_pending;
	running_ = false;
	if (thread_.joinable()) thread_.join();
	drain_pending_ = false;
}

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

	auto next_input_send = std::chrono::steady_clock::now();
	auto next_round_send = std::chrono::steady_clock::now();
	auto drain_deadline = std::chrono::steady_clock::time_point::max();
	while (running_ || drain_pending_) {
		const auto now = std::chrono::steady_clock::now();
		if (!running_) {
			if (drain_deadline == std::chrono::steady_clock::time_point::max()) {
				drain_deadline = now + kFinalDrainTimeout;
			}
			std::lock_guard<std::mutex> lock(mtx_);
			if (backlog_.empty() && pending_round_events_.empty() && pending_round_results_.empty()) break;
			if (now >= drain_deadline) {
				WARN_LOG(COMMON, "spectator uplink final drain timed out: inputs=%zu starts=%zu results=%zu",
					backlog_.size(), pending_round_events_.size(), pending_round_results_.size());
				break;
			}
		}
		// Drain any pending acks (non-blocking - see UdpClient::Bind).
		for (int received = 0; received < 64; ++received) {
			char buf[256];
			sockaddr_storage sender{};
			socklen_t addrlen = sizeof(sender);
			int n = client.RecvFrom(buf, sizeof(buf), &sender, &addrlen);
			if (n <= 0) break;
			if (!is_same_addr(reinterpret_cast<const sockaddr *>(&sender), remote.net_addr())) continue;

			proto::Packet pkt;
			if (!pkt.ParseFromArray(buf, n)) continue;
			if (pkt.type() != proto::MessageType::SpectatorInputAckType) continue;
			if (pkt.spectator_input_ack_data().battle_code() != battle_code) continue;

			std::lock_guard<std::mutex> lock(mtx_);
			const auto &ack = pkt.spectator_input_ack_data();
			const int32_t ack_frame = ack.ack_frame();
			// LBS can be ahead of our queue because another participant
			// already supplied these inputs. Evict only what we actually hold.
			while (backlog_start_frame_ < ack_frame && !backlog_.empty()) {
				backlog_.pop_front();
				backlog_start_frame_++;
			}
			for (int32_t frame : ack.round_event_ack()) {
				if (!pending_round_events_.empty() && pending_round_events_.front().frame == frame) {
					pending_round_events_.pop_front();
					next_round_send = now;
				}
			}
			for (int32_t index : ack.round_result_ack()) {
				if (!pending_round_results_.empty() && pending_round_results_.front().round_index == index) {
					pending_round_results_.pop_front();
					next_round_send = now;
				}
			}
		}

		bool should_send_inputs = false;
		int32_t start_frame = 0;
		std::vector<uint64_t> inputs;
		std::vector<RoundEvent> round_events;
		std::vector<RoundResult> round_results;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			if (!backlog_.empty() && (dirty_ || now >= next_input_send)) {
				should_send_inputs = true;
				start_frame = backlog_start_frame_;
				inputs.assign(backlog_.begin(), backlog_.end());
				dirty_ = false;
				next_input_send = now + kRetryInterval;
			}
			if (now >= next_round_send) {
				// Only the oldest unacknowledged start is sent. UDP reordering
				// cannot put a later round before a missing RNG seed on LBS.
				if (!pending_round_events_.empty()) round_events.push_back(pending_round_events_.front());
				if (!pending_round_results_.empty()) round_results.push_back(pending_round_results_.front());
				next_round_send = now + kRetryInterval;
			}
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
