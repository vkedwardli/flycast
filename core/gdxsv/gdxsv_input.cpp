// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_input_hooks.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "gdxsv_emu_hooks.h"
#include "input/gamepad_device.h"
#ifdef USE_SDL
#include "sdl/sdl_gamepad.h"
#endif

#ifdef USE_SDL
namespace {

// What the last poll saw, to report only changes
struct GdxsvJoystickPollState {
	u64 buttons = 0;  // up to 64 buttons
	u32 hats = 0;	  // 4 bits per hat, up to 8 hats
	std::vector<Sint16> axes;
};

struct GdxsvInputPoll {
	// The main thread and the emulator thread both poll
	std::mutex mutex;
	// By SDL joystick instance id; ids are never reused
	std::map<SDL_JoystickID, GdxsvJoystickPollState> joysticks;
};

GdxsvInputPoll input_poll;

void SetHatButtons(GamepadDevice& gamepad, u32 code, bool neg, bool pos) {
	gamepad.gamepad_btn_input(code, neg);
	gamepad.gamepad_btn_input(code + 1, pos);
}

void PollJoystick(SDL_Joystick* joystick, GamepadDevice& gamepad, GdxsvJoystickPollState& prev) {
	const int numButtons = std::min(SDL_JoystickNumButtons(joystick), 64);
	const int numAxes = SDL_JoystickNumAxes(joystick);
	const int numHats = std::min(SDL_JoystickNumHats(joystick), 8);

	if (prev.axes.size() != (size_t)numAxes) prev.axes.resize(numAxes, 0);

	u64 buttons = 0;
	for (int i = 0; i < numButtons; i++) {
		if (SDL_JoystickGetButton(joystick, i)) buttons |= 1ULL << i;
	}
	const u64 changed = buttons ^ prev.buttons;
	if (changed) {
		for (int i = 0; i < numButtons; i++) {
			if (changed & (1ULL << i)) gamepad.gamepad_btn_input(i, (buttons & (1ULL << i)) != 0);
		}
		prev.buttons = buttons;
	}

	for (int i = 0; i < numAxes; i++) {
		const Sint16 value = SDL_JoystickGetAxis(joystick, i);
		if (value != prev.axes[i]) {
			prev.axes[i] = value;
			gamepad.gamepad_axis_input(i, value);
		}
	}

	u32 hats = 0;
	for (int i = 0; i < numHats; i++) hats |= (SDL_JoystickGetHat(joystick, i) & 0xF) << (i * 4);
	const u32 hatChanged = hats ^ prev.hats;
	if (hatChanged) {
		for (int i = 0; i < numHats; i++) {
			if (!(hatChanged & (0xF << (i * 4)))) continue;
			// Same button codes as the SDL event path: (hat + 1) << 8, then up, down, left, right
			const Uint8 hat = (hats >> (i * 4)) & 0xF;
			const u32 code = (i + 1) << 8;
			SetHatButtons(gamepad, code + 0, (hat & SDL_HAT_UP) != 0, !(hat & SDL_HAT_UP) && (hat & SDL_HAT_DOWN));
			SetHatButtons(gamepad, code + 2, (hat & SDL_HAT_LEFT) != 0, !(hat & SDL_HAT_LEFT) && (hat & SDL_HAT_RIGHT));
		}
		prev.hats = hats;
	}
}

}  // namespace
#endif

void gdxsv_input_poll_joysticks() {
#ifdef USE_SDL
	std::lock_guard<std::mutex> lock(input_poll.mutex);
	SDL_JoystickUpdate();
	const int count = SDL_NumJoysticks();
	for (int i = 0; i < count; i++) {
		const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
		SDL_Joystick* joystick = SDL_JoystickFromInstanceID(id);
		std::shared_ptr<SDLGamepad> gamepad = SDLGamepad::GetSDLGamepad(id);
		if (joystick == nullptr || gamepad == nullptr) continue;
		PollJoystick(joystick, *gamepad, input_poll.joysticks[id]);
	}
#endif
}

void gdxsv_input_update_joysticks() {
	GamepadDevice::RampAnalog();
	gdxsv_input_poll_joysticks();
}

bool gdxsv_input_digital_axis(u32 state, u32 negDir, u32 posDir, s16& joystick) {
	if (!gdxsv_enabled()) return false;
	const u32 pressed = state & (negDir | posDir);
	if (pressed == 0 || pressed == (negDir | posDir))
		joystick = 0;
	else if (pressed == negDir)
		joystick = -32768;
	else
		joystick = 32767;
	return true;
}
