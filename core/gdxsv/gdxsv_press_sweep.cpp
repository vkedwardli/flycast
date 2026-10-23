// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_press_sweep.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include "emulator.h"
#include "gdxsv_ggpo.h"
#include "gdxsv_perf_trace.h"
#include "input/gamepad_device.h"
#ifdef USE_SDL
#include "sdl/sdl_gamepad.h"
#endif

namespace {

#ifdef USE_SDL
// The virtual pad gdxsv_input_attach_test_joystick creates, found by its name.
SDL_Joystick* OpenTestJoystick() {
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		const char* name = SDL_JoystickNameForIndex(i);
		if (SDL_JoystickIsVirtual(i) && name != nullptr && strcmp(name, "gdxsv test joystick") == 0) return SDL_JoystickOpen(i);
	}
	return nullptr;
}
#endif

}  // namespace

bool gdxsv_press_sweep_start(int player) {
#ifdef USE_SDL
	static bool started = false;
	int who, startMs, holdMs, gapMin, gapMax;
	const char* env = getenv("GDXSV_PRESS_SWEEP");
	if (started || env == nullptr || sscanf(env, "%d,%d,%d,%d,%d", &who, &startMs, &holdMs, &gapMin, &gapMax) != 5 || who != player)
		return false;
	SDL_Joystick* joystick = OpenTestJoystick();
	if (joystick == nullptr) return false;
	started = true;
	NOTICE_LOG(INPUT, "press sweep: player %d from %d ms, hold %d ms, gap %d-%d ms", who, startMs, holdMs, gapMin, gapMax);
	// Triggers rest at raw 0 = half pressed (A held via the gdxsv trigger mapping): release them.
	SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
	SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
	std::thread([=]() {
		std::mt19937 rng(540);
		std::uniform_int_distribution<int> gap(gapMin, gapMax);
		std::this_thread::sleep_for(std::chrono::milliseconds(startMs));
		// Random input until here gets the match into the battle; then the pad alone drives this peer.
		gdxsv_ggpo_random_input(false, 0, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		kcode[0] = ~0u;
		GDXSV_PERF_TRACE("sweep_start", -1, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		for (int n = 0;; n++) {
			// Alternate left / right so the unit stays in place.
			const int button = n % 2 ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT : SDL_CONTROLLER_BUTTON_DPAD_LEFT;
			GDXSV_PERF_TRACE("vpress", -1, button);
			SDL_JoystickSetVirtualButton(joystick, button, 1);
			std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
			GDXSV_PERF_TRACE("vrelease", -1, button);
			SDL_JoystickSetVirtualButton(joystick, button, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(gap(rng)));
		}
	}).detach();
	return true;
#else
	return false;
#endif
}
