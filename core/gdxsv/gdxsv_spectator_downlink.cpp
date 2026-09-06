#include "gdxsv_spectator_downlink.h"

#include <chrono>
#include <thread>

#include "gdxsv_network.h"
#include "sleep.h"
#include "types.h"

namespace {
// How often SpectatorSubscribeRequest is resent as a keepalive/resume-point
// refresh - must stay comfortably under LBS's spectatorSubscriberTimeout
// (10s, see lbs_spectator.go) so a brief delay never drops the subscription.
constexpr int kSubscribeIntervalMs = 2000;
constexpr size_t kSubscribeCookieBytes = 16;
}  // namespace

void GdxsvSpectatorDownlink::Start(const std::string &lbs_host, int lbs_port, const std::string &battle_code,
									int32_t from_frame) {
	if (running_ || lbs_host.empty() || lbs_port == 0 || battle_code.empty()) {
		return;
	}
	Stop(); // Join an old worker, including one that failed during startup.
	applied_round_state_version_ = 0;

	{
		std::lock_guard<std::mutex> lock(mtx_);
		pending_.clear();
		have_header_ = false;
		patches_.clear();
		patch_total_ = -1;
		acked_frame_ = from_frame;
		acked_round_state_version_ = 0;
		acked_dirty_ = false;
	}
	running_ = true;
	thread_ = std::thread([this, lbs_host, lbs_port, battle_code, from_frame]() {
		ThreadMain(lbs_host, lbs_port, battle_code, from_frame);
	});
}

void GdxsvSpectatorDownlink::Stop() {
	running_ = false;
	if (thread_.joinable()) thread_.join();
}

bool GdxsvSpectatorDownlink::WaitForBootstrap(proto::BattleLogFile *out, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (running_ && std::chrono::steady_clock::now() < deadline) {
		{
			std::lock_guard<std::mutex> lock(mtx_);
			if (have_header_ && patch_total_ >= 0 && static_cast<int32_t>(patches_.size()) == patch_total_) {
				*out = header_;
				for (const auto &patch : patches_) {
					*out->add_patches() = patch;
				}
				return true;
			}
		}
		sleep_us(2000);
	}
	return false;
}

bool GdxsvSpectatorDownlink::DrainInto(proto::BattleLogFile *log_file) {
	std::deque<proto::SpectatorInputPush> pushes;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		pushes.swap(pending_);
	}

	bool applied = false;
	for (const auto &push : pushes) {
		const int32_t have = log_file->inputs_size();

		if (0 < push.inputs_size()) {
			if (push.start_frame() > have) {
				// A gap. Drop the rest of the batch instead of guessing. The
				// next keepalive re-asks from acked_frame_ and LBS refills it.
				// Should never happen: LBS pushes from the frame we acked.
				WARN_LOG(COMMON, "spectator downlink gap: start_frame=%d have=%d", push.start_frame(), have);
				return applied;
			}
			const int32_t offset = have - push.start_frame();
			for (int32_t i = offset; i < push.inputs_size(); ++i) {
				log_file->add_inputs(push.inputs(i));
			}
			if (offset < push.inputs_size()) applied = true;
		}

		// Results change without another round start. A version also keeps a
		// delayed older snapshot from overwriting a result already applied.
		// Close-only pushes carry the required version, but no snapshot.
		if (push.close_reason().empty() && push.round_state_version() > applied_round_state_version_) {
			log_file->mutable_start_msg_indexes()->CopyFrom(push.start_msg_indexes());
			log_file->mutable_start_msg_randoms()->CopyFrom(push.start_msg_randoms());
			log_file->mutable_round_data()->CopyFrom(push.round_data());
			applied_round_state_version_ = push.round_state_version();
			{
				std::lock_guard<std::mutex> lock(mtx_);
				acked_round_state_version_ = applied_round_state_version_;
				acked_dirty_ = true;
			}
			applied = true;
		}
		if (!push.close_reason().empty() && log_file->close_reason().empty() &&
			log_file->inputs_size() >= push.start_frame() &&
			applied_round_state_version_ >= push.round_state_version()) {
			log_file->set_close_reason(push.close_reason());
			log_file->set_disconnect_user_index(push.disconnect_user_index());
			applied = true;
		}
	}

	return applied;
}

void GdxsvSpectatorDownlink::ReportAcked(int32_t frame) {
	std::lock_guard<std::mutex> lock(mtx_);
	if (frame != acked_frame_) {
		acked_frame_ = frame;
		acked_dirty_ = true;
	}
}

void GdxsvSpectatorDownlink::ThreadMain(std::string lbs_host, int lbs_port, std::string battle_code,
										 int32_t from_frame) {
	NOTICE_LOG(COMMON, "Start GdxsvSpectatorDownlink Thread battle_code=%s", battle_code.c_str());

	UdpClient client;
	if (!client.Bind(0)) {
		WARN_LOG(COMMON, "GdxsvSpectatorDownlink client.Bind failed");
		running_ = false;
		return;
	}

	UdpRemote remote;
	if (!remote.Open(lbs_host.c_str(), lbs_port)) {
		WARN_LOG(COMMON, "GdxsvSpectatorDownlink remote.Open failed");
		running_ = false;
		return;
	}

	// The first request reserves enough bytes for the stateless challenge.
	// Keep the echoed cookie local to this battle's socket/thread.
	std::string cookie(kSubscribeCookieBytes, '\0');
	auto send_subscribe = [&](int32_t from) {
		proto::Packet pkt;
		pkt.set_type(proto::MessageType::SpectatorSubscribeType);
		auto *sub = pkt.mutable_spectator_subscribe_data();
		sub->set_battle_code(battle_code);
		sub->set_from_frame(from);
		sub->set_cookie(cookie);

		char buf[256];
		if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
			client.SendTo(buf, pkt.GetCachedSize(), remote);
		}
	};

	send_subscribe(from_frame);
	int64_t last_subscribe_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
			.count();

	while (running_) {
		for (int received = 0; running_ && received < 64; ++received) {
			// Must fit the largest datagram LBS sends. Input pushes are ~1KB,
			// but the bootstrap header measured ~1.8KB for 2 players and grows
			// with player count. Too small and recvfrom truncates silently,
			// ParseFromArray fails, and the header is lost.
			char buf[8192];
			sockaddr_storage sender{};
			socklen_t addrlen = sizeof(sender);
			int n = client.RecvFrom(buf, sizeof(buf), &sender, &addrlen);
			if (n <= 0) break;
			if (!is_same_addr(reinterpret_cast<const sockaddr *>(&sender), remote.net_addr())) continue;

			proto::Packet pkt;
			if (!pkt.ParseFromArray(buf, n)) continue;
			if (pkt.type() == proto::MessageType::SpectatorSubscribeChallengeType) {
				const auto &challenge = pkt.spectator_subscribe_challenge_data();
				if (challenge.battle_code() != battle_code || challenge.cookie().size() != kSubscribeCookieBytes) continue;
				cookie = challenge.cookie();
				int32_t from;
				{
					std::lock_guard<std::mutex> lock(mtx_);
					from = acked_frame_;
				}
				send_subscribe(from);
				last_subscribe_ms =
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
						.count();
				continue;
			}
			if (pkt.type() != proto::MessageType::SpectatorInputPushType) continue;
			if (pkt.spectator_input_push_data().battle_code() != battle_code) continue;

			const auto &push = pkt.spectator_input_push_data();
			std::lock_guard<std::mutex> lock(mtx_);
			if (push.has_header()) {
				header_ = push.header();
				// Completion belongs to the final, ACK-gated push, never the
				// bootstrap metadata of an already-finished battle.
				header_.clear_close_reason();
				header_.clear_disconnect_user_index();
				have_header_ = true;
				patch_total_ = push.patch_total(); // Includes the zero-patch case.
			}
			if (0 < push.patches_size() || 0 < push.patch_total()) {
				patch_total_ = push.patch_total();
				// Only append a chunk that starts exactly where we left off.
				// Anything else is a resend or out of order, and LBS keeps
				// resending from patch_ack until it lands.
				if (push.patch_start() == static_cast<int32_t>(patches_.size())) {
					for (int i = 0; i < push.patches_size(); ++i) {
						patches_.push_back(push.patches(i));
					}
				}
			}
			pending_.push_back(push);
			// Repeat receipt feedback even during bootstrap, when playback
			// cannot advance its frame ACK yet. This keeps patch delivery and
			// the server's silent-subscriber recovery moving.
			acked_dirty_ = true;
		}

		int32_t ack_frame = 0;
		int32_t patch_ack = 0;
		int32_t round_ack = 0;
		bool should_ack = false;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			patch_ack = static_cast<int32_t>(patches_.size());
			if (acked_dirty_) {
				ack_frame = acked_frame_;
				round_ack = acked_round_state_version_;
				should_ack = true;
				acked_dirty_ = false;
			}
		}
		if (should_ack) {
			proto::Packet pkt;
			pkt.set_type(proto::MessageType::SpectatorInputAckType);
			auto *ack = pkt.mutable_spectator_input_ack_data();
			ack->set_battle_code(battle_code);
			ack->set_ack_frame(ack_frame);
			ack->set_patch_ack(patch_ack);
			ack->set_round_ack(round_ack);

			char buf[256];
			if (pkt.SerializePartialToArray(buf, sizeof(buf))) {
				client.SendTo(buf, pkt.GetCachedSize(), remote);
			}
		}

		const int64_t now_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count();
		if (kSubscribeIntervalMs <= now_ms - last_subscribe_ms) {
			int32_t from;
			{
				std::lock_guard<std::mutex> lock(mtx_);
				from = acked_frame_;
			}
			send_subscribe(from);
			last_subscribe_ms = now_ms;
		}

		sleep_us(1000);
	}

	client.Close();
	NOTICE_LOG(COMMON, "End GdxsvSpectatorDownlink Thread");
}
