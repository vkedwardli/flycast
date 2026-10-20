// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_viewer.h"

#if defined(VIDEO_ROUTING) && defined(USE_VULKAN) && !defined(LIBRETRO)
#include <array>

#include "gdxsv_multi_pov.h"
#include "gdxsv_multi_pov_viewer_vulkan.h"

namespace {
struct GdxsvViewerMetalImage : Deletable {
	std::shared_ptr<void> metal;
	vk::UniqueImage image;
	GdxsvViewerVulkanTexture texture;
};
struct GdxsvVulkanViewer {
	std::array<std::unique_ptr<GdxsvViewerMetalImage>, kGdxsvMultiPovScreens> images;
};
GdxsvVulkanViewer viewer;
}  // namespace

void* gdxsv_multi_pov_viewer_metal_device() {
	vk::ExportMetalDeviceInfoEXT device;
	vk::ExportMetalObjectsInfoEXT objects(&device);
	VulkanContext::Instance()->GetDevice().exportMetalObjectsEXT(&objects);
	return device.mtlDevice;
}

GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_metal_receive(int screen, const std::shared_ptr<void>& texture, int width, int height) {
	auto& previous = viewer.images[screen];
	VulkanContext* context = VulkanContext::Instance();
	// The OSD is built while a command buffer is recording. Settings screens
	// reuse the last image until rendering resumes.
	if (!context->IsRendering() || (previous && previous->metal == texture))
		return previous ? previous->texture.frame : GdxsvMultiPovVideoFrame{};
	auto frame = std::make_unique<GdxsvViewerMetalImage>();
	frame->metal = texture;
	vk::ImportMetalTextureInfoEXT imported(vk::ImageAspectFlagBits::eColor, texture.get());
	vk::ImageCreateInfo info({}, vk::ImageType::e2D, vk::Format::eB8G8R8A8Unorm,
							 {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1}, 1, 1, vk::SampleCountFlagBits::e1,
							 vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled);
	info.pNext = &imported;
	frame->image = context->GetDevice().createImageUnique(info);
	const vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eUndefined,
										 vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
										 *frame->image, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
	context->GetCurrentCommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eFragmentShader,
													   {}, nullptr, nullptr, barrier);
	frame->texture.Init(context->GetDevice(), *frame->image, info.format, width, height, {}, true);
	if (previous) context->addToFlight(previous.release());
	previous = std::move(frame);
	return previous->texture.frame;
}

void gdxsv_multi_pov_viewer_metal_release(int screen) {
	if (viewer.images[screen]) VulkanContext::Instance()->addToFlight(viewer.images[screen].release());
}

void gdxsv_multi_pov_viewer_metal_term() {
	for (const auto& image : viewer.images) {
		if (image) {
			VulkanContext::Instance()->WaitIdle();
			break;
		}
	}
	for (auto& image : viewer.images) image.reset();
}
#endif
