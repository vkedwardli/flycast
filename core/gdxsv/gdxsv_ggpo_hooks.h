// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <mutex>
#include <random>

#include "types.h"

// Functions provided to core/network/ggpo.cpp by the gdxsv module.

void gdxsv_ggpo_session_start();
void gdxsv_ggpo_session_stop();
// GGPO saved the state of a frame, rollbacks included.
void gdxsv_ggpo_state_saved(int frame, const unsigned char* buffer, int len);

// gdxsv state of a GGPO session, shared by core/network/ggpo.cpp and gdxsv_ggpo.cpp.
struct GdxsvGgpoState {
	int playerHandles[4];  // GGPOPlayerHandle per player
	int disconnectFlags;   // bit per disconnected player, from ggpo_synchronize_input
	std::array<int, 3> inputBlockCount;
	bool useExInput;
	bool testLog;  // GGPO_TEST_LOG: log timesync skips and rollback loads for tools/rbk_test
	int totalRollbackFrames;
	int totalTimeSync;
	float fpsAvg;
	bool useRandInput;
	u32 randInputMask;
	std::mt19937 randSource;
};
extern GdxsvGgpoState gdxsv_ggpo_state;

struct GGPOSession;
// Upstream state of core/network/ggpo.cpp that gdxsv_ggpo.cpp starts a session with.
struct GdxsvGgpoUpstream {
	GGPOSession** session;
	std::recursive_mutex* mutex;
	int* localPlayerNum;
	int* localPlayer;  // GGPOPlayerHandle
	int* remotePlayer;
	bool* synchronized;
	int* analogAxes;
	int* inputSize;
};
// Defined in core/network/ggpo.cpp. Fills cb (a GGPOSessionCallbacks) with upstream's callbacks.
GdxsvGgpoUpstream gdxsv_ggpo_upstream(void* cb);
