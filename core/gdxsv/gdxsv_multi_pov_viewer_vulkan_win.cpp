// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_multi_pov_viewer.h"
#include "gdxsv_video_hooks.h"

#if defined(VIDEO_ROUTING) && defined(USE_VULKAN) && !defined(LIBRETRO)
#include <dxgi1_6.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

#include "SpoutDX.h"
#include "gdxsv_multi_pov.h"
#include "gdxsv_multi_pov_viewer_vulkan.h"
#include "windows/comptr.h"

namespace {
constexpr auto kSharedHandle = vk::ExternalMemoryHandleTypeFlagBits::eD3D11TextureKmt;
constexpr vk::ImageSubresourceRange kColor(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
constexpr const char* kImportUnavailable = "Four-screen Vulkan needs DirectX texture sharing; select OpenGL or DirectX 11";
constexpr const char* kDeviceReset = "DirectX video device was reset; retrying video sharing";

void Check(HRESULT result, const char* operation) {
	if (FAILED(result)) throw std::runtime_error(operation);
}

uint32_t MemoryType(const vk::PhysicalDeviceMemoryProperties& properties, uint32_t bits) {
	// Imported allocations need only a compatible type. Prefer device-local
	// memory, but never use the renderer's fatal findMemoryType() here.
	for (bool local : {true, false})
		for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
			if ((bits & (1u << i)) && (!local || (properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)))
				return i;
	throw std::runtime_error("No compatible DirectX texture memory");
}

// ReceiveTexture's return value reports connection, not a successful copy;
// IsFrameNew can retain its old value after a failed texture lock.
struct GdxsvSpoutSource : spoutDX {
	~GdxsvSpoutSource() {
		// The base destructor treats an initialized, disconnected object as a
		// sender. Clear receiver state first, including after a failed connect.
		ReleaseReceiver();
	}

	bool Connect() {
		if (!ReceiveSenderData()) {
			ReleaseReceiver();
			return false;
		}
		if (!m_pSharedTexture) {
			if (FAILED(m_pd3dDevice->GetDeviceRemovedReason())) throw std::runtime_error(kDeviceReset);
			throw std::runtime_error("Cannot open the guest texture on the Vulkan GPU; check that both use the same GPU");
		}
		D3D11_TEXTURE2D_DESC desc{};
		m_pSharedTexture->GetDesc(&desc);
		if (desc.Width != m_Width || desc.Height != m_Height || desc.Format != m_dwFormat)
			throw std::runtime_error("Spout texture does not match its published dimensions or format");
		return true;
	}

	bool CopyTo(ID3D11Texture2D* texture, ID3D11Query* copied) {
		const bool access = frame.CheckTextureAccess(m_pSharedTexture);
		// Copy even an unchanged picture, without depending on Spout's
		// per-user frame-count setting or changing it for other applications.
		if (access) {
			m_pImmediateContext->CopyResource(texture, m_pSharedTexture);
			m_pImmediateContext->End(copied);
			m_pImmediateContext->Flush();
		}
		// An abandoned mutex transfers ownership even though Spout reports failure.
		frame.AllowTextureAccess(m_pSharedTexture);
		return access;
	}
};

// One D3D copy into a host-owned texture, sampled directly by Vulkan. The
// sender's lock is released immediately; it never waits for host presentation.
struct GdxsvSpoutVulkanImage {
	ComPtr<ID3D11Texture2D> shared_texture;
	ComPtr<ID3D11Query> copied;
	vk::UniqueDeviceMemory memory;
	vk::UniqueImage image;
	GdxsvViewerVulkanTexture texture;
	bool pending = false;
	uint64_t sequence = 0;
};

struct GdxsvRetainedVideoImage : Deletable {
	std::shared_ptr<GdxsvSpoutVulkanImage> image;
	explicit GdxsvRetainedVideoImage(std::shared_ptr<GdxsvSpoutVulkanImage> image) : image(std::move(image)) {}
};

struct GdxsvSpoutVulkanReceiver {
	std::unique_ptr<GdxsvSpoutSource> spout;
	std::vector<std::shared_ptr<GdxsvSpoutVulkanImage>> images;
	std::shared_ptr<GdxsvSpoutVulkanImage> displayed;
	uint64_t sequence = 0;
	int draw_frame = -1;
	std::chrono::steady_clock::time_point retry;
	std::chrono::steady_clock::time_point next_pool_warning;
	std::string error;

	GdxsvMultiPovVideoFrame Frame() const {
		auto frame = displayed ? displayed->texture.frame : GdxsvMultiPovVideoFrame{};
		if (!error.empty()) frame.error = error.c_str();
		return frame;
	}
};

struct GdxsvSpoutVulkanViewer {
	std::atomic<bool> extension_available{false};
	// Graphics state is confined to the UI thread, including probing and teardown.
	vk::Device probed_device;
	std::string error = kImportUnavailable;
	unsigned retries = 0;
	std::chrono::steady_clock::time_point retry;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	std::array<GdxsvSpoutVulkanReceiver, kGdxsvMultiPovScreens> receivers;
};
GdxsvSpoutVulkanViewer viewer;

void InitDevice(VulkanContext* context) {
	const auto properties = context->GetPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
	const auto& id = properties.get<vk::PhysicalDeviceIDProperties>();
	if (!id.deviceLUIDValid) throw std::runtime_error("Vulkan GPU has no DirectX adapter identifier");

	// Match DX11Context's selection policy: high-performance adapter, then
	// the system default. Reject a different Vulkan GPU before spawning guests.
	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIFactory6> modern;
	ComPtr<IDXGIAdapter> adapter;
	if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory.get())))) {
		factory.as(modern);
		if (modern)
			modern->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter),
											   reinterpret_cast<void**>(&adapter.get()));
	}
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
	Check(
		D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
						  levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &viewer.device.get(), nullptr, &viewer.context.get()),
		"Cannot create the guests' DirectX device");
	ComPtr<IDXGIDevice> dxgi;
	Check(viewer.device.as(dxgi), "Cannot identify the DirectX device");
	adapter.reset();
	Check(dxgi->GetAdapter(&adapter.get()), "Cannot identify the DirectX adapter");
	DXGI_ADAPTER_DESC desc{};
	Check(adapter->GetDesc(&desc), "Cannot read the DirectX adapter");
	if (std::memcmp(&desc.AdapterLuid, id.deviceLUID.data(), sizeof(desc.AdapterLuid)) != 0) {
		throw std::runtime_error("Vulkan and DirectX 11 select different GPUs; select OpenGL or DirectX 11 for four-screen playback");
	}
}

std::shared_ptr<GdxsvSpoutVulkanImage> CreateImage(VulkanContext* context, spoutDirectX& shared, uint32_t width, uint32_t height,
												   DXGI_FORMAT dx_format, bool make_view = true) {
	const auto format = dx_format == DXGI_FORMAT_B8G8R8A8_UNORM
							? vk::Format::eB8G8R8A8Unorm
							: (dx_format == DXGI_FORMAT_R8G8B8A8_UNORM ? vk::Format::eR8G8B8A8Unorm : vk::Format::eUndefined);
	if (width == 0 || height == 0 || width > 16384 || height > 16384 || format == vk::Format::eUndefined)
		throw std::runtime_error("Unsupported Spout texture size or format");
	const auto physical = context->GetPhysicalDevice();
	const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment;
	vk::PhysicalDeviceExternalImageFormatInfo external(kSharedHandle);
	vk::PhysicalDeviceImageFormatInfo2 query(format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, usage);
	query.pNext = &external;
	const auto properties = physical.getImageFormatProperties2<vk::ImageFormatProperties2, vk::ExternalImageFormatProperties>(query);
	if (!(properties.get<vk::ExternalImageFormatProperties>().externalMemoryProperties.externalMemoryFeatures &
		  vk::ExternalMemoryFeatureFlagBits::eImportable))
		throw std::runtime_error("Vulkan cannot import DirectX textures on this GPU");

	auto result = std::make_shared<GdxsvSpoutVulkanImage>();
	const auto device = context->GetDevice();
	HANDLE handle = nullptr;
	if (!shared.CreateSharedDX11Texture(viewer.device, width, height, dx_format, &result->shared_texture.get(), handle))
		throw std::runtime_error("Cannot create the shared DirectX receiving texture");
	const D3D11_QUERY_DESC copied{D3D11_QUERY_EVENT, 0};
	Check(viewer.device->CreateQuery(&copied, &result->copied.get()), "Cannot create the DirectX copy query");

	vk::ExternalMemoryImageCreateInfo shared_info(kSharedHandle);
	vk::ImageCreateInfo info({}, vk::ImageType::e2D, format, {width, height, 1}, 1, 1, vk::SampleCountFlagBits::e1,
							 vk::ImageTiling::eOptimal, usage);
	info.pNext = &shared_info;
	result->image = device.createImageUnique(info);
	const auto requirements = device.getImageMemoryRequirements(*result->image);
	const auto bits = requirements.memoryTypeBits & device.getMemoryWin32HandlePropertiesKHR(kSharedHandle, handle).memoryTypeBits;
	const auto memory_type = MemoryType(physical.getMemoryProperties(), bits);
	vk::MemoryDedicatedAllocateInfo dedicated(*result->image);
	vk::ImportMemoryWin32HandleInfoKHR imported(kSharedHandle, handle, nullptr, &dedicated);
	result->memory = device.allocateMemoryUnique(vk::MemoryAllocateInfo(requirements.size, memory_type, &imported));
	device.bindImageMemory(*result->image, *result->memory, 0);
	if (make_view) {
		vk::ComponentMapping components;
		// TODO: Restore alpha once the upstream opaque video-routing output fix is merged here.
		components.a = vk::ComponentSwizzle::eOne;
		result->texture.Init(device, *result->image, format, width, height, components);
	}
	// KMT handles have no CloseHandle ownership. The D3D texture outlives
	// its Vulkan view, image and imported memory, including on failed imports.
	return result;
}

void ReleaseDisplayed(VulkanContext* context, GdxsvSpoutVulkanReceiver& receiver) {
	if (!receiver.displayed) return;
	const vk::ImageMemoryBarrier release(vk::AccessFlagBits::eShaderRead, {}, vk::ImageLayout::eShaderReadOnlyOptimal,
										 vk::ImageLayout::eGeneral, context->GetGraphicsQueueFamilyIndex(), VK_QUEUE_FAMILY_EXTERNAL,
										 *receiver.displayed->image, kColor);
	context->GetCurrentCommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eBottomOfPipe,
													   {}, nullptr, nullptr, release);
	// The swapchain's existing fence retires this reference. Only then may
	// D3D write the texture again; no separate submit, fence or GPU copy.
	context->addToFlight(new GdxsvRetainedVideoImage(std::move(receiver.displayed)));
}

GdxsvMultiPovVideoFrame Receive(VulkanContext* context, GdxsvSpoutVulkanReceiver& receiver, const std::string& name) {
	if (!receiver.spout) {
		receiver.spout = std::make_unique<GdxsvSpoutSource>();
		if (!receiver.spout->OpenDirectX11(viewer.device)) throw std::runtime_error("Cannot open the Spout receiver");
		receiver.spout->SetReceiverName(name.c_str());
	}
	if (!receiver.spout->Connect()) {
		receiver.error = kGdxsvMultiPovWaitingForVideo;
		return receiver.Frame();
	}
	if (receiver.spout->IsUpdated() && !receiver.images.empty()) {
		D3D11_TEXTURE2D_DESC desc{};
		receiver.images.front()->shared_texture->GetDesc(&desc);
		if (desc.Width != receiver.spout->GetSenderWidth() || desc.Height != receiver.spout->GetSenderHeight() ||
			desc.Format != receiver.spout->GetSenderFormat())
			receiver.images.clear();
		// Reconnecting does not invalidate our last completed copy. Keep it
		// displayed until a replacement, possibly at a new size, is ready.
	}

	std::shared_ptr<GdxsvSpoutVulkanImage> newest = receiver.displayed;
	for (const auto& image : receiver.images) {
		if (image->pending) {
			const HRESULT done = viewer.context->GetData(image->copied, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
			Check(done, "DirectX texture copy failed");
			if (done == S_OK) image->pending = false;
		}
		if (!image->pending && image->sequence != 0 && (!newest || image->sequence > newest->sequence)) newest = image;
	}
	if (newest != receiver.displayed) {
		ReleaseDisplayed(context, receiver);
		// The external-image implied-layout rule puts D3D11 KMT textures in
		// GENERAL after D3D access, even on their first Vulkan acquire.
		const vk::ImageMemoryBarrier acquire({}, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eGeneral,
											 vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_EXTERNAL,
											 context->GetGraphicsQueueFamilyIndex(), *newest->image, kColor);
		context->GetCurrentCommandBuffer().pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
														   vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, acquire);
		receiver.displayed = std::move(newest);
	}

	std::shared_ptr<GdxsvSpoutVulkanImage> target;
	for (const auto& image : receiver.images)
		if (image.use_count() == 1 && !image->pending) {
			target = image;
			break;
		}
	// Accommodate every in-flight presentation, the displayed texture, the
	// next D3D copy and a spare for uneven swapchain-image reuse.
	if (!target && receiver.images.size() < context->GetSwapChainSize() + 3) {
		target = CreateImage(context, receiver.spout->spoutdx, receiver.spout->GetSenderWidth(), receiver.spout->GetSenderHeight(),
							 receiver.spout->GetSenderFormat());
		receiver.images.push_back(target);
	}
	if (!target) {
		const auto now = std::chrono::steady_clock::now();
		if (now >= receiver.next_pool_warning) {
			WARN_LOG(RENDERER, "multi-pov Vulkan %s: video copy skipped; all %zu textures are busy", name.c_str(), receiver.images.size());
			receiver.next_pool_warning = now + std::chrono::seconds(5);
		}
	}
	const bool copied = target && receiver.spout->CopyTo(target->shared_texture, target->copied);
	if (copied) {
		target->pending = true;
		target->sequence = ++receiver.sequence;
	}
	receiver.error.clear();
	return receiver.Frame();
}
}  // namespace

void gdxsv_video_vulkan_extensions(uint32_t device_version, const std::set<std::string>& supported, std::vector<const char*>& enabled) {
	const bool available = device_version >= VK_API_VERSION_1_1 && VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion != nullptr &&
						   vk::enumerateInstanceVersion() >= VK_API_VERSION_1_1 &&
						   supported.count(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) != 0;
	viewer.extension_available.store(available, std::memory_order_release);
	if (available) enabled.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
}

const char* gdxsv_multi_pov_viewer_vulkan_unavailable() {
	return viewer.extension_available.load(std::memory_order_acquire) ? nullptr : kImportUnavailable;
}

const char* gdxsv_multi_pov_viewer_vulkan_prepare(bool starting) {
	auto* context = dynamic_cast<VulkanContext*>(GraphicsContext::Instance());
	if (!context || !viewer.extension_available.load(std::memory_order_acquire)) return kImportUnavailable;
	// An explicit new attempt retries an earlier failed probe.
	if (starting && !viewer.device) {
		viewer.probed_device = nullptr;
		viewer.retries = 0;
	}
	if (viewer.device && FAILED(viewer.device->GetDeviceRemovedReason())) {
		WARN_LOG(RENDERER, "multi-pov Vulkan: DirectX device removed; rebuilding video sharing");
		gdxsv_multi_pov_viewer_vulkan_term(true);
		viewer.error = kDeviceReset;
		viewer.retries = 3;
		viewer.retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	}
	if (viewer.retries && std::chrono::steady_clock::now() < viewer.retry) return starting ? viewer.error.c_str() : nullptr;
	if (viewer.probed_device == context->GetDevice()) return viewer.error.empty() ? nullptr : viewer.error.c_str();
	viewer.probed_device = context->GetDevice();
	try {
		InitDevice(context);
		spoutDirectX shared;
		// Test allocation, handle compatibility and binding, not just the
		// extension name. No ImGui resources or GPU submissions are needed.
		for (auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM})
			CreateImage(context, shared, 16, 16, format, /*make_view=*/false);
		viewer.error.clear();
		viewer.retries = 0;
	} catch (const std::exception& error) {
		// Launch failures are reported by the caller; log each recovery attempt here.
		if (!starting) WARN_LOG(RENDERER, "multi-pov Vulkan unavailable: %s", error.what());
		viewer.context.reset();
		viewer.device.reset();
		viewer.error = error.what();
		if (viewer.retries) {
			if (--viewer.retries) {
				viewer.probed_device = nullptr;
				viewer.retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
				viewer.error += "; retrying video sharing";
			} else {
				viewer.error += "; restart four-screen playback to retry";
			}
		}
	}
	return viewer.error.empty() || (!starting && viewer.retries) ? nullptr : viewer.error.c_str();
}

GdxsvMultiPovVideoFrame gdxsv_multi_pov_viewer_vulkan_receive(int screen, const std::string& name) {
	auto& receiver = viewer.receivers[screen];
	if (!viewer.error.empty()) {
		auto frame = receiver.Frame();
		frame.error = viewer.error.c_str();
		return frame;
	}
	auto* context = dynamic_cast<VulkanContext*>(GraphicsContext::Instance());
	if (!context || !context->IsRendering() || receiver.draw_frame == ImGui::GetFrameCount()) return receiver.Frame();
	receiver.draw_frame = ImGui::GetFrameCount();
	if (std::chrono::steady_clock::now() < receiver.retry) return receiver.Frame();
	try {
		return Receive(context, receiver, name);
	} catch (const std::exception& error) {
		if (receiver.error != error.what()) WARN_LOG(RENDERER, "multi-pov Vulkan receiver %dP: %s", screen + 1, error.what());
		receiver.error = error.what();
		receiver.retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		receiver.spout.reset();
		return receiver.Frame();
	}
}

void gdxsv_multi_pov_viewer_vulkan_term(bool reset_device) {
	for (const auto& receiver : viewer.receivers) {
		if (!receiver.images.empty() || receiver.displayed) {
			if (auto* context = dynamic_cast<VulkanContext*>(GraphicsContext::Instance())) context->WaitIdle();
			break;
		}
	}
	for (auto& receiver : viewer.receivers) receiver = {};
	if (!reset_device) return;
	viewer.context.reset();
	viewer.device.reset();
	viewer.probed_device = nullptr;
	viewer.error = kImportUnavailable;
	viewer.retries = 0;
	viewer.retry = {};
}
#endif
