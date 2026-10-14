#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "gdxsv/gdxsv_input_hooks.h"
#ifdef USE_SDL
#include "sdl/sdl_gamepad.h"

namespace {

class RecordingGamepad : public SDLGamepad {
public:
	RecordingGamepad(int idx, SDL_Joystick *joystick) : SDLGamepad(0, idx, joystick) {}
	bool gamepad_btn_input(u32 code, bool pressed) override {
		events.push_back("b" + std::to_string(code) + "=" + std::to_string(pressed));
		return true;
	}
	bool gamepad_axis_input(u32 code, int value) override {
		events.push_back("a" + std::to_string(code) + "=" + std::to_string(value));
		return true;
	}
	std::vector<std::string> Take() { return std::move(events); }
	std::vector<std::string> events;
};

}  // namespace

TEST(GdxsvInputTest, PollReportsJoystickChanges) {
	ASSERT_EQ(0, SDL_InitSubSystem(SDL_INIT_JOYSTICK));
	const int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_UNKNOWN, 2, 4, 1);
	ASSERT_GE(idx, 0);
	SDL_Joystick *joystick = SDL_JoystickOpen(idx);
	ASSERT_NE(nullptr, joystick);
	auto gamepad = std::make_shared<RecordingGamepad>(idx, joystick);
	SDLGamepad::AddSDLGamepad(gamepad);

	gdxsv_input_poll_joysticks();
	EXPECT_EQ(std::vector<std::string>{}, gamepad->Take());

	SDL_JoystickSetVirtualButton(joystick, 1, 1);
	SDL_JoystickSetVirtualAxis(joystick, 0, 1000);
	SDL_JoystickSetVirtualHat(joystick, 0, SDL_HAT_UP | SDL_HAT_RIGHT);
	gdxsv_input_poll_joysticks();
	EXPECT_EQ((std::vector<std::string>{"b1=1", "a0=1000", "b256=1", "b257=0", "b258=0", "b259=1"}), gamepad->Take());

	gdxsv_input_poll_joysticks();
	EXPECT_EQ(std::vector<std::string>{}, gamepad->Take());

	SDL_JoystickSetVirtualButton(joystick, 1, 0);
	SDL_JoystickSetVirtualButton(joystick, 3, 1);
	SDL_JoystickSetVirtualHat(joystick, 0, SDL_HAT_DOWN);
	gdxsv_input_poll_joysticks();
	EXPECT_EQ((std::vector<std::string>{"b1=0", "b3=1", "b256=0", "b257=1", "b258=0", "b259=0"}), gamepad->Take());

	gamepad->close();
	SDL_JoystickDetachVirtual(idx);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}
#endif

TEST(GdxsvInputTest, DigitalAxisIsUpstreamWhenGdxsvIsOff) {
	s16 joystick = 123;
	EXPECT_FALSE(gdxsv_input_digital_axis(1, 1, 2, joystick));
	EXPECT_EQ(123, joystick);
}
