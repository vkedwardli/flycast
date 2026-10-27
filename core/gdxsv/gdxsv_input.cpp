// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_input_hooks.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "emulator.h"
#include "gdxsv.h"
#include "gdxsv_emu_hooks.h"
#ifdef USE_SDL
#include "sdl/sdl_gamepad.h"
#endif

#ifdef USE_SDL
namespace {

// Test only: an SDL virtual joystick that plays a script, one change per line:
// "<vblank> b|a|h <index> <value>" (button, axis, hat), vblanks counted from the first.
struct TestJoystickStep {
	u64 vblank;
	char kind;
	int index;
	int value;
};

struct TestJoystick {
	SDL_Joystick* joystick = nullptr;
	std::vector<TestJoystickStep> steps;
	size_t next = 0;
	u64 vblank = 0;
};

TestJoystick test_joystick;

void TestJoystickVBlank(Event, void*) {
	TestJoystick& t = test_joystick;
	t.vblank++;
	for (; t.next < t.steps.size() && t.steps[t.next].vblank <= t.vblank; t.next++) {
		const TestJoystickStep& s = t.steps[t.next];
		if (s.kind == 'b')
			SDL_JoystickSetVirtualButton(t.joystick, s.index, (Uint8)s.value);
		else if (s.kind == 'a')
			SDL_JoystickSetVirtualAxis(t.joystick, s.index, (Sint16)s.value);
		else
			SDL_JoystickSetVirtualHat(t.joystick, s.index, (Uint8)s.value);
		NOTICE_LOG(INPUT, "test joystick: vblank %llu %c %d %d", (unsigned long long)t.vblank, s.kind, s.index, s.value);
	}
}

}  // namespace
#endif

bool gdxsv_input_toggle_fast_forward() { return gdxsv.HookFastForward(); }

void gdxsv_input_attach_test_joystick() {
#ifdef USE_SDL
	const char* path = getenv("GDXSV_TEST_JOYSTICK");
	if (path == nullptr || test_joystick.joystick != nullptr) return;
	FILE* f = fopen(path, "r");
	if (f == nullptr) {
		WARN_LOG(INPUT, "test joystick: cannot open %s", path);
		return;
	}
	unsigned long long vblank;
	char kind;
	int index, value;
	while (fscanf(f, "%llu %c %d %d", &vblank, &kind, &index, &value) == 4)
		test_joystick.steps.push_back({vblank, kind, index, value});
	fclose(f);

	SDL_VirtualJoystickDesc desc{};
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	desc.nhats = 1;
	desc.name = "gdxsv test joystick";
	const int device = SDL_JoystickAttachVirtualEx(&desc);
	test_joystick.joystick = device < 0 ? nullptr : SDL_JoystickOpen(device);
	if (test_joystick.joystick == nullptr) {
		WARN_LOG(INPUT, "test joystick: %s", SDL_GetError());
		return;
	}
	NOTICE_LOG(INPUT, "test joystick: attached, %d steps from %s", (int)test_joystick.steps.size(), path);
	EventManager::listen(Event::VBlank, TestJoystickVBlank);
#endif
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
