/*
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
// gdxsv: added by the fork (docs/gdxsv/upstream.md)
#include "render_thread.h"
#include "mainui.h"
#include "gui.h"
#include "imgui_driver.h"
#include "emulator.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "wsi/context.h"
#include "gdxsv/gdxsv_emu_hooks.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#ifdef USE_SDL
#include <SDL.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace renderthread
{

static std::thread thread;
static std::atomic<bool> stopRequested;
static std::atomic<bool> done;
static std::exception_ptr error;
static bool reinit;
static std::atomic<bool> resizePending;
static std::atomic<bool> stylePending;
static thread_local bool onRenderThread;
static std::mutex mainTasksMutex;
static std::vector<std::function<void()>> mainTasks;

static void wakeMainThread()
{
#ifdef USE_SDL
	static const u32 type = SDL_RegisterEvents(1);
	if (type != (u32)-1)
	{
		SDL_Event event{};
		event.type = type;
		SDL_PushEvent(&event);
	}
#endif
}

// Headless: no context
static void makeCurrent(bool current)
{
	if (GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->makeCurrent(current);
}

static void runMainTasks()
{
	std::vector<std::function<void()>> tasks;
	{
		std::lock_guard<std::mutex> lock(mainTasksMutex);
		tasks.swap(mainTasks);
	}
	for (auto& task : tasks)
		task();
}

static void applyDisplayChange()
{
	if (resizePending.exchange(false) && GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->resize();
	if (stylePending.exchange(false))
		gui_updateStyle();
}

static void loop()
{
	ThreadName _("Flycast-render");
	gdxsv_emu_thread_start();
	onRenderThread = true;
	INFO_LOG(RENDERER, "Render thread started");
	makeCurrent(true);
	// TEMPDBG: where the render thread spends its time, for any renderer
	using DbgClock = std::chrono::steady_clock;
	const auto dbgMs = [](DbgClock::time_point from, DbgClock::time_point to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
	DbgClock::time_point dbgWindow = DbgClock::now(), dbgLastFrame = dbgWindow;
	int dbgFrames = 0;
	double dbgReadySum = 0, dbgRenderSum = 0, dbgRenderMax = 0, dbgPresentSum = 0, dbgPresentMax = 0, dbgPeriodMax = 0;
	bool dbgVSync = config::VSync && !settings.input.fastForwardMode;
	try {
		while (!stopRequested && emu.running() && !gui_is_open())
		{
			applyDisplayChange();
			const auto dbgStart = DbgClock::now();
			if (GraphicsContext::Instance() != nullptr)
				GraphicsContext::Instance()->waitPresentReady();
			const auto dbgReady = DbgClock::now();
			if (!emu.render())
				continue;
			const auto dbgRendered = DbgClock::now();
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
			try {
				imguiDriver->present();
			} catch (const FlycastException&) {
				reinit = true;
				break;
			}
			MainFrameCount++;
			gdxsv_emu_render_presented();
			// TEMPDBG
			const auto dbgPresented = DbgClock::now();
			dbgFrames++;
			dbgReadySum += dbgMs(dbgStart, dbgReady);
			dbgRenderSum += dbgMs(dbgReady, dbgRendered);
			dbgRenderMax = std::max(dbgRenderMax, dbgMs(dbgReady, dbgRendered));
			dbgPresentSum += dbgMs(dbgRendered, dbgPresented);
			dbgPresentMax = std::max(dbgPresentMax, dbgMs(dbgRendered, dbgPresented));
			dbgPeriodMax = std::max(dbgPeriodMax, dbgMs(dbgLastFrame, dbgPresented));
			dbgLastFrame = dbgPresented;
			const bool vsyncNow = config::VSync && !settings.input.fastForwardMode;
			if (dbgFrames >= 600 || vsyncNow != dbgVSync)
			{
				NOTICE_LOG(RENDERER, "TEMPDBG render thread renderer=%d vsync=%d frames=%d period avg=%.1f max=%.1f ready avg=%.1f render avg=%.1f max=%.1f present avg=%.1f max=%.1f ms",
						(int)config::RendererType.get(), dbgVSync ? 1 : 0, dbgFrames, dbgMs(dbgWindow, dbgPresented) / dbgFrames, dbgPeriodMax, dbgReadySum / dbgFrames,
						dbgRenderSum / dbgFrames, dbgRenderMax, dbgPresentSum / dbgFrames, dbgPresentMax);
				dbgWindow = dbgPresented;
				dbgFrames = 0;
				dbgReadySum = dbgRenderSum = dbgRenderMax = dbgPresentSum = dbgPresentMax = dbgPeriodMax = 0;
				dbgVSync = vsyncNow;
			}
		}
	} catch (...) {
		error = std::current_exception();
	}
	makeCurrent(false);
	done = true;
	wakeMainThread();
}

void start()
{
	if (thread.joinable())
		return;
	stopRequested = false;
	done = false;
	error = nullptr;
	reinit = false;
	makeCurrent(false);
	thread = std::thread(loop);
}

bool started() {
	return thread.joinable();
}

bool finished() {
	return done;
}

std::exception_ptr stop()
{
	if (!thread.joinable())
		return nullptr;
	stopRequested = true;
#ifdef __APPLE__
	// SDL's Cocoa GL swap waits for the main thread after a window change ([NSOpenGLContext update] by
	// dispatch_sync): run the main queue until the render thread ends, or both wait for each other
	while (!done)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
	thread.join();
	makeCurrent(true);
	runMainTasks();
	// a change that came while it stopped
	applyDisplayChange();
	done = false;
	std::exception_ptr e = error;
	error = nullptr;
	return e;
}

bool current() {
	return onRenderThread;
}

bool takeReinit()
{
	bool r = reinit;
	reinit = false;
	return r;
}

void waitEvents(int timeoutMs)
{
#ifdef USE_SDL
	SDL_WaitEventTimeout(nullptr, timeoutMs);
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

void displayChanged(bool scaleChanged)
{
	if (thread.joinable())
	{
		resizePending = true;
		if (scaleChanged)
			stylePending = true;
		return;
	}
	if (GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->resize();
	if (scaleChanged)
		gui_updateStyle();
}

void runOnMainThread(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(mainTasksMutex);
		mainTasks.push_back(std::move(task));
	}
	stopRequested = true;
}

} // namespace renderthread
