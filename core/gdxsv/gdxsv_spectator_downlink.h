#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "gdxsv.pb.h"

// Receives this spectator's live updates from LBS over UDP.
//
// Same port and proto messages as the uplink: sends SpectatorSubscribeRequest
// to register and keep alive, receives SpectatorInputPush, acks the highest
// contiguous frame with SpectatorInputAck.
//
// Owns a background thread and its own socket. That thread only queues what it
// receives. DrainInto and ReportAcked run on the main thread and do the actual
// folding into the BattleLogFile.
class GdxsvSpectatorDownlink {
   public:
	~GdxsvSpectatorDownlink() { Stop(); }

	// Starts the background thread for one battle. from_frame is
	// log_file->inputs_size() at call time, so the first push only carries
	// what came after it.
	void Start(const std::string &lbs_host, int lbs_port, const std::string &battle_code, int32_t from_frame);

	// Stops the background thread. Asynchronous, same as the uplink's Stop.
	void Stop();

	// Blocks until the header and every patch have arrived, or timeout_ms
	// passes. Writes the header to *out, returns false on timeout.
	// Patches stream as their own chunks and usually land after the header.
	// Playback needs them all to simulate correctly.
	bool WaitForBootstrap(proto::BattleLogFile *out, int timeout_ms);

	// Folds all queued pushes into *log_file in receive order. Main thread,
	// every frame. Returns true if anything was folded in.
	bool DrainInto(proto::BattleLogFile *log_file);

	// Reports what the main thread has actually folded in, so the next ack
	// reflects that and not just what was received. Call after every
	// DrainInto that returns true.
	void ReportAcked(int32_t frame);

   private:
	void ThreadMain(std::string lbs_host, int lbs_port, std::string battle_code, int32_t from_frame);

	std::atomic<bool> running_{false};

	std::mutex mtx_;
	std::deque<proto::SpectatorInputPush> pending_;	// raw received pushes, main thread folds them in
	proto::BattleLogFile header_;					// bootstrap header, once received
	bool have_header_ = false;
	std::vector<proto::GamePatch> patches_;			// bootstrap patches, assembled in order
	int32_t patch_total_ = -1;						// -1 until a chunk tells us how many there are
	int32_t acked_frame_ = 0;							// highest frame the main thread has folded in
	bool acked_dirty_ = false;							// true if acked_frame_ changed since the last ack sent
};
