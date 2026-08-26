#pragma once
#include <string>

// Ensures the shared slot-99 bootstrap savestate exists for this disk,
// downloading it if this install has never needed it before. It is a canned
// "sitting at the network-ready lobby screen" snapshot that file replay,
// Live Spectate and the rollback test harness all resume from, so it must be
// present before any dc_loadstate(99).
bool gdxsv_ensure_replay_savestate(int disk);

void gdxsv_start_replay(const std::string& replay_path, int pov);
void gdxsv_end_replay();
void gdxsv_replay_select_dialog();
