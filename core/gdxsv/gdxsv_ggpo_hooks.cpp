// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_ggpo_hooks.h"

#include <xxhash.h>

#include <cstdio>
#include <nowide/cstdio.hpp>
#include <string>

#include "cfg/cfg.h"
#include "gdxsv_config.h"
#include "log/Log.h"
#include "stdclass.h"

namespace {
// Per-frame log of GGPO saved states (gdxsv:hashlog=yes). Each line is
// "frame state_hash state_len". Every saved frame is logged, rollbacks
// included, so a frame number can appear more than once; the last line for a
// frame is the state the session settled on. The test harness reads the frame
// numbers to measure how far a local match progressed; state_hash is a cheap
// per-frame fingerprint of what GGPO saves (CPU and peripherals, not memory:
// rollback tracks memory by page).
FILE* hashLogFile;

void CloseHashLog() {
	if (hashLogFile != nullptr) {
		fclose(hashLogFile);
		hashLogFile = nullptr;
	}
}
}  // namespace

void gdxsv_ggpo_session_start() {
	// RollbackInputBeforeAdvance wins over RollbackInputAtFrameTop: the input is already added at the frame end
	gdxsv_ggpo_state.inputBeforeAdvance = config::GdxRollbackInputBeforeAdvance;
	gdxsv_ggpo_state.inputAtFrameTop = config::GdxRollbackInputAtFrameTop && !gdxsv_ggpo_state.inputBeforeAdvance;
	gdxsv_ggpo_state.localInputPending = false;
	if (gdxsv_ggpo_state.inputAtFrameTop) NOTICE_LOG(NETWORK, "GGPO local input read at the frame top");
	if (gdxsv_ggpo_state.inputBeforeAdvance) NOTICE_LOG(NETWORK, "GGPO local input added before the frame advance");
	CloseHashLog();
	if (config::loadBool("gdxsv", "hashlog", false)) {
		std::string path = get_writable_data_path("hashlog.txt");
		hashLogFile = nowide::fopen(path.c_str(), "w");
		if (hashLogFile == nullptr)
			WARN_LOG(NETWORK, "Can't open hash log %s", path.c_str());
		else
			NOTICE_LOG(NETWORK, "State hash log: %s", path.c_str());
	}
}

void gdxsv_ggpo_session_stop() {
	CloseHashLog();
	gdxsv_ggpo_state.localInputPending = false;
}

void gdxsv_ggpo_state_saved(int frame, const unsigned char* buffer, int len) {
	if (hashLogFile != nullptr) fprintf(hashLogFile, "%d %08x %d\n", frame, XXH32(buffer, len, 7), len);
}

int gdxsv_ggpo_frame_delay(int delay) { return gdxsv_ggpo_state.inputBeforeAdvance ? delay + 1 : delay; }
