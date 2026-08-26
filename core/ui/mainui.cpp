/*
	Copyright 2020 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "imgui_driver.h"
#include "profiler/fc_profiler.h"
#include "network/ggpo.h"
#include "oslib/i18n.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include "sleep.h"
#include "gdxsv/gdxsv_emu_hooks.h"

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

int64_t get_period() {
	const auto mode = config::FixedFrequency.get();
	// Native NTSC/VGA
	if (mode == 2 ||
		(mode == 1 && (config::Cable == 0 || config::Cable == 1)) ||
		(mode == 1 && config::Cable == 3 && (config::Broadcast == 0 || config::Broadcast == 4)))
		return 16683; // 1/59.94
	// Approximate VGA
	if (mode == 3) return 16666; // 1/60
	// PAL
	if (mode == 4 || (mode == 1 && config::Cable == 3))
		return 20000; // 1/50
	// Half Native NTSC/VGA
	if (mode == 5) return 33333; // 1/30
	return 16683;
}

// The nominal period, plus whatever trim replay/Live Spectate is asking for.
// The trim is zero outside those modes.
static int64_t get_trimmed_period() {
	const int64_t trim = gdxsv_frame_period_trim_us;
	return std::max<int64_t>(1000, get_period() + trim);
}

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	os_UpdateInputState();

	if (gui_is_open())
	{
		try {
			gui_display_ui();
		} catch (const FlycastException& e) {
			// Assume this is a graphics API issue
			forceReinit = true;
			return false;
		}
#ifndef TARGET_IPHONE
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
		} catch (const RendererException& e) {
			gui_error(i18n::Ts("Renderer error:") + "\n" + e.what() + "\n\n"
					+ i18n::Ts("The game has been paused but it is recommended to restart Flycast"));
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			return false;
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error(i18n::T("Renderer initialization failed.\nPlease select a different graphics API"));
	}
}

void mainui_term()
{
	rend_term_renderer();
}

void mainui_loop(bool forceStart)
{
	ThreadName _("Flycast-rend");
	if (forceStart)
		mainui_enabled = true;
	mainui_init();
	RenderType currentRenderer = config::RendererType;
	int currentDupeFrames = config::DupeFrames;

	set_timer_resolution();
	std::chrono::time_point<std::chrono::steady_clock> start;

	auto getElapsed = [&start]() {
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count();
	};
	auto fixedFrequencyWait = [&start, &getElapsed]() {
		if (!config::FixedFrequency || gui_is_open() || settings.input.fastForwardMode)
			return;

		const auto period = get_trimmed_period();
		int64_t overSlept = 0;

		auto remaining = period - getElapsed();
		if (remaining > 0)
			overSlept = sleep_and_busy_wait(remaining);

		start = std::chrono::steady_clock::now();
		if (1000 <= overSlept)
			WARN_LOG(RENDERER, "FixedFrequency: Over slept %d [us]", overSlept);

		sleep_benchmark_periodic_report();
	};

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");
		const auto rendered = mainui_rend_frame();

		if (rendered && imguiDriver != nullptr)
		{
			try {
				imguiDriver->present();
			} catch (const FlycastException& e) {
				forceReinit = true;
			}
		}
		if (imguiDriver == nullptr)
			forceReinit = true;

		if (currentDupeFrames != config::DupeFrames) {
			forceReinit = true;
			currentDupeFrames = config::DupeFrames;
		}

		if (config::RendererType != currentRenderer || forceReinit)
		{
			mainui_term();
			int prevApi = isOpenGL(currentRenderer) ? 0 : isVulkan(currentRenderer) ? 1 : currentRenderer == RenderType::DirectX9 ? 2 : 3;
			int newApi = isOpenGL(config::RendererType) ? 0 : isVulkan(config::RendererType) ? 1 : config::RendererType == RenderType::DirectX9 ? 2 : 3;
			if (newApi != prevApi || forceReinit)
			{
				try {
					switchRenderApi();
				} catch (const FlycastException& e) {
					ERROR_LOG(RENDERER, "switchRenderApi failed: %s", e.what());
					if (prevApi == newApi)
						// fatal
						throw;
					// try to go back to the previous API
					config::RendererType = currentRenderer;
					try {
						switchRenderApi();
					} catch (const FlycastException& e) {
						ERROR_LOG(RENDERER, "Falling back to previous renderer also failed: %s", e.what());
						// fatal
						throw;
					}
				}
			}
			mainui_init();
			forceReinit = false;
			currentRenderer = config::RendererType;
		}

		gdxsv_emu_mainui_loop();

		if (rendered)
			fixedFrequencyWait();

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	reset_timer_resolution();
	mainui_term();
}

void mainui_start()
{
	mainui_enabled = true;
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}
