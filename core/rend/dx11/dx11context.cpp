/*
	Copyright 2021 flyinghead

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
#include "dx11context.h"
#ifndef LIBRETRO
#include "rend/osd.h"
#ifdef USE_SDL
#include "sdl/sdl.h"
#endif
#include "hw/pvr/Renderer_if.h"
#include "emulator.h"
#include "dx11_driver.h"
#include "gdxsv/gdxsv_video_hooks.h"
#include "imgui_impl_dx11.h"
#include <dxgi1_6.h>
#ifdef TARGET_UWP
#include <windows.h>
#include <gamingdeviceinformation.h>
#endif

#include "nowide/stackstring.hpp"

#ifndef TARGET_UWP
// TEMPDBG: Windows 7 swap chain diagnostics, not for merging
namespace {
const char *dbgEffect(DXGI_SWAP_EFFECT effect)
{
	switch (effect)
	{
	case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
	case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
	case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
	case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
	default: return "?";
	}
}

template<typename T>
int dbgHas(IUnknown *object)
{
	T *p = nullptr;
	if (FAILED(object->QueryInterface(__uuidof(T), (void **)&p)))
		return 0;
	p->Release();
	return 1;
}

void dbgLogModule(const wchar_t *name)
{
	wchar_t path[MAX_PATH] = L"(not loaded)";
	if (HMODULE module = GetModuleHandleW(name))
		GetModuleFileNameW(module, path, MAX_PATH);
	nowide::stackstring s;
	s.convert(path);
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 module %s", s.get());
}

void dbgLogEnvironment(HWND window, IDXGIFactory1 *factory, ID3D11Device *device, IDXGIAdapter *adapter, D3D_FEATURE_LEVEL featureLevel, bool allowTearing)
{
	OSVERSIONINFOW os{};
	os.dwOSVersionInfoSize = sizeof(os);
	auto rtlGetVersion = (LONG (WINAPI *)(OSVERSIONINFOW *))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (rtlGetVersion != nullptr)
		rtlGetVersion(&os);
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 os %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
	dbgLogModule(L"dxgi.dll");
	dbgLogModule(L"d3d11.dll");
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 factory2=%d factory3=%d factory4=%d factory5=%d factory6=%d",
			dbgHas<IDXGIFactory2>(factory), dbgHas<IDXGIFactory3>(factory), dbgHas<IDXGIFactory4>(factory),
			dbgHas<IDXGIFactory5>(factory), dbgHas<IDXGIFactory6>(factory));
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 device featureLevel=%x flags=%x device1=%d dxgiDevice2=%d", featureLevel, device->GetCreationFlags(),
			dbgHas<ID3D11Device1>(device), dbgHas<IDXGIDevice2>(device));

	DXGI_ADAPTER_DESC desc{};
	adapter->GetDesc(&desc);
	LARGE_INTEGER umd{};
	HRESULT umdHr = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
	int outputs = 0;
	for (ComPtr<IDXGIOutput> output; SUCCEEDED(adapter->EnumOutputs(outputs, &output.get())); output.reset())
		outputs++;
	nowide::stackstring name;
	name.convert(desc.Description);
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 adapter '%s' vendor=%x device=%x driver=%d.%d.%d.%d (hr=%x) outputs=%d", name.get(), desc.VendorId, desc.DeviceId,
			HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart), umdHr, outputs);

	BOOL composition = FALSE;
	HRESULT dwmHr = E_FAIL;
	if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
	{
		auto isEnabled = (HRESULT (WINAPI *)(BOOL *))(void *)GetProcAddress(dwm, "DwmIsCompositionEnabled");
		if (isEnabled != nullptr)
			dwmHr = isEnabled(&composition);
		FreeLibrary(dwm);
	}
	RECT client{};
	GetClientRect(window, &client);
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 dwm composition=%d (hr=%x) window isWindow=%d visible=%d style=%lx exStyle=%lx classStyle=%lx client=%ldx%ld sameThread=%d threaded=%d tearing=%d",
			composition, dwmHr, IsWindow(window) ? 1 : 0, IsWindowVisible(window) ? 1 : 0, (unsigned long)GetWindowLongPtrW(window, GWL_STYLE),
			(unsigned long)GetWindowLongPtrW(window, GWL_EXSTYLE), (unsigned long)GetClassLongPtrW(window, GCL_STYLE), client.right, client.bottom,
			GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId() ? 1 : 0, config::ThreadedRendering ? 1 : 0, allowTearing ? 1 : 0);
}

// Each case gets a new window: a window that already had a swap chain rejects some others.
void dbgProbeSwapChains(IDXGIFactory1 *factory, IDXGIFactory2 *factory2, ID3D11Device *device, ID3D11DeviceContext *context)
{
	WNDCLASSW wc{};
	wc.style = CS_OWNDC | CS_BYTEALIGNCLIENT;
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"FlycastDx11Probe";
	RegisterClassW(&wc);
	auto newWindow = [&]() {
		return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
	};
	const DXGI_SWAP_EFFECT effects[] = { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_DISCARD };
	const DXGI_ALPHA_MODE alphas[] = { DXGI_ALPHA_MODE_IGNORE, DXGI_ALPHA_MODE_UNSPECIFIED };
	const UINT bufferCounts[] = { 1, 2, 3 };
	for (DXGI_SWAP_EFFECT effect : effects)
		for (int waitable = 0; waitable < 2; waitable++)
			for (UINT buffers : bufferCounts)
			{
				const UINT flags = waitable != 0 ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
				if (factory2 != nullptr)
					for (DXGI_ALPHA_MODE alpha : alphas)
					{
						HWND hwnd = newWindow();
						DXGI_SWAP_CHAIN_DESC1 desc{};
						desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
						desc.SwapEffect = effect;
						desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
						desc.BufferCount = buffers;
						desc.SampleDesc.Count = 1;
						desc.AlphaMode = alpha;
						desc.Flags = flags;
						ComPtr<IDXGISwapChain1> sc;
						HRESULT hr = factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &sc.get());
						NOTICE_LOG(RENDERER, "TEMPDBG dx11 probe api=hwnd effect=%s buffers=%u alpha=%s waitable=%d window=%d hr=%x", dbgEffect(effect), buffers,
								alpha == DXGI_ALPHA_MODE_IGNORE ? "IGNORE" : "UNSPECIFIED", waitable, hwnd != nullptr ? 1 : 0, hr);
						sc.reset();
						context->ClearState();
						context->Flush();
						if (hwnd != nullptr)
							DestroyWindow(hwnd);
					}
				HWND hwnd = newWindow();
				DXGI_SWAP_CHAIN_DESC desc{};
				desc.BufferCount = buffers;
				desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.BufferDesc.RefreshRate.Numerator = 60;
				desc.BufferDesc.RefreshRate.Denominator = 1;
				desc.OutputWindow = hwnd;
				desc.Windowed = TRUE;
				desc.SwapEffect = effect;
				desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				desc.SampleDesc.Count = 1;
				desc.Flags = flags;
				ComPtr<IDXGISwapChain> sc;
				HRESULT hr = factory->CreateSwapChain(device, &desc, &sc.get());
				NOTICE_LOG(RENDERER, "TEMPDBG dx11 probe api=legacy effect=%s buffers=%u waitable=%d window=%d hr=%x", dbgEffect(effect), buffers, waitable, hwnd != nullptr ? 1 : 0, hr);
				sc.reset();
				context->ClearState();
				context->Flush();
				if (hwnd != nullptr)
					DestroyWindow(hwnd);
			}
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
}
#endif

void DX11Context::Create(void *window, void *display) {
	new DX11Context(window, display);
}

DX11Context::DX11Context(void *window, void *display)
	: GraphicsContext(window, display)
{
	if (!init())
		throw FlycastException("DX11 initialization failed");
}

DX11Context::~DX11Context() {
	term();
}

bool DX11Context::init(bool keepCurrentWindow)
{
	NOTICE_LOG(RENDERER, "DX11 Context initializing");
#ifdef USE_SDL
	if (!keepCurrentWindow && !sdl_recreate_window(0))
		return false;
#endif
#ifdef TARGET_UWP
	GAMING_DEVICE_MODEL_INFORMATION info {};
	GetGamingDeviceModelInformation(&info);
	if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT && info.deviceId != GAMING_DEVICE_DEVICE_ID_NONE)
	{
		Windows::Graphics::Display::Core::HdmiDisplayInformation^ dispInfo = Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
		Windows::Graphics::Display::Core::HdmiDisplayMode^ displayMode = dispInfo->GetCurrentDisplayMode();
		NOTICE_LOG(RENDERER, "HDMI resolution: %d x %d", displayMode->ResolutionWidthInRawPixels, displayMode->ResolutionHeightInRawPixels);
		settings.display.width = displayMode->ResolutionWidthInRawPixels;
		settings.display.height = displayMode->ResolutionHeightInRawPixels;
		settings.display.uiScale = settings.display.width / 1920.0f * 1.4f;
	}
#endif

	// Use high performance GPU on Windows 10 (1803 or later)
	ComPtr<IDXGIFactory1> dxgiFactory;
	ComPtr<IDXGIFactory6> dxgiFactory6;
	ComPtr<IDXGIAdapter> dxgiAdapter;
	HRESULT hr;
	allowTearing = false;
	hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgiFactory.get());
	if (SUCCEEDED(hr))
	{
		dxgiFactory.as(dxgiFactory6);
		if (dxgiFactory6) 
		{
			dxgiFactory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter), (void **)&dxgiAdapter.get());
			UINT tearing;
			if (SUCCEEDED(dxgiFactory6->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
			                                                 sizeof(tearing))) && tearing != 0)
				allowTearing = true;
			dxgiFactory6.reset();
		}
	}
	dxgiFactory.reset();

	D3D_FEATURE_LEVEL featureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	hr = D3D11CreateDevice(
	    dxgiAdapter.get(), // High performance GPU, or fallback to use the default adapter.
	    dxgiAdapter.get() == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN, // D3D_DRIVER_TYPE_UNKNOWN is required when providing an adapter.
	    nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, // | D3D11_CREATE_DEVICE_DEBUG,
	    featureLevels,
	    ARRAYSIZE(featureLevels),
	    D3D11_SDK_VERSION,
	    &pDevice.get(),
	    &featureLevel,
	    &pDeviceContext.get());
	if (FAILED(hr)) {
		WARN_LOG(RENDERER, "D3D11 device creation failed: %x", hr);
		return false;
	}

	ComPtr<IDXGIDevice2> dxgiDevice;
	pDevice.as(dxgiDevice);
	if (int latency = gdxsv_video_max_frame_latency())
		dxgiDevice->SetMaximumFrameLatency(latency);

	dxgiAdapter.reset();
	dxgiDevice->GetAdapter(&dxgiAdapter.get());
	DXGI_ADAPTER_DESC desc;
	dxgiAdapter->GetDesc(&desc);
	nowide::stackstring wdesc;
	wdesc.convert(desc.Description);
	adapterDesc = wdesc.get();
	adapterVersion = std::to_string(desc.Revision);
	vendorId = desc.VendorId;

	dxgiAdapter->GetParent(__uuidof(IDXGIFactory1), (void **)&dxgiFactory.get());

	ComPtr<IDXGIFactory2> dxgiFactory2;
	dxgiFactory.as(dxgiFactory2);

#ifndef TARGET_UWP
	// TEMPDBG
	dbgLogEnvironment((HWND)window, dxgiFactory, pDevice, dxgiAdapter, featureLevel, allowTearing);
	static bool dbgProbed = false;
	if (!dbgProbed)
	{
		dbgProbed = true;
		dbgProbeSwapChains(dxgiFactory, dxgiFactory2, pDevice, pDeviceContext);
	}
#endif

	if (dxgiFactory2)
	{
		// DX 11.1
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.SampleDesc.Count = 1;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		if (allowTearing)
			desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		// gdxsv: threaded rendering presents the newest frame (waitPresentReady, Present)
		const bool latestFrame = config::ThreadedRendering;
		if (latestFrame)
		{
			desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
			// a free buffer to render to while one is shown and one waits for the vblank
			desc.BufferCount = 3;
		}

#ifdef TARGET_UWP
		desc.Width = settings.display.width;
		desc.Height = settings.display.height;
		hr = dxgiFactory2->CreateSwapChainForCoreWindow(pDevice, (IUnknown *)window, &desc, nullptr, &swapchain1.get());
#else
		hr = dxgiFactory2->CreateSwapChainForHwnd(pDevice, (HWND)window, &desc, nullptr, nullptr, &swapchain1.get());
#endif
		if (SUCCEEDED(hr))
			swapchain1.as(swapchain);
		ComPtr<IDXGISwapChain2> swapchain2; // gdxsv: threaded rendering
		if (SUCCEEDED(hr) && latestFrame && SUCCEEDED(swapchain1.as(swapchain2)))
		{
			swapchain2->SetMaximumFrameLatency(1);
			frameLatencyWaitable = swapchain2->GetFrameLatencyWaitableObject();
			presentWaited = false;
			NOTICE_LOG(RENDERER, "DX11: presenting the newest frame");
		}
	}
	else
	{
		// DX 11.0
		swapchain1.reset();
#ifdef TARGET_UWP
		return false;
#endif
		DXGI_SWAP_CHAIN_DESC desc{};
		desc.BufferCount = 2;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferDesc.RefreshRate.Numerator = 60;
		desc.BufferDesc.RefreshRate.Denominator = 1;
		desc.OutputWindow = (HWND)window;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		desc.BufferCount = 2;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		if (allowTearing)
			desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

		hr = dxgiFactory->CreateSwapChain(pDevice, &desc, &swapchain.get());
	}
#ifndef TARGET_UWP
	// TEMPDBG: the normal request's result, then a swap chain this OS accepts, to see how far the renderer gets
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 normal swap chain factory2=%d threaded=%d hr=%x", dxgiFactory2 ? 1 : 0, config::ThreadedRendering ? 1 : 0, hr);
	if (FAILED(hr) && dxgiFactory2)
	{
		const struct { DXGI_SWAP_EFFECT effect; UINT buffers; DXGI_ALPHA_MODE alpha; } tries[] = {
			{ DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, DXGI_ALPHA_MODE_IGNORE },
			{ DXGI_SWAP_EFFECT_DISCARD, 2, DXGI_ALPHA_MODE_UNSPECIFIED },
			{ DXGI_SWAP_EFFECT_SEQUENTIAL, 2, DXGI_ALPHA_MODE_UNSPECIFIED },
			{ DXGI_SWAP_EFFECT_DISCARD, 1, DXGI_ALPHA_MODE_UNSPECIFIED },
			{ DXGI_SWAP_EFFECT_DISCARD, 2, DXGI_ALPHA_MODE_IGNORE },
		};
		for (const auto& t : tries)
		{
			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SwapEffect = t.effect;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = t.buffers;
			desc.SampleDesc.Count = 1;
			desc.AlphaMode = t.alpha;
			swapchain1.reset();
			hr = dxgiFactory2->CreateSwapChainForHwnd(pDevice, (HWND)window, &desc, nullptr, nullptr, &swapchain1.get());
			NOTICE_LOG(RENDERER, "TEMPDBG dx11 fallback effect=%s buffers=%u hr=%x", dbgEffect(t.effect), t.buffers, hr);
			if (SUCCEEDED(hr))
			{
				swapchain1.as(swapchain);
				break;
			}
		}
	}
	if (SUCCEEDED(hr) && swapchain)
	{
		DXGI_SWAP_CHAIN_DESC used{};
		swapchain->GetDesc(&used);
		NOTICE_LOG(RENDERER, "TEMPDBG dx11 swap chain in use effect=%s buffers=%u flags=%x size=%ux%u waitable=%d", dbgEffect(used.SwapEffect), used.BufferCount, used.Flags,
				used.BufferDesc.Width, used.BufferDesc.Height, frameLatencyWaitable != nullptr ? 1 : 0);
	}
#endif
	if (FAILED(hr)) {
		WARN_LOG(RENDERER, "D3D11 swap chain creation failed: %x", hr);
		pDevice.reset();
		return false;
	}

#ifndef TARGET_UWP
	// Prevent DXGI from monitoring our message queue for ALT+Enter
	dxgiFactory->MakeWindowAssociation((HWND)window, DXGI_MWA_NO_WINDOW_CHANGES);
#endif
	D3D11_FEATURE_DATA_SHADER_CACHE cacheSupport{};
	if (SUCCEEDED(pDevice->CheckFeatureSupport(D3D11_FEATURE_SHADER_CACHE, &cacheSupport, (UINT)sizeof(cacheSupport))))
	{
		_hasShaderCache = cacheSupport.SupportFlags & D3D11_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE;
		if (!_hasShaderCache)
			NOTICE_LOG(RENDERER, "No system-provided shader cache");
	}

	imguiDriver = std::unique_ptr<ImGuiDriver>(new DX11Driver(pDevice, pDeviceContext));
	resize();
	shaders.init(pDevice, &D3DCompile);
	overlay.init(pDevice, pDeviceContext, &shaders, &samplers);
	bool success = checkTextureSupport();
	NOTICE_LOG(RENDERER, "TEMPDBG dx11 init done success=%d perPixel=%d", success ? 1 : 0, hasPerPixel() ? 1 : 0); // TEMPDBG
	if (!success)
		term();
	return success;
}

void DX11Context::term()
{
	NOTICE_LOG(RENDERER, "DX11 Context terminating");
	overlay.term();
	samplers.term();
	shaders.term();
	imguiDriver.reset();
	renderTargetView.reset();
	if (frameLatencyWaitable != nullptr) // gdxsv: threaded rendering
		CloseHandle(frameLatencyWaitable);
	frameLatencyWaitable = nullptr;
	swapchain1.reset();
	swapchain.reset();
	if (pDeviceContext)
	{
		pDeviceContext->ClearState();
		pDeviceContext->Flush();
	}
	pDeviceContext.reset();
	pDevice.reset();
	d3dcompiler = nullptr;
}

void DX11Context::Present()
{
	if (!frameRendered)
		return;
	frameRendered = false;
	// gdxsv: one wait per Present, also for one the render thread did not wait for (the GUI)
	waitPresentReady();
	presentWaited = false;
	bool swapOnVSync = !settings.input.fastForwardMode && config::VSync;
	HRESULT hr;
	if (!swapchain) {
		hr = DXGI_ERROR_DEVICE_RESET;
	}
	else if (swapOnVSync) {
		// gdxsv: only duplicate frames on high refresh rate monitors when asked to
		float vsyncsPerFrame = config::DupeFrames && settings.display.refreshRate > 60.f ? settings.display.refreshRate / 60.f : 1.f;
		int swapInterval = std::clamp((int)(vsyncsPerFrame * gameSwapInterval), 1, 4);
		// gdxsv: no wait for the vblank, a newer frame replaces one that waits for it (no tearing without
		// DXGI_PRESENT_ALLOW_TEARING)
		hr = swapchain->Present(frameLatencyWaitable != nullptr ? 0 : swapInterval, 0);
		gdxsv_video_dx11_presented(swapchain, hr);
	}
	else {
		hr = swapchain->Present(0, allowTearing ? DXGI_PRESENT_ALLOW_TEARING : DXGI_PRESENT_DO_NOT_WAIT);
		gdxsv_video_dx11_presented(swapchain, hr);
	}
#ifndef TARGET_UWP
	// TEMPDBG: the first presents, each change of result (bounded), and a heartbeat
	{
		static int dbgPresents = 0;
		static int dbgChanges = 0;
		static HRESULT dbgLastHr = S_OK;
		const bool changed = hr != dbgLastHr && dbgChanges < 50;
		if (changed)
			dbgChanges++;
		if (dbgPresents < 5 || changed || dbgPresents % 1800 == 0)
			NOTICE_LOG(RENDERER, "TEMPDBG dx11 present %d vsync=%d hr=%x", dbgPresents, swapOnVSync ? 1 : 0, hr);
		dbgPresents++;
		dbgLastHr = hr;
	}
#endif
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		WARN_LOG(RENDERER, "Present failed: device removed/reset");
		handleDeviceLost();
	}
	else if (hr != DXGI_ERROR_WAS_STILL_DRAWING && FAILED(hr)) {
		WARN_LOG(RENDERER, "Present failed %x", hr);
	}
}

// gdxsv: render thread. Waits until the last frame is rendered, so that the frame taken next is the newest one
void DX11Context::waitPresentReady()
{
	if (frameLatencyWaitable == nullptr || presentWaited)
		return;
	WaitForSingleObjectEx(frameLatencyWaitable, 100, TRUE);
	presentWaited = true;
}

void DX11Context::EndImGuiFrame()
{
	if (pDevice && pDeviceContext && renderTargetView)
	{
		if (overlayOnly) {
			overlay.draw(settings.display.width, settings.display.height, config::FloatVMUs, true);
		}
		else
		{
			pDeviceContext->OMSetRenderTargets(1, &renderTargetView.get(), nullptr);
			const FLOAT black[4] { 0.f, 0.f, 0.f, 1.f };
			pDeviceContext->ClearRenderTargetView(renderTargetView, black);
			if (renderer != nullptr)
				renderer->RenderLastFrame();
		}
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}
	frameRendered = true;
}

void DX11Context::resize()
{
	if (!pDevice)
		return;
	if (swapchain)
	{
		ID3D11RenderTargetView *nullRTV = nullptr;
		pDeviceContext->OMSetRenderTargets(1, &nullRTV, nullptr);
		renderTargetView.reset();
#ifdef TARGET_UWP
		HRESULT hr = swapchain->ResizeBuffers(2, settings.display.width, settings.display.height, DXGI_FORMAT_R8G8B8A8_UNORM, allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
#else
		HRESULT hr = swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
				| (frameLatencyWaitable != nullptr ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0)); // gdxsv
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			handleDeviceLost();
		    return;
		}
#endif
		if (FAILED(hr))
		{
			WARN_LOG(RENDERER, "ResizeBuffers failed: %x", hr);
			return;
		}

		// Create a render target view
		ComPtr<ID3D11Texture2D> backBuffer;
		hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer.get());
		if (FAILED(hr))
		{
			WARN_LOG(RENDERER, "swapChain->GetBuffer() failed: %x", hr);
			return;
		}

		hr = pDevice->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView.get());
		if (FAILED(hr))
		{
			WARN_LOG(RENDERER, "CreateRenderTargetView failed: %x", hr);
			return;
		}
		pDeviceContext->OMSetRenderTargets(1, &renderTargetView.get(), nullptr);

		if (swapchain1)
		{
			DXGI_SWAP_CHAIN_DESC1 desc;
			swapchain1->GetDesc1(&desc);
#ifndef TARGET_UWP
			settings.display.width = desc.Width;
			settings.display.height = desc.Height;
#endif
			NOTICE_LOG(RENDERER, "Swapchain resized: %d x %d", desc.Width, desc.Height);
		}
		else
		{
			DXGI_SWAP_CHAIN_DESC desc;
			swapchain->GetDesc(&desc);
			settings.display.width = desc.BufferDesc.Width;
			settings.display.height = desc.BufferDesc.Height;
			NOTICE_LOG(RENDERER, "Swapchain resized: %d x %d", desc.BufferDesc.Width, desc.BufferDesc.Height);
		}
	}
	// TODO minimized window
}

void DX11Context::handleDeviceLost()
{
	if (pDevice)
	{
		HRESULT hr = pDevice->GetDeviceRemovedReason();
		WARN_LOG(RENDERER, "Device removed reason: %x", hr);
	}
	rend_term_renderer();
	term();
	if (init(true))
	{
		rend_init_renderer();
	}
	else
	{
		Renderer* rend_norend(void);
		renderer = rend_norend();
		renderer->Init();
	}
}

const pD3DCompile DX11Context::getCompiler()
{
	if (d3dcompiler == nullptr)
	{
#ifndef TARGET_UWP
		if (!d3dcompilerLib.load("d3dcompiler_47.dll"))
		{
			if (!d3dcompilerLib.load("d3dcompiler_46.dll"))
			{
				WARN_LOG(RENDERER, "Neither d3dcompiler_47.dll or d3dcompiler_46.dll can be loaded");
				return D3DCompile;
			}
		}
		d3dcompiler = d3dcompilerLib.getFunc("D3DCompile", d3dcompiler);
#endif
		if (d3dcompiler == nullptr)
			d3dcompiler = D3DCompile;
	}
	return d3dcompiler;
}
#endif // !LIBRETRO

bool DX11Context::checkTextureSupport()
{
	const DXGI_FORMAT formats[] = { DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_A8_UNORM };
	const char * const fmtNames[] = { "B5G5R5A1", "B4G4R4A4", "B5G6R5", "B8G8R8A8", "A8" };
	const TextureType dcTexTypes[] = { TextureType::_5551, TextureType::_4444, TextureType::_565, TextureType::_8888, TextureType::_8 };
	UINT support;
	for (std::size_t i = 0; i < std::size(formats); i++)
	{
		supportedTexFormats[(int)dcTexTypes[i]] = false;
		pDevice->CheckFormatSupport(formats[i], &support);
		if ((support & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) != (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
		{
			if (formats[i] == DXGI_FORMAT_B8G8R8A8_UNORM)
			{
				// Can't do much without this format
				ERROR_LOG(RENDERER, "Fatal: Format %s not supported", fmtNames[i]);
				return false;
			}
			WARN_LOG(RENDERER, "Format %s not supported", fmtNames[i]);
		}
		else
		{
			if ((support & D3D11_FORMAT_SUPPORT_MIP) == 0)
				WARN_LOG(RENDERER, "Format %s doesn't support mipmaps", fmtNames[i]);
			else if ((support & (D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_RENDER_TARGET)) != (D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_RENDER_TARGET))
				WARN_LOG(RENDERER, "Format %s doesn't support mipmap autogen", fmtNames[i]);
			else
				supportedTexFormats[(int)dcTexTypes[i]] = true;
		}
	}

	return true;
}
