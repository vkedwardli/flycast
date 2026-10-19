// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_viewer.h"

#if defined(VIDEO_ROUTING) && defined(USE_VULKAN) && !defined(LIBRETRO)
#include <array>

#include "gdxsv_multi_pov.h"
#include "imgui_impl_vulkan.h"
#include "rend/vulkan/vulkan_context.h"

namespace {
struct GdxsvViewerMetalImage : Deletable {
	std::shared_ptr<void> metal;
	vk::Device device;
	vk::DescriptorPool pool;
	vk::UniqueImage image;
	vk::UniqueImageView view;
	vk::UniqueSampler sampler;
	vk::DescriptorSet descriptor;
	GdxsvMultiPovVideoFrame frame;
	~GdxsvViewerMetalImage() override {
		if (descriptor) device.freeDescriptorSets(pool, descriptor);
	}
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
	if (!context->IsRendering() || (previous && previous->metal == texture)) return previous ? previous->frame : GdxsvMultiPovVideoFrame{};
	auto frame = std::make_unique<GdxsvViewerMetalImage>();
	frame->metal = texture;
	frame->device = context->GetDevice();
	frame->pool = context->GetDescriptorPool();
	vk::ImportMetalTextureInfoEXT imported(vk::ImageAspectFlagBits::eColor, texture.get());
	vk::ImageCreateInfo info({}, vk::ImageType::e2D, vk::Format::eB8G8R8A8Unorm,
							 {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1}, 1, 1, vk::SampleCountFlagBits::e1,
							 vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled);
	info.pNext = &imported;
	frame->image = frame->device.createImageUnique(info);
	frame->view = frame->device.createImageViewUnique(
		vk::ImageViewCreateInfo({}, *frame->image, vk::ImageViewType::e2D, info.format, {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}));
	frame->sampler = frame->device.createSamplerUnique(vk::SamplerCreateInfo(
		{}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge));
	const vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eUndefined,
										 vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
										 *frame->image, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
	context->GetCurrentCommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eFragmentShader,
													   {}, nullptr, nullptr, barrier);
	frame->descriptor = ImGui_ImplVulkan_AddTexture(*frame->sampler, *frame->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (previous) context->addToFlight(previous.release());
	frame->frame = {reinterpret_cast<ImTextureID>(static_cast<VkDescriptorSet>(frame->descriptor)), width, height, true};
	previous = std::move(frame);
	return previous->frame;
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
