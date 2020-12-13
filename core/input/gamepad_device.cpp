/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "gamepad_device.h"
#include "cfg/cfg.h"
#include "oslib/oslib.h"
#include "rend/gui.h"
#include "emulator.h"
#include "gdxsv/gdxsv.h"

#include <algorithm>
#include <climits>

#define MAPLE_PORT_CFG_PREFIX "maple_"

// Gamepads
u16 kcode[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
s8 joyx[4];
s8 joyy[4];
s8 joyrx[4];
s8 joyry[4];
u8 rt[4];
u8 lt[4];

std::vector<std::shared_ptr<GamepadDevice>> GamepadDevice::_gamepads;
std::mutex GamepadDevice::_gamepads_mutex;
bool fast_forward_mode;

#ifdef TEST_AUTOMATION
#include "hw/sh4/sh4_sched.h"
static FILE *record_input;
#endif

bool GamepadDevice::gamepad_btn_input(u32 code, bool pressed)
{
	if (_input_detected != nullptr && _detecting_button
			&& os_GetSeconds() >= _detection_start_time && pressed)
	{
		_input_detected(code);
		_input_detected = nullptr;
		return true;
	}
	if (!input_mapper || _maple_port < 0 || _maple_port >= (int)ARRAY_SIZE(kcode))
		return false;
	DreamcastKey key = input_mapper->get_button_id(code);
	if (key == EMU_BTN_NONE)
		return false;

	if (key < 0x10000)
	{
		if (pressed)
		{
			kcode[_maple_port] &= ~(u16)key;
			// Avoid two opposite dpad keys being pressed simultaneously
			switch (key)
			{
			case DC_DPAD_UP:
				kcode[_maple_port] |= (u16)DC_DPAD_DOWN;
				break;
			case DC_DPAD_DOWN:
				kcode[_maple_port] |= (u16)DC_DPAD_UP;
				break;
			case DC_DPAD_LEFT:
				kcode[_maple_port] |= (u16)DC_DPAD_RIGHT;
				break;
			case DC_DPAD_RIGHT:
				kcode[_maple_port] |= (u16)DC_DPAD_LEFT;
				break;
			case DC_DPAD2_UP:
				kcode[_maple_port] |= (u16)DC_DPAD2_DOWN;
				break;
			case DC_DPAD2_DOWN:
				kcode[_maple_port] |= (u16)DC_DPAD2_UP;
				break;
			case DC_DPAD2_LEFT:
				kcode[_maple_port] |= (u16)DC_DPAD2_RIGHT;
				break;
			case DC_DPAD2_RIGHT:
				kcode[_maple_port] |= (u16)DC_DPAD2_LEFT;
				break;
			default:
				break;
			}
		}
		else
			kcode[_maple_port] |= (u16)key;
#ifdef TEST_AUTOMATION
		if (record_input != NULL)
			fprintf(record_input, "%ld button %x %04x\n", sh4_sched_now64(), _maple_port, kcode[_maple_port]);
#endif
	}
	else
	{
		switch (key)
		{
		case EMU_BTN_ESCAPE:
			if (pressed && !gdxsv.InGame())
				dc_exit();
			break;
		case EMU_BTN_MENU:
			if (pressed && !gdxsv.InGame())
				gui_open_settings();
			break;
		case EMU_BTN_FFORWARD:
			if (pressed && !gdxsv.InGame())
				fast_forward_mode = !fast_forward_mode;
			break;
		case EMU_BTN_TRIGGER_LEFT:
			lt[_maple_port] = pressed ? 255 : 0;
			break;
		case EMU_BTN_TRIGGER_RIGHT:
			rt[_maple_port] = pressed ? 255 : 0;
			break;
		case EMU_BTN_ANA_UP:
			joyy[_maple_port] = pressed ? -128 : 0;
			break;
		case EMU_BTN_ANA_DOWN:
			joyy[_maple_port] = pressed ? 127 : 0;
			break;
		case EMU_BTN_ANA_LEFT:
			joyx[_maple_port] = pressed ? -128 : 0;
			break;
		case EMU_BTN_ANA_RIGHT:
			joyx[_maple_port] = pressed ? 127 : 0;
			break;
		default:
			return false;
		}
	}

	DEBUG_LOG(INPUT, "%d: BUTTON %s %x -> %d. kcode=%x", _maple_port, pressed ? "down" : "up", code, key, kcode[_maple_port]);
	return true;
}

bool GamepadDevice::gamepad_axis_input(u32 code, int value)
{
	s32 v;
	if (input_mapper->get_axis_inverted(code))
		v = (get_axis_min_value(code) + get_axis_range(code) - value) * 255 / get_axis_range(code) - 128;
	else
		v = (value - get_axis_min_value(code)) * 255 / get_axis_range(code) - 128; //-128 ... +127 range
	if (_input_detected != NULL && !_detecting_button 
			&& os_GetSeconds() >= _detection_start_time && (v >= 64 || v <= -64))
	{
		_input_detected(code);
		_input_detected = NULL;
		return true;
	}
	if (!input_mapper || _maple_port < 0 || _maple_port >= (int)ARRAY_SIZE(kcode))
		return false;
	DreamcastKey key = input_mapper->get_axis_id(code);

	if ((int)key < 0x10000)
	{
		kcode[_maple_port] |= key | (key << 1);
		if (v <= -64)
			kcode[_maple_port] &= ~key;
		else if (v >= 64)
			kcode[_maple_port] &= ~(key << 1);

		// printf("Mapped to %d %d %d\n",mo,kcode[port]&mo,kcode[port]&(mo*2));
	}
	else if (((int)key >> 16) == 1)	// Triggers
	{
		//printf("T-AXIS %d Mapped to %d -> %d\n",key, value, v + 128);

		if (key == DC_AXIS_LT)
			lt[_maple_port] = (u8)(v + 128);
		else if (key == DC_AXIS_RT)
			rt[_maple_port] = (u8)(v + 128);
		else
			return false;
	}
	else if (((int)key >> 16) == 2) // Analog axes
	{
		//printf("AXIS %d Mapped to %d -> %d\n", key, value, v);
		s8 *this_axis;
		s8 *other_axis;
		switch (key)
		{
		case DC_AXIS_X:
			this_axis = &joyx[_maple_port];
			other_axis = &joyy[_maple_port];
			break;

		case DC_AXIS_Y:
			this_axis = &joyy[_maple_port];
			other_axis = &joyx[_maple_port];
			break;

		case DC_AXIS_X2:
			this_axis = &joyrx[_maple_port];
			other_axis = &joyry[_maple_port];
			break;

		case DC_AXIS_Y2:
			this_axis = &joyry[_maple_port];
			other_axis = &joyrx[_maple_port];
			break;

		default:
			return false;
		}
		// Radial dead zone
		// FIXME compute both axes at the same time
		if ((float)(v * v + *other_axis * *other_axis) < input_mapper->dead_zone * input_mapper->dead_zone * 128.f * 128.f)
		{
			*this_axis = 0;
			*other_axis = 0;
		}
		else
			*this_axis = (s8)v;
	}
	else if (((int)key >> 16) == 4) // Map triggers to digital buttons
	{
		if (v <= -64)
			kcode[_maple_port] |=  (key & ~0x40000); // button released
		else if (v >= 64)
			kcode[_maple_port] &= ~(key & ~0x40000); // button pressed
	}
	else
		return false;

	return true;
}

int GamepadDevice::get_axis_min_value(u32 axis) {
	auto it = axis_min_values.find(axis);
	if (it == axis_min_values.end()) {
		load_axis_min_max(axis);
		it = axis_min_values.find(axis);
		if (it == axis_min_values.end())
			return INT_MIN;
	}
	return it->second;
}

unsigned int GamepadDevice::get_axis_range(u32 axis) {
	auto it = axis_ranges.find(axis);
	if (it == axis_ranges.end()) {
		load_axis_min_max(axis);
		it = axis_ranges.find(axis);
		if (it == axis_ranges.end())
			return UINT_MAX;
	}
	return it->second;
}

std::string GamepadDevice::make_mapping_filename(bool instance)
{
	std::string mapping_file = api_name() + "_" + name();
	if (instance)
		mapping_file += "-" + _unique_id;
	std::replace(mapping_file.begin(), mapping_file.end(), '/', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '\\', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), ':', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '?', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '*', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '|', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '"', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '<', '-');
	std::replace(mapping_file.begin(), mapping_file.end(), '>', '-');
	mapping_file += ".cfg";

	return mapping_file;
}

bool GamepadDevice::find_mapping(const char *custom_mapping /* = nullptr */)
{
	std::string mapping_file;
	if (custom_mapping != nullptr)
		mapping_file = custom_mapping;
	else
		mapping_file = make_mapping_filename(true);

	input_mapper = InputMapping::LoadMapping(mapping_file.c_str());
	if (!input_mapper && custom_mapping == nullptr)
	{
		mapping_file = make_mapping_filename(false);
		input_mapper = InputMapping::LoadMapping(mapping_file.c_str());
	}
	return !!input_mapper;
}

int GamepadDevice::GetGamepadCount()
{
	_gamepads_mutex.lock();
	int count = _gamepads.size();
	_gamepads_mutex.unlock();
	return count;
}

std::shared_ptr<GamepadDevice> GamepadDevice::GetGamepad(int index)
{
	_gamepads_mutex.lock();
	std::shared_ptr<GamepadDevice> dev;
	if (index >= 0 && index < (int)_gamepads.size())
		dev = _gamepads[index];
	else
		dev = NULL;
	_gamepads_mutex.unlock();
	return dev;
}

void GamepadDevice::save_mapping()
{
	if (!input_mapper)
		return;
	std::string filename = make_mapping_filename();
	InputMapping::SaveMapping(filename.c_str(), input_mapper);
}

void UpdateVibration(u32 port, float power, float inclination, u32 duration_ms)
{
	int i = GamepadDevice::GetGamepadCount() - 1;
	for ( ; i >= 0; i--)
	{
		std::shared_ptr<GamepadDevice> gamepad = GamepadDevice::GetGamepad(i);
		if (gamepad != NULL && gamepad->maple_port() == (int)port && gamepad->is_rumble_enabled())
			gamepad->rumble(power, inclination, duration_ms);
	}
}

void GamepadDevice::detect_btn_input(input_detected_cb button_pressed)
{
	_input_detected = button_pressed;
	_detecting_button = true;
	_detection_start_time = os_GetSeconds() + 0.2;
}

void GamepadDevice::detect_axis_input(input_detected_cb axis_moved)
{
	_input_detected = axis_moved;
	_detecting_button = false;
	_detection_start_time = os_GetSeconds() + 0.2;
}

#ifdef TEST_AUTOMATION
static FILE *get_record_input(bool write)
{
	if (write && !cfgLoadBool("record", "record_input", false))
		return NULL;
	if (!write && !cfgLoadBool("record", "replay_input", false))
		return NULL;
	std::string game_dir = settings.imgread.ImagePath;
	size_t slash = game_dir.find_last_of("/");
	size_t dot = game_dir.find_last_of(".");
	std::string input_file = "scripts/" + game_dir.substr(slash + 1, dot - slash) + "input";
	return fopen(input_file.c_str(), write ? "w" : "r");
}
#endif

void GamepadDevice::Register(std::shared_ptr<GamepadDevice> gamepad)
{
	int maple_port = cfgLoadInt("input",
			(MAPLE_PORT_CFG_PREFIX + gamepad->unique_id()).c_str(), 12345);
	if (maple_port != 12345)
		gamepad->set_maple_port(maple_port);
#ifdef TEST_AUTOMATION
	if (record_input == NULL)
	{
		record_input = get_record_input(true);
		if (record_input != NULL)
			setbuf(record_input, NULL);
	}
#endif
	_gamepads_mutex.lock();
	_gamepads.push_back(gamepad);
	_gamepads_mutex.unlock();
}

void GamepadDevice::Unregister(std::shared_ptr<GamepadDevice> gamepad)
{
	gamepad->save_mapping();
	_gamepads_mutex.lock();
	for (auto it = _gamepads.begin(); it != _gamepads.end(); it++)
		if (*it == gamepad) {
			_gamepads.erase(it);
			break;
		}
	_gamepads_mutex.unlock();
}

void GamepadDevice::SaveMaplePorts()
{
	for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++)
	{
		std::shared_ptr<GamepadDevice> gamepad = GamepadDevice::GetGamepad(i);
		if (gamepad != NULL && !gamepad->unique_id().empty())
			cfgSaveInt("input", (MAPLE_PORT_CFG_PREFIX + gamepad->unique_id()).c_str(), gamepad->maple_port());
	}
}

#ifdef TEST_AUTOMATION
static bool replay_inited;
FILE *replay_file;
u64 next_event;
u32 next_port;
u32 next_kcode;
bool do_screenshot;

void replay_input()
{
	if (!replay_inited)
	{
		replay_file = get_record_input(false);
		replay_inited = true;
	}
	u64 now = sh4_sched_now64();
	if (settings.bios.UseReios)
	{
		// Account for the swirl time
		if (settings.dreamcast.broadcast == 0)
			now = std::max((int64_t)now - 2152626532L, 0L);
		else
			now = std::max((int64_t)now - 2191059108L, 0L);
	}
	if (replay_file == NULL)
	{
		if (next_event > 0 && now - next_event > SH4_MAIN_CLOCK * 5)
			die("Automation time-out after 5 s\n");
		return;
	}
	while (next_event <= now)
	{
		if (next_event > 0)
			kcode[next_port] = next_kcode;

		char action[32];
		if (fscanf(replay_file, "%ld %s %x %x\n", &next_event, action, &next_port, &next_kcode) != 4)
		{
			fclose(replay_file);
			replay_file = NULL;
			NOTICE_LOG(INPUT, "Input replay terminated");
			do_screenshot = true;
			break;
		}
	}
}
#endif
