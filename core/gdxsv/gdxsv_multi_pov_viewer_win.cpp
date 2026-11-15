// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_viewer.h"

#if defined(VIDEO_ROUTING) && !defined(LIBRETRO)
#include <algorithm>
#include <array>
#include <memory>

#include "SpoutDX.h"
// Keep Spout's GL declarations separate from GLAD's incompatible 32-bit types.
#include "SpoutReceiver.h"
#include "cfg/option.h"
#include "gdxsv_multi_pov.h"
#include "imgui_impl_dx11.h"
#include "log/Log.h"  // TEMPDBG
#include "rend/dx11/dx11context.h"
#include "sdl/sdl.h"
#ifdef USE_VULKAN
#include "rend/vulkan/vulkan_context.h"
#endif

// TEMPDBG: in opengl_driver.cpp
void gdxsv_tempdbg_gl(const char* where);
void gdxsv_tempdbg_glcache_reset();
extern int gdxsv_tempdbg_gui_frames;

namespace {
struct GdxsvSpoutReceiver {
	std::unique_ptr<SpoutReceiver> gl;
	std::unique_ptr<spoutDX> dx;
	GLuint texture = 0;
	ComPtr<ID3D11Texture2D> dx_texture;
	ComPtr<ID3D11ShaderResourceView> view;
	ImTextureDX11 dx_image{};
	GdxsvMultiPovVideoFrame frame;
	bool tempdbg_connected = false;  // TEMPDBG
};
struct GdxsvSpoutViewer {
	std::array<GdxsvSpoutReceiver, kGdxsvMultiPovScreens> receivers;
};
GdxsvSpoutViewer viewer;

enum class GdxsvViewerBackend { Unsupported, OpenGL, DirectX11, Vulkan };

GdxsvViewerBackend CurrentBackend() {
	// Settings can change before the context owning this frame is replaced.
	if (dynamic_cast<DX11Context*>(GraphicsContext::Instance())) return GdxsvViewerBackend::DirectX11;
#ifdef USE_VULKAN
	if (dynamic_cast<VulkanContext*>(GraphicsContext::Instance())) return GdxsvViewerBackend::Vulkan;
#endif
	// The GL context is current only on the thread drawing: the render thread while a game runs.
	SDL_Window* window = sdl_get_window();
	return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL) ? GdxsvViewerBackend::OpenGL : GdxsvViewerBackend::Unsupported;
}

GdxsvMultiPovVideoFrame ReceiveGL(GdxsvSpoutReceiver& receiver, const std::string& name) {
	if (!receiver.gl) {
		// TEMPDBG: Spout's own log, %APPDATA%\Spout\flycast-host-spout.log
		static const bool spout_log = (spoututils::EnableSpoutLogFile("flycast-host-spout.log"),
									   spoututils::SetSpoutLogLevel(spoututils::SPOUT_LOG_VERBOSE), true);
		(void)spout_log;
		receiver.gl = std::make_unique<SpoutReceiver>();
		receiver.gl->SetReceiverName(name.c_str());
	}
	GLint fbo, texture;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fbo);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	const bool connected = receiver.gl->ReceiveTexture(receiver.texture, GL_TEXTURE_2D, false, fbo);
	// TEMPDBG
	if (connected != receiver.tempdbg_connected) {
		NOTICE_LOG(RENDERER, "TEMPDBG receive %s: connected %d -> %d, fbo %d, texture %d, own texture %u", name.c_str(), receiver.tempdbg_connected,
				   connected, fbo, texture, receiver.texture);
		receiver.tempdbg_connected = connected;
		gdxsv_tempdbg_gl("after receive change");
	}
	const bool updated = receiver.gl->IsUpdated();
	if (connected && updated) {
		const auto width = receiver.gl->GetSenderWidth();
		const auto height = receiver.gl->GetSenderHeight();
		if (width == 0 || height == 0 || width > 16384 || height > 16384) {
			glBindTexture(GL_TEXTURE_2D, texture);
			return {};
		}
		if (!receiver.texture) glGenTextures(1, &receiver.texture);
		glBindTexture(GL_TEXTURE_2D, receiver.texture);
		// The finished picture already includes transparency; don't blend its alpha again.
		// TODO: Restore RGBA once the upstream opaque video-routing output fix is merged here.
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		receiver.frame = {static_cast<ImTextureID>(receiver.texture), static_cast<int>(width), static_cast<int>(height), false};
	}
	glBindTexture(GL_TEXTURE_2D, texture);
	return connected && !updated ? receiver.frame : GdxsvMultiPovVideoFrame{};
}

// TODO: Remove this callback once the upstream opaque video-routing output fix is merged here.
void DrawOpaqueDX(const ImDrawList*, const ImDrawCmd*) {
	auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
	state->DeviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

GdxsvMultiPovVideoFrame ReceiveDX(DX11Context* context, GdxsvSpoutReceiver& receiver, const std::string& name) {
	if (!receiver.dx) {
		receiver.dx = std::make_unique<spoutDX>();
		if (!receiver.dx->OpenDirectX11(context->getDevice())) {
			receiver.dx.reset();
			return {};
		}
		receiver.dx->SetReceiverName(name.c_str());
	}
	if (!receiver.dx->ReceiveTexture()) return {};
	receiver.dx->IsUpdated();
	ID3D11Texture2D* texture = receiver.dx->GetSenderTexture();
	if (texture == nullptr) return {};
	if (texture != receiver.dx_texture.get()) {
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);
		if (desc.Width == 0 || desc.Height == 0 || desc.Width > 16384 || desc.Height > 16384) return {};
		receiver.view.reset();
		receiver.dx_texture.reset();
		if (FAILED(context->getDevice()->CreateShaderResourceView(texture, nullptr, &receiver.view.get()))) return {};
		texture->AddRef();
		receiver.dx_texture.reset(texture);
		receiver.dx_image = {texture, receiver.view.get(), false};
		receiver.frame = {reinterpret_cast<ImTextureID>(&receiver.dx_image), static_cast<int>(desc.Width), static_cast<int>(desc.Height),
						  false, DrawOpaqueDX};
	}
	return receiver.frame;
}
}  // namespace

const char* gdxsv_multi_pov_viewer_win_unavailable() {
	switch (CurrentBackend()) {
		case GdxsvViewerBackend::DirectX11:
		case GdxsvViewerBackend::OpenGL:
			return nullptr;
#ifdef USE_VULKAN
		case GdxsvViewerBackend::Vulkan:
			return gdxsv_multi_pov_viewer_vulkan_unavailable();
#endif
		default:
			return kGdxsvMultiPovRendererUnavailable;
	}
}

bool gdxsv_multi_pov_viewer_win_needs_probe() { return CurrentBackend() == GdxsvViewerBackend::Vulkan; }

const char* gdxsv_multi_pov_viewer_win_prepare(bool starting) {
#ifdef USE_VULKAN
	if (CurrentBackend() == GdxsvViewerBackend::Vulkan) return gdxsv_multi_pov_viewer_vulkan_prepare(starting);
#endif
	return gdxsv_multi_pov_viewer_win_unavailable();
}

GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_receive(int screen, const std::string& name) {
	if (screen < 1 || screen >= kGdxsvMultiPovScreens) return {};
	auto& receiver = viewer.receivers[screen];
	switch (CurrentBackend()) {
		case GdxsvViewerBackend::DirectX11:
			return ReceiveDX(static_cast<DX11Context*>(GraphicsContext::Instance()), receiver, name);
#ifdef USE_VULKAN
		case GdxsvViewerBackend::Vulkan:
			return gdxsv_multi_pov_viewer_vulkan_receive(screen, name);
#endif
		case GdxsvViewerBackend::OpenGL:
			return ReceiveGL(receiver, name);
		default:
			return {};
	}
}

void gdxsv_multi_pov_viewer_term(bool reset_device) {
#ifdef USE_VULKAN
	gdxsv_multi_pov_viewer_vulkan_term(reset_device);
#endif
	// TEMPDBG
	const bool tempdbg = CurrentBackend() == GdxsvViewerBackend::OpenGL && std::any_of(viewer.receivers.begin(), viewer.receivers.end(), [](const auto& r) { return r.gl != nullptr; });
	if (tempdbg) gdxsv_tempdbg_gl("term before");
	for (auto& receiver : viewer.receivers) {
		if (receiver.gl) receiver.gl->ReleaseReceiver();
		if (receiver.texture) glDeleteTextures(1, &receiver.texture);
		if (receiver.dx) {
			receiver.dx->ReleaseReceiver();
			receiver.dx->CloseDirectX11();
		}
		receiver = {};
	}
	if (tempdbg) {
		gdxsv_tempdbg_gl("term after");
		gdxsv_tempdbg_glcache_reset();
		gdxsv_tempdbg_gui_frames = 5;
	}
}
#endif
