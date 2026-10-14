// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

// Input hooks for core/input and core/sdl.

// Reads the state of every open SDL joystick and feeds the changes to its
// gamepad device, without the SDL event queue (option JoystickPolling). Safe
// to call from the emulator thread.
void gdxsv_input_poll_joysticks();

// gdxsv_input_poll_joysticks() plus the digital-to-analog ramp: the input
// update of the emulator thread when rendering is threaded.
void gdxsv_input_update_joysticks();

// A digital direction pair mapped to an analog axis. With gdxsv enabled the
// axis goes to full deflection at once and opposite directions cancel out;
// returns true then and the caller skips its ramp.
bool gdxsv_input_digital_axis(u32 state, u32 negDir, u32 posDir, s16& joystick);
