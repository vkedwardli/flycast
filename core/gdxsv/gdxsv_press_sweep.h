// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Test only: with env GDXSV_PRESS_SWEEP=<player 0-3>,<start ms>,<hold ms>,<gap min ms>,<gap max ms> and the
// test joystick attached, a host thread presses dpad left / right at random times (perf trace "vpress").
// Random input of that peer stays on until the start, so the match reaches the battle.
// Returns true if it started for this player.
bool gdxsv_press_sweep_start(int player);
