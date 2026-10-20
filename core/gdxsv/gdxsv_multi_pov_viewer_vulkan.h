// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gdxsv_multi_pov_viewer.h"
#include "imgui_impl_vulkan.h"
#include "rend/vulkan/vulkan_context.h"

// Declare after the image and its backing allocation, so the descriptor and
// view are destroyed first. The owner stays alive until its GPU work completes.
struct GdxsvViewerVulkanTexture {
	vk::UniqueImageView view;
	vk::UniqueSampler sampler;
	vk::DescriptorSet descriptor;
	GdxsvMultiPovVideoFrame frame;

	~GdxsvViewerVulkanTexture() {
		if (descriptor) ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(descriptor));
	}

	void Init(vk::Device device, vk::Image image, vk::Format format, int width, int height, vk::ComponentMapping components = {},
			  bool flipped = false) {
		view = device.createImageViewUnique(
			vk::ImageViewCreateInfo({}, image, vk::ImageViewType::e2D, format, components, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}));
		sampler = device.createSamplerUnique(vk::SamplerCreateInfo(
			{}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge));
		descriptor = vk::DescriptorSet(ImGui_ImplVulkan_AddTexture(static_cast<VkSampler>(*sampler), static_cast<VkImageView>(*view),
																   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		frame = {(ImTextureID)(VkDescriptorSet)descriptor, width, height, flipped};
	}
};
