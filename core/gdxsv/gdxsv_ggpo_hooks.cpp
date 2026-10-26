// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_ggpo_hooks.h"

#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <nowide/cstdio.hpp>
#include <string>

#include "cfg/cfg.h"
#include "gdxsv_config.h"
#include "gdxsv_net_report.h"
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

constexpr float kRollbackBudgetMax = 1.5f;
constexpr int kRollbackBudgetMinFrames = 1800;

// The session's budget is the rollback depth that fits in a frame, minus the frame the input is sent after the
// Present; the setting moves halfway to it.
void UpdateRollbackBudget() {
	double fits = 0;
	const int frames = gdxsv_net_report_slack_frames(&fits);
	if (frames < kRollbackBudgetMinFrames) return;
	const float session = std::clamp(static_cast<float>(fits - 1), 0.f, kRollbackBudgetMax);
	const float old = gdxsv_rollback_budget();
	config::GdxRollbackBudget = 0.5f * old + 0.5f * session;
	config::GdxRollbackBudget.save();
	NOTICE_LOG(COMMON, "rollback budget: fits %.2f frames (%d frames), budget %.2f -> %.2f", fits, frames, old,
			   config::GdxRollbackBudget.get());
}
}  // namespace

float gdxsv_rollback_budget() { return std::clamp(config::GdxRollbackBudget.get(), 0.f, kRollbackBudgetMax); }

void gdxsv_ggpo_session_start() {
	gdxsv_ggpo_state.inputBeforeAdvance = false;
	gdxsv_net_report_session_start();
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
	gdxsv_net_report_session_stop();
	UpdateRollbackBudget();
}

void gdxsv_ggpo_state_saved(int frame, const unsigned char* buffer, int len) {
	if (hashLogFile != nullptr) fprintf(hashLogFile, "%d %08x %d\n", frame, XXH32(buffer, len, 7), len);
}

int gdxsv_ggpo_frame_delay(int delay) { return gdxsv_ggpo_state.inputBeforeAdvance ? delay + 1 : delay; }
