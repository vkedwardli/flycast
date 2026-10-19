// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_viewer.h"

#import <AppKit/AppKit.h>
#include "log/LogManager.h"

#if defined(VIDEO_ROUTING) && !defined(LIBRETRO)
#include <array>
#include <chrono>
#include <memory>

#import <Syphon/Syphon.h>
#include "cfg/option.h"
#include "gdxsv_multi_pov.h"
#include "wsi/gl_context.h"

namespace {
struct GdxsvSyphonReceiver {
	SyphonClientBase* client = nil;
	std::chrono::steady_clock::time_point retry;
	GLuint texture = 0;
	GLuint read_fbo = 0;
	GLuint draw_fbo = 0;
	GdxsvMultiPovVideoFrame frame;
};

struct GdxsvSyphonViewer {
	std::array<GdxsvSyphonReceiver, kGdxsvMultiPovScreens> receivers;
};
GdxsvSyphonViewer viewer;

void Release(int screen) {
	auto& receiver = viewer.receivers[screen];
	[receiver.client stop];
	[receiver.client release];
	receiver.client = nil;
	if (receiver.texture) glDeleteTextures(1, &receiver.texture);
	if (receiver.read_fbo) glDeleteFramebuffers(1, &receiver.read_fbo);
	if (receiver.draw_fbo) glDeleteFramebuffers(1, &receiver.draw_fbo);
#ifdef USE_VULKAN
	gdxsv_multi_pov_viewer_metal_release(screen);
#endif
	receiver = {};
}

void Connect(GdxsvSyphonReceiver& receiver, const std::string& name) {
	const auto now = std::chrono::steady_clock::now();
	if (now < receiver.retry) return;
	receiver.retry = now + std::chrono::milliseconds(250);
	NSArray* servers = [[SyphonServerDirectory sharedDirectory] serversMatchingName:[NSString stringWithUTF8String:name.c_str()]
																			appName:nil];
	// Never pick an arbitrary process when a streaming setup reused a name.
	if (servers.count != 1) return;
	NSDictionary* description = [servers objectAtIndex:0];
	if (isOpenGL(config::RendererType)) {
		receiver.client = [[SyphonOpenGLClient alloc] initWithServerDescription:description
																		context:CGLGetCurrentContext()
																		options:nil
																newFrameHandler:nil];
	}
#ifdef USE_VULKAN
	else if (isVulkan(config::RendererType)) {
		receiver.client = [[SyphonMetalClient alloc] initWithServerDescription:description
																		device:(id<MTLDevice>)gdxsv_multi_pov_viewer_metal_device()
																	   options:nil
															   newFrameHandler:nil];
	}
#endif
}

void ReceiveGL(GdxsvSyphonReceiver& receiver) {
	SyphonOpenGLImage* image = [(SyphonOpenGLClient*)receiver.client newFrameImage];
	if (image == nil) return;
	const int width = static_cast<int>(image.textureSize.width);
	const int height = static_cast<int>(image.textureSize.height);
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
		[image release];
		return;
	}
	GLint readFbo, drawFbo, texture;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	if (!receiver.texture) glGenTextures(1, &receiver.texture);
	if (!receiver.read_fbo) glGenFramebuffers(1, &receiver.read_fbo);
	if (!receiver.draw_fbo) glGenFramebuffers(1, &receiver.draw_fbo);
	glBindTexture(GL_TEXTURE_2D, receiver.texture);
	if (receiver.frame.width != width || receiver.frame.height != height) {
		// The finished picture already includes transparency; don't blend its alpha again.
		// TODO: Restore RGBA once the upstream opaque video-routing output fix is merged here.
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	// Syphon supplies rectangle textures; ImGui samples ordinary 2D textures.
	// Keep the conversion on the GPU, with no readback to system memory.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, receiver.read_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, image.textureName, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, receiver.draw_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, receiver.texture, 0);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
	glBindTexture(GL_TEXTURE_2D, texture);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	receiver.frame = {static_cast<ImTextureID>(receiver.texture), width, height, true};
	[image release];
}

#ifdef USE_VULKAN
void ReceiveMetal(int screen, GdxsvSyphonReceiver& receiver) {
	id<MTLTexture> texture = [(SyphonMetalClient*)receiver.client newFrameImage];
	if (texture == nil) return;
	if (texture.width == 0 || texture.height == 0 || texture.width > 16384 || texture.height > 16384 ||
		texture.pixelFormat != MTLPixelFormatBGRA8Unorm) {
		[texture release];
		return;
	}
	// Keep the Syphon texture alive until Vulkan has finished sampling it.
	std::shared_ptr<void> retained(texture, [](void* image) { [(id)image release]; });
	receiver.frame =
		gdxsv_multi_pov_viewer_metal_receive(screen, retained, static_cast<int>(texture.width), static_cast<int>(texture.height));
}
#endif
}  // namespace

GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_receive(int screen, const std::string& name) {
	if (screen < 1 || screen >= kGdxsvMultiPovScreens) return {};
	@autoreleasepool {
		auto& receiver = viewer.receivers[screen];
		if (receiver.client && !receiver.client.isValid) Release(screen);
		if (receiver.client == nil) Connect(receiver, name);
		if (receiver.client == nil || !receiver.client.isValid) return {};
		if (receiver.client.hasNewFrame || !receiver.frame.texture) {
			if (isOpenGL(config::RendererType)) ReceiveGL(receiver);
#ifdef USE_VULKAN
			else if (isVulkan(config::RendererType))
				ReceiveMetal(screen, receiver);
#endif
		}
		return receiver.frame;
	}
}

void gdxsv_multi_pov_viewer_term(bool) {
#ifdef USE_VULKAN
	gdxsv_multi_pov_viewer_metal_term();
#endif
	for (int screen = 1; screen < kGdxsvMultiPovScreens; ++screen) Release(screen);
}
#endif
