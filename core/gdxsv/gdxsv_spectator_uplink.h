#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// Streams this client's confirmed inputs to LBS so the match can be watched
// live. All 4 participants stream the same frames redundantly; LBS dedups by
// frame index.
//
// Confirmed frames sit in a backlog and are resent on every push until LBS
// acks them, so a dropped packet delays a frame instead of losing it. Same
// idea as GGPO's UdpProtocol::SendPendingOutput.
//
// Owns a background thread and its own UDP socket on an ephemeral port. This
// is client-to-server, not P2P, so no port forwarding is needed. The Push*
// methods only touch a mutex-guarded queue and never block, so the emulation
// thread can call them on the hot path.
class GdxsvSpectatorUplink {
   public:
	~GdxsvSpectatorUplink() { Stop(); }

	// Starts the background thread for one battle. No-op if already running,
	// or for a training game - LBS ignores those, so don't spend a thread on
	// one. SaveReplay() skips them the same way.
	void Start(const std::string &lbs_host, int lbs_port, const std::string &battle_code, int32_t session_id,
			   bool is_training_game);

	// Stops the background thread (asynchronously - see Stop()'s doc).
	void Stop();

	// Once per confirmed frame, with the same packed 4-player input that goes
	// into input_logs_.
	void PushInput(int32_t frame, uint64_t packed_input);

	// Mirrors start_msg_indexes_/start_msg_randoms_: called once per
	// round start with the RNG seed read at that frame.
	void PushRoundEvent(int32_t frame, uint64_t random_value);

	// Mirrors round_data_: called when a round's win_team becomes known.
	void PushRoundResult(int32_t round_index, int32_t win_team, const std::vector<int32_t> &used_ms);

   private:
	struct RoundEvent {
		int32_t frame;
		uint64_t random_value;
	};
	struct RoundResult {
		int32_t round_index;
		int32_t win_team;
		std::vector<int32_t> used_ms;
	};

	void ThreadMain(std::string lbs_host, int lbs_port, std::string battle_code, int32_t session_id);

	// Stop() clears this; the thread notices next loop, closes its socket and
	// exits. Not joined - this only ever lives in the global Gdxsv singleton,
	// which outlives any battle. Same pattern as UdpPingPong.
	std::atomic<bool> running_{false};

	std::mutex mtx_;
	// backlog holds confirmed frames LBS has not yet acked, in frame
	// order starting at backlog_start_frame_. Grown by PushInput, trimmed
	// as acks arrive on the background thread.
	int32_t backlog_start_frame_ = 0;
	std::deque<uint64_t> backlog_;
	bool dirty_ = false;
	std::vector<RoundEvent> pending_round_events_;
	std::vector<RoundResult> pending_round_results_;
};
