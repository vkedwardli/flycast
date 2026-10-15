// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <future>
#include <string>
#include <vector>

#include "types.h"

// gdxsv GGPO session on top of core/network/ggpo.cpp: up to 4 peers, relays, stats.

// Same layout as GGPONetworkStats, plus gdxsv counters.
struct GdxsvGgpoNetworkStats {
	struct {
		int send_queue_len;
		int recv_queue_len;
		int ping;
		int kbps_sent;
		int recv_packet_loss;
		int send_packet_loss;
		int is_relay;
	} network;
	struct {
		int local_frames_behind;
		int remote_frames_behind;
	} timesync;
	struct {
		int predicted_frames;
	} sync;
	struct {
		int total_rollbacked_frames;
		int total_timesync;
		int current_timesync;
		float current_fps;
		int input_block_count[3];
	} extra;
};

struct GdxsvGgpoRelayServerAddr {
	std::string ip;		// what this peer uses
	std::string altIp;	// the other IP family, or empty
	u16 port;
};

std::future<bool> gdxsv_ggpo_start_network(const char* sessionCode, int me, const std::vector<std::string>& ips,
										   const std::vector<u16>& ports, const std::vector<u8>& relays,
										   const std::vector<GdxsvGgpoRelayServerAddr>& relayServers);
bool gdxsv_ggpo_is_connected(int playerNum);
void gdxsv_ggpo_disconnect(int playerNum);
void gdxsv_ggpo_random_input(bool enable, u64 seed, u32 inputMask);
void gdxsv_ggpo_get_network_stats(int playerNum, GdxsvGgpoNetworkStats* stats);
