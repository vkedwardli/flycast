// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

// Input hooks for core/input and core/sdl.

// True when replay consumed the mapped Fast-forward press.
bool gdxsv_input_toggle_fast_forward();

// Test only: with env GDXSV_TEST_JOYSTICK=<script>, attaches an SDL virtual
// gamepad that plays the script on vblanks (see gdxsv_input.cpp).
void gdxsv_input_attach_test_joystick();

// A digital direction pair mapped to an analog axis. With gdxsv enabled the
// axis goes to full deflection at once and opposite directions cancel out;
// returns true then and the caller skips its ramp.
bool gdxsv_input_digital_axis(u32 state, u32 negDir, u32 posDir, s16& joystick);
