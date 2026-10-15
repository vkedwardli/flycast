// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Functions provided to core/network/ggpo.cpp by the gdxsv module.

void gdxsv_ggpo_session_start();
void gdxsv_ggpo_session_stop();
// GGPO saved the state of a frame, rollbacks included.
void gdxsv_ggpo_state_saved(int frame, const unsigned char* buffer, int len);
