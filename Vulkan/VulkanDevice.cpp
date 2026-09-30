
#include "VulkanDevice.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "../SparseMap.h"
#include "VulkanCommandPool.h"
#include "VulkanImageView.h"
#include "VulkanMemory.h"
#include "VulkanRHI.h"
#include "VulkanSwapChian.h"
#include "VulkanTexture.h"

// ============================================================
// 描述符路径开关：环境变量 FISIR_DISABLE_DESCRIPTOR_HEAP=1
// 强制所有路径走传统 DescriptorSet 降级实现，无视设备对
// VK_EXT_descriptor_heap 的实际支持。判定点就是唯一事实源
// isDescriptorHeapSupported()（见下方实现），管线创建 /
// ResourcePack 创建 / 绑定 / Buffer usage 全都问它。
// ============================================================

namespace FISIR {
	extern VkInstance GetGlobalInstance();
#ifdef _DEBUG
	static PFN_vkSetDebugUtilsObjectNameEXT    __SetDebugUtilsObjectName = nullptr;
#endif // _DEBUG

	static sparse_map<uint32_t, VulkanQueue*> FamilyIndexToQue;

	static VulkanQueue* getVulkanQue(VulkanDevice* device, uint32_t familyIndex, const char* name) {
		if (FamilyIndexToQue.contains(familyIndex)) return FamilyIndexToQue[familyIndex];
		return FamilyIndexToQue[familyIndex] = new VulkanQueue(device, familyIndex, name);
	}

	struct __VkDeviceData {
		VulkanDeviceExtensions Externsions;
		VkPhysicalDevice mPhysicalDevice;
		VkPhysicalDeviceIDProperties mGpuID;
		VkPhysicalDeviceDescriptorHeapPropertiesEXT mDescriptorHeapProperties;
		VkPhysicalDeviceProperties2 mPhysicalDeviceProperties;
		std::vector<VkQueueFamilyProperties> mQueueFamilyVkQueueFamilyProperties;
		std::vector<VkQueueFamilyProperties> mQueueFamilyProperties;
		VkDevice mLogicalDevice = nullptr;
		std::unordered_map<RHIResource*, VkAccessFlagBits> ResourceAccessMap;
		std::unordered_map<RHITexture*, VkImageLayout> TextureLayoutMap;
		std::unordered_map<VkSurfaceKHR, VulkanQueue*> SurafacePresentQue;
		bool DescriptorHeapSupport = false;
		bool SwapchainMaintenance1Support = false;
		int GQueFamilyIndex = -1;
		int CQueFamilyIndex = -1;
		int TQueFamilyIndex = -1;
	};


	void setVkObjectName(VkDevice device, uint64_t objectHandle, VkObjectType objectType, const char* name) {
#ifdef _DEBUG

		if (__SetDebugUtilsObjectName) {
			VkDebugUtilsObjectNameInfoEXT nameInfo{
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
				.pNext = nullptr,
				.objectType = objectType,
				.objectHandle = objectHandle,
				.pObjectName = name
			};
			__SetDebugUtilsObjectName(device, &nameInfo);
		}
#endif // _DEBUG
	}

	static ResourceAccess getResourceAccessFromVK(VkAccessFlagBits Access) {
		switch (Access)
		{
		case VK_ACCESS_SHADER_READ_BIT:
			return ResourceAccess::ShaderReadOnly;
		case VK_ACCESS_SHADER_WRITE_BIT:
			return ResourceAccess::ShaderWriteOnly;
			case (VkAccessFlagBits)(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) :
				return ResourceAccess::ShaderReadWrite;
			case VK_ACCESS_TRANSFER_READ_BIT:
				return ResourceAccess::TransferSrc;
			case VK_ACCESS_TRANSFER_WRITE_BIT:
				return ResourceAccess::TransferDst;
			default:
				return ResourceAccess::Undefined;
		}
	}

	static TextureLayout getTextureLayoutFromVK(VkImageLayout Layout) {
		switch (Layout) {
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return TextureLayout::ColorAttachmentOptimal;
		case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
			return TextureLayout::DepthStencilAttachmentOptimal;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return TextureLayout::ShaderReadOnlyOptimal;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			return TextureLayout::TransferSrcOptimal;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			return TextureLayout::TransferDstOptimal;
		default:
			return TextureLayout::Undefined;
		}
	}

	static VkAccessFlagBits getAccessFlagBits(ResourceAccess type) {
		switch (type)
		{
		case ResourceAccess::ShaderReadOnly:
			return VK_ACCESS_SHADER_READ_BIT;
		case ResourceAccess::ShaderWriteOnly:
			return VK_ACCESS_SHADER_WRITE_BIT;
		case ResourceAccess::ShaderReadWrite:
			return (VkAccessFlagBits)(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		case ResourceAccess::TransferSrc:
			return VK_ACCESS_TRANSFER_READ_BIT;
		case ResourceAccess::TransferDst:
			return VK_ACCESS_TRANSFER_WRITE_BIT;
		case ResourceAccess::ColorAttachmentWrite:
			return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		default:
			return (VkAccessFlagBits)0;
		}
	}

	static VkImageLayout getImageLayout(TextureLayout type) {
		switch (type) {
		case TextureLayout::ColorAttachmentOptimal:
			return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case TextureLayout::DepthStencilAttachmentOptimal:
			return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		case TextureLayout::ShaderReadOnlyOptimal:
			return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case TextureLayout::TransferSrcOptimal:
			return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		case TextureLayout::TransferDstOptimal:
			return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		default:
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	VulkanDevice::~VulkanDevice() {
		delete mData;
	}

	bool VulkanDevice::Init() {
		uint32_t queueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(mData->mPhysicalDevice, &queueCount, nullptr);
		mData->mQueueFamilyProperties.resize(queueCount);
		vkGetPhysicalDeviceQueueFamilyProperties(mData->mPhysicalDevice, &queueCount, mData->mQueueFamilyProperties.data());

		if (!InitDevice()) return false;

#ifdef _DEBUG
		__SetDebugUtilsObjectName = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(GetGlobalInstance(), "vkSetDebugUtilsObjectNameEXT");
#endif
		mData->mPhysicalDeviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

		if (isDescriptorHeapSupported()) {
			mData->mDescriptorHeapProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
			mData->mPhysicalDeviceProperties.pNext = &mData->mDescriptorHeapProperties;
		}
		vkGetPhysicalDeviceProperties2(mData->mPhysicalDevice, &mData->mPhysicalDeviceProperties);

		return true;
	}

	void VulkanDevice::Destory() {
		vkDeviceWaitIdle(mData->mLogicalDevice);

		delete mAllocator;

		for (auto& [id, Que] : FamilyIndexToQue) {
			delete Que;
		}

		vkDestroyDevice(mData->mLogicalDevice, nullptr);
	}

	void VulkanDevice::setResourceAccess(RHIResource* resource, ResourceAccess access) {
		mData->ResourceAccessMap[resource] = getAccessFlagBits(access);
	}

	void VulkanDevice::setTextureLayout(RHITexture* texture, TextureLayout layout) {
		mData->TextureLayoutMap[texture] = getImageLayout(layout);
	}

	void VulkanDevice::waitIdle() {
		vkDeviceWaitIdle(mData->mLogicalDevice);
	}

	ResourceAccess VulkanDevice::getResourceAccess(RHIResource* resource) {
		auto it = mData->ResourceAccessMap.find(resource);
		if (it != mData->ResourceAccessMap.end()) return getResourceAccessFromVK(it->second);
		return ResourceAccess::Undefined;
	}


	TextureLayout VulkanDevice::getTextureLayout(RHITexture* texture) {
		auto it = mData->TextureLayoutMap.find(texture);
		if (it != mData->TextureLayoutMap.end()) return getTextureLayoutFromVK(it->second);
		return TextureLayout::Undefined;
	}

	void VulkanDevice::submitCommandBuffer(const std::vector<VkCommandBuffer_T*>& cmds, CmdType poolType,
		const std::vector<RHISemaphore*>& SignalSemaphores,
		const std::vector<RHISemaphore*>& WaitSemaphores, VkFence Fence) {
		switch (poolType) {
		case CmdType::Render:
			mGraphicQue->Submit(cmds, SignalSemaphores, WaitSemaphores, Fence);
			break;
		case CmdType::Compute:
			mComputeQue->Submit(cmds, SignalSemaphores, WaitSemaphores, Fence);
			break;
		case CmdType::Transfer:
			mTransferQueue->Submit(cmds, SignalSemaphores, WaitSemaphores, Fence);
			break;
		default:
			Error("Invalid Command Pool Type!");
			break;
		}
	}

	bool VulkanDevice::isDescriptorHeapSupported() const {
		// 运行时开关：FISIR_DISABLE_DESCRIPTOR_HEAP=1 强制走传统 DescriptorSet 降级实现，
		// 无视设备对 VK_EXT_descriptor_heap 的支持情况。用途：在桌面（有该扩展）上验证
		// 移动端/无扩展设备的代码路径。必须放在这个唯一事实源上 —— 管线创建、ResourcePack、
		// 绑定、Buffer usage 全都问这里，开关一改就整条链一致。
		static const bool forceDescriptorSet = [] {
			const char* value = getenv("FISIR_DISABLE_DESCRIPTOR_HEAP");
			return value && value[0] != '\0' && value[0] != '0';
		}();
		if (forceDescriptorSet) return false;
		return mData->DescriptorHeapSupport;
	}

	bool VulkanDevice::isSwapchainMaintenance1Supported() const {
		return mData->SwapchainMaintenance1Support;
	}

	DescriptorSizes& VulkanDevice::getHeapSizeInfo() {
		if (!HeapSizes.isInit) QueryDescriptorSizes();
		return HeapSizes;
	}

	VkPhysicalDeviceDescriptorHeapPropertiesEXT& VulkanDevice::getDescriptorHeapProperties() {
		return mData->mDescriptorHeapProperties;
	}

	void VulkanDevice::QueryDescriptorSizes() {
		VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{};
		heapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;

		VkPhysicalDeviceProperties2 props2{};
		props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props2.pNext = &heapProps;
		vkGetPhysicalDeviceProperties2(getPhysicalDevice(), &props2);

		HeapSizes.resourceHeapAlignment = heapProps.resourceHeapAlignment;
		HeapSizes.samplerHeapAlignment = heapProps.samplerHeapAlignment;
		HeapSizes.bufferAlignment = heapProps.bufferDescriptorAlignment;
		HeapSizes.imageAlignment = heapProps.imageDescriptorAlignment;
		HeapSizes.samplerAlignment = heapProps.samplerDescriptorAlignment;

		HeapSizes.bufferDescriptorSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		HeapSizes.imageDescriptorSize = static_cast<uint32_t>(heapProps.imageDescriptorSize);
		HeapSizes.samplerDescriptorSize = static_cast<uint32_t>(heapProps.samplerDescriptorSize);

		HeapSizes.maxResourceHeapSize = heapProps.maxResourceHeapSize;
		HeapSizes.maxSamplerHeapSize = heapProps.maxSamplerHeapSize;
		HeapSizes.minResourceReserved = heapProps.minResourceHeapReservedRange;
		HeapSizes.minSamplerReserved = heapProps.minSamplerHeapReservedRange;
		HeapSizes.maxEmbeddedSamplers = heapProps.maxDescriptorHeapEmbeddedSamplers;
		HeapSizes.isInit = 1;
		Debug("Descriptor Heap Sizes: minResourceReserved = {}, minSamplerReserved = {}",
			HeapSizes.minResourceReserved, HeapSizes.minSamplerReserved);
	}

	static bool isDeviceExtensionSupported(VkPhysicalDevice device, const char* extensionName) {
		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> extensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
		for (const auto& extension : extensions) {
			if (strcmp(extension.extensionName, extensionName) == 0) return true;
		}
		return false;
	}

	// 运行时新建视口的交换链（Init 之后才 RHICreateViewport 的，例如 ImGui 拖出去的独立窗口）。
	//
	// Init 阶段的做法是给每个视口分配**独占的呈现队列族**，好让各视口的呈现互不阻塞 —— 但那些
	// 队列必须在 vkCreateDevice 时就写进 VkDeviceQueueCreateInfo，**事后无法再加队列**。所以运行时
	// 视口只能走另一条路：**复用设备创建时已经建好的队列族**（图形 → 计算 → 传输），在其中挑一个
	// 对该 surface 支持呈现的。代价是它和图形/计算队列共享同一个队列（呈现会与渲染串行），
	// 功能上完全可用；这也正是"从已有的渲染队列创建 ViewPort Swapchain"。
	int32_t VulkanDevice::FindPresentQueueFamilyForSurface(VkSurfaceKHR_T* surface) {
		if (surface == VK_NULL_HANDLE) return -1;

		const int32_t candidates[3] = { mData->GQueFamilyIndex, mData->CQueFamilyIndex, mData->TQueFamilyIndex };
		for (int32_t family : candidates) {
			if (family < 0) continue;
			VkBool32 supportPresent = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(mData->mPhysicalDevice, (uint32_t)family, surface, &supportPresent);
			if (!supportPresent) continue;
			Info("[Vulkan] runtime viewport: creating swapchain on reused queue family {} (no new queues after Init)", family);
			return family;
		}

		// 只试 G/C/T：只有这三个队列族能保证设备创建时就建好了队列。
		// 去试别的族会因为 vkGetDeviceQueue 拿不到队列而更糟，所以这里直接失败更清楚。
		Error("[Vulkan] runtime viewport: no graphics/compute/transfer family can present to this surface");
		return -1;
	}

	bool VulkanDevice::InitDevice() {
		// VK_KHR_swapchain_maintenance1 depends on VK_KHR_surface_maintenance1 — probe both.
		const bool swapchainMaintenance1ExtensionSupported =
			isDeviceExtensionSupported(mData->mPhysicalDevice, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
			isDeviceExtensionSupported(mData->mPhysicalDevice, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);

		std::vector<const char*> extensions{
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		};

		// 64 位原子（RWByteAddressBuffer.InterlockedMin64 等编译出的 OpAtomicUMin(64)）所需的
		// shaderBufferInt64Atomics。注意必须用专用的 ShaderAtomicInt64 结构，而不是 Vulkan12Features：
		// 后者与本链里已有的 TimelineSemaphoreFeatures / BufferDeviceAddressFeatures 互斥（见 Vulkan 规范）。
		// Slang 编出的顶点着色器会在 SPIR-V 里声明 DrawParameters 能力集（用到 SV_VertexID /
		// SV_InstanceID 的入口都会带上），而该能力集要求设备开启 shaderDrawParameters
		// （Vulkan 1.1 核心特性，桌面驱动普遍支持）。与下面几个特性同法：查询结果留在同一结构里，
		// 结构又原样进 vkCreateDevice，于是「支持即开启」。
		VkPhysicalDeviceShaderDrawParametersFeatures drawParametersFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
		};

		VkPhysicalDeviceShaderAtomicInt64Features atomicInt64Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,
			.pNext = &drawParametersFeatures,
		};

		VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
			.pNext = &atomicInt64Features,
		};

		VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
			.pNext = &timelineFeature
		};

		VkPhysicalDeviceBufferDeviceAddressFeatures supportedFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
			.pNext = &swapchainMaintenance1Features,
		};

		VkPhysicalDeviceDescriptorHeapFeaturesEXT DescriptorHeapFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
			.pNext = &supportedFeatures
		};

		VkPhysicalDeviceFeatures2 deviceFeatures2{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &DescriptorHeapFeatures,
		};

		vkGetPhysicalDeviceFeatures2(mData->mPhysicalDevice, &deviceFeatures2);

		// Only enable the maintenance1 feature when both the extension and the
		// feature are actually supported by the physical device.
		const bool enableSwapchainMaintenance1 =
			swapchainMaintenance1ExtensionSupported &&
			swapchainMaintenance1Features.swapchainMaintenance1 == VK_TRUE;
		mData->SwapchainMaintenance1Support = enableSwapchainMaintenance1;

		const bool enableTimeLine = timelineFeature.timelineSemaphore;

		if (!enableTimeLine) {
			Warn("TimeLine Semaphore No Suport");
		}

		if (enableSwapchainMaintenance1) {
			extensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
			extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
			swapchainMaintenance1Features.swapchainMaintenance1 = VK_TRUE;
		}
		else {
			Warn("VK_KHR_swapchain_maintenance1 not supported - present fence unavailable");
		}

		VkDeviceCreateInfo deviceCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		};

		// Build pNext chain: DescriptorHeap → BufferDeviceAddress → SwapchainMaintenance1
		if (DescriptorHeapFeatures.descriptorHeap == VK_TRUE && supportedFeatures.bufferDeviceAddress == VK_TRUE) {
			extensions.push_back("VK_EXT_descriptor_heap");
			Debug("Device Support Descriptor Heap, Enable Descriptor Heap Extension!");
			mData->DescriptorHeapSupport = true;

			DescriptorHeapFeatures.pNext = &supportedFeatures;
			supportedFeatures.bufferDeviceAddress = VK_TRUE;
			supportedFeatures.pNext = enableSwapchainMaintenance1 ? &swapchainMaintenance1Features : (void*)&timelineFeature;
			deviceCreateInfo.pNext = &DescriptorHeapFeatures;
		}
		else {
			Warn("Device Not Support Descriptor Buffer, Fallback To Normal Descriptor Set!");
			// 不能在这里断链成 nullptr：timelineFeature.pNext 上挂着 atomicInt64Features，
			// 断开会让 vkCreateDevice 既拿不到 timelineSemaphore，也拿不到
			// shaderBufferInt64Atomics（Nanite 的 64 位 VisBuffer 原子依赖后者）。
			deviceCreateInfo.pNext = enableSwapchainMaintenance1 ? (void*)&swapchainMaintenance1Features : (void*)&timelineFeature;
		}
		// 开启 shaderInt64（64 位整型运算 / Int64 原子）。设备不支持时回退为关闭并告警，
		// 避免 vkCreateDevice 因 VK_ERROR_FEATURE_NOT_PRESENT 直接失败。
		VkPhysicalDeviceFeatures2 enableFeatures2{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		};
		enableFeatures2.features.shaderInt64 = deviceFeatures2.features.shaderInt64 ? VK_TRUE : VK_FALSE;
		if (deviceFeatures2.features.shaderInt64 != VK_TRUE) {
			Warn("Device Does Not Support shaderInt64");
		}
		// 硬光栅 GPU-driven：multiDrawIndirect 允许 vkCmdDrawIndirect 的 drawCount>1（逐簇派发）；
		// vertexPipelineStoresAndAtomics 允许 VS 读取未加 NonWritable 装饰的存储缓冲
		// （DXC 对 ByteAddressBuffer/StructuredBuffer 一律不加 NonWritable，见 .claude 内存）。
		// 两者均为 Vulkan 1.0 核心特性，桌面 GPU 普遍支持。
		enableFeatures2.features.multiDrawIndirect = deviceFeatures2.features.multiDrawIndirect ? VK_TRUE : VK_FALSE;
		enableFeatures2.features.vertexPipelineStoresAndAtomics = deviceFeatures2.features.vertexPipelineStoresAndAtomics ? VK_TRUE : VK_FALSE;
		// 硬光栅 PS 直接往 FrameBuffer（存储缓冲）写像素 → 片元阶段也需要存储缓冲/图像写 +
		// 原子（DXC 不会给 RWByteAddressBuffer 加 NonWritable，不禁用该特性会直接校验报错）。
		enableFeatures2.features.fragmentStoresAndAtomics = deviceFeatures2.features.fragmentStoresAndAtomics ? VK_TRUE : VK_FALSE;
		if (!deviceFeatures2.features.multiDrawIndirect) {
			Warn("Device Does Not Support multiDrawIndirect");
		}
		if (!deviceFeatures2.features.vertexPipelineStoresAndAtomics) {
			Warn("Device Does Not Support vertexPipelineStoresAndAtomics");
		}
		if (!deviceFeatures2.features.fragmentStoresAndAtomics) {
			Warn("Device Does Not Support fragmentStoresAndAtomics");
		}
		// atomicInt64Features 已通过 timelineFeature.pNext 进入查询链与使能链，
		// shaderBufferInt64Atomics 字段由 vkGetPhysicalDeviceFeatures2 填充为设备支持值，
		// 同一结构随后经 enableFeatures2 → deviceCreateInfo.pNext 交给 vkCreateDevice，
		// 于是「支持即开启」。Nanite 示例的 VisBuffer 用 64 位原子（OpAtomicUMin(64)），
		// 设备不支持时该示例的管线会创建失败。
		if (atomicInt64Features.shaderBufferInt64Atomics != VK_TRUE) {
			Warn("Device Does Not Support shaderBufferInt64Atomics (Nanite 64-bit VisBuffer atomics unavailable)");
		}
		if (drawParametersFeatures.shaderDrawParameters != VK_TRUE) {
			Warn("Device Does Not Support shaderDrawParameters (Slang 顶点着色器声明的 DrawParameters 能力集不可用)");
		}

		// 把 enableFeatures2 插到 pNext 链最前端，使 shaderInt64 在 vkCreateDevice 生效。
		// 注意：VkDeviceCreateInfo::pEnabledFeatures 与 pNext 中的 VkPhysicalDeviceFeatures2 互斥，
		// 这里只走 pNext 一条链（原链可能是 DescriptorHeap / swapchainMaintenance1 / timeline）。
		enableFeatures2.pNext = (void*)deviceCreateInfo.pNext;
		deviceCreateInfo.pNext = &enableFeatures2;


		deviceCreateInfo.enabledExtensionCount = uint32_t(extensions.size()),
			deviceCreateInfo.ppEnabledExtensionNames = extensions.data(),

			mData->GQueFamilyIndex = -1;
		mData->CQueFamilyIndex = -1;
		mData->TQueFamilyIndex = -1;
		uint32_t NumProrities = 0;
		std::vector<VkDeviceQueueCreateInfo> QueInfos;
		// 与 QueInfos 一一对应：pQueuePriorities 必须指向**稳定**的内存，不能指向会被搬移的临时 vector
		std::vector<std::vector<float>> QueuePriorityStorage;
		for (uint32_t FamilyIndex = 0; FamilyIndex < mData->mQueueFamilyProperties.size(); FamilyIndex++) {
			const auto& Prpos = mData->mQueueFamilyProperties[FamilyIndex];
			bool IsVaild = false;
			if (!IsVaild && (Prpos.queueFlags & VK_QUEUE_GRAPHICS_BIT) && mData->GQueFamilyIndex == -1) {
				mData->GQueFamilyIndex = FamilyIndex;
				IsVaild = 1;
			}

			if (!IsVaild && (Prpos.queueFlags & VK_QUEUE_COMPUTE_BIT) && mData->CQueFamilyIndex == -1) {
				mData->CQueFamilyIndex = FamilyIndex;
				IsVaild = 1;

			}

			if (!IsVaild && (Prpos.queueFlags & VK_QUEUE_TRANSFER_BIT) && mData->TQueFamilyIndex == -1) {
				mData->TQueFamilyIndex = FamilyIndex;
				IsVaild = 1;

			}

			if (!IsVaild) continue;
			VkDeviceQueueCreateInfo Que = {
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = FamilyIndex,
				.queueCount = Prpos.queueCount
			};
			QueInfos.push_back(Que);
			NumProrities += Prpos.queueCount;
		}

		// ── Pass 2：把 Pass 1 没认领到的角色回填到已有族上 ────────────────────────
		// Pass 1 是「一个族只认领一个角色」，在桌面驱动上正好（G/C/T 各自有独立族）。
		// 但移动 GPU（Adreno / Mali）通常只有**一个** GRAPHICS|COMPUTE|TRANSFER 族：
		// Pass 1 认领了 G 之后就不再认领 C/T，于是它们一直是 -1，下面那个检查便直接报
		// "The Graphic Que haven't found" 让整个设备初始化失败 —— 一个只在手机上暴露的 bug。
		// 这里按「支持该位的最小族号」回填（多族设备不受影响，因为首选角色已在前一轮定好）。
		if (mData->CQueFamilyIndex == -1 || mData->TQueFamilyIndex == -1) {
			for (uint32_t FamilyIndex = 0; FamilyIndex < mData->mQueueFamilyProperties.size(); FamilyIndex++) {
				const auto& Prpos = mData->mQueueFamilyProperties[FamilyIndex];
				if (mData->CQueFamilyIndex == -1 && (Prpos.queueFlags & VK_QUEUE_COMPUTE_BIT))
					mData->CQueFamilyIndex = FamilyIndex;
				// 传输能力由 GRAPHICS / COMPUTE **隐含**：Vulkan 只在"专用传输族"上置
				// VK_QUEUE_TRANSFER_BIT，图形/计算族不置也算支持传输。Adreno 的族 0 就是
				// GRAPHICS|COMPUTE（没置 TRANSFER 位），只认标志位的话 T 会一直是 -1。
				if (mData->TQueFamilyIndex == -1 &&
					((Prpos.queueFlags & VK_QUEUE_TRANSFER_BIT) ||
					 (Prpos.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))))
					mData->TQueFamilyIndex = FamilyIndex;
			}
			Info("[Vulkan] 单队列族设备：C/T 回填到族 {} / {}（G={}）",
				mData->CQueFamilyIndex, mData->TQueFamilyIndex, mData->GQueFamilyIndex);
		}

		// ── 队列：设备创建时一次性要足，并按用途分三档优先级 ────────────────────
		//
		// 为什么这么做：视口/交换链改成**初始化之后**才创建（运行期随时会多出窗口），而 Vulkan 的
		// 队列必须在 vkCreateDevice 时写进 VkDeviceQueueCreateInfo、事后加不了。所以这里把队列
		// 一次要足（≥ kMinTotalQueues），运行期新建视口直接从这个池子里取一条，不必再"预先建窗口
		// + 预先建队列"。优先级三档（数值越低越优先让位）：
		//   · kPriorityRender   —— 图形/计算/传输的**主队列**（真正跑渲染/资源/计算的那些）：最低
		//   · kPriorityViewport —— 预留/已被视口占用的队列：次低
		//   · kPrioritySpare    —— 还没被用上的空闲队列：最高
		// 这样调度器会优先让"空闲的备用队列"抢占，而正在干活的渲染队列排最后，互不干扰。
		constexpr uint32_t kMinTotalQueues = 8;      // 至少要这么多条
		constexpr uint32_t kReservedViewportQueues = 3;      // 其中按"次低"档预留给视口的条数
		constexpr float    kPriorityRender = 0.0f;   // 渲染/资源/计算：最低
		constexpr float    kPriorityViewport = 0.33f;  // 视口占用：次低
		constexpr float    kPrioritySpare = 1.0f;   // 空闲备用：最高

		// 1) 先把 G/C/T 三个族的队列全部要出来（每族第 0 条就是我们的主队列）
		uint32_t totalQueues = 0;
		for (auto& info : QueInfos) totalQueues += info.queueCount;

		// 2) 还不够 kMinTotalQueues 就把别的族也拉进来（优先支持呈现的族 —— 运行期视口要用它 present）
		for (uint32_t FamilyIndex = 0; FamilyIndex < mData->mQueueFamilyProperties.size() && totalQueues < kMinTotalQueues; FamilyIndex++) {
			if (FamilyIndex == mData->GQueFamilyIndex || FamilyIndex == mData->CQueFamilyIndex ||
				FamilyIndex == mData->TQueFamilyIndex) continue;
			const auto& Prpos = mData->mQueueFamilyProperties[FamilyIndex];
			if (Prpos.queueCount == 0) continue;
			VkDeviceQueueCreateInfo Que = {
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = FamilyIndex,
				.queueCount = Prpos.queueCount
			};
			QueInfos.push_back(Que);
			totalQueues += Prpos.queueCount;
			Info("[Vulkan] queue pool top-up: family {} contributes {} queues (total {})", FamilyIndex, Prpos.queueCount, totalQueues);
		}
		if (totalQueues < kMinTotalQueues)
			Warn("[Vulkan] hardware offers only {} queues (target >= {}); runtime viewport count is limited", totalQueues, kMinTotalQueues);

		// 缺任何一个角色都不行：缺 G 没法渲染，缺 C/T 会让 mComputeQue/mTransferQueue 拿不到队列
		// （注意这里必须检查 G，原实现检查的是 C 却打印 "Graphic Que"）。
		if (mData->GQueFamilyIndex == -1 || mData->CQueFamilyIndex == -1 || mData->TQueFamilyIndex == -1) {
			Error("Error Device: 找不到队列族 G={} C={} T={}",
				mData->GQueFamilyIndex, mData->CQueFamilyIndex, mData->TQueFamilyIndex);
			return false;
		}

		// 3) 逐条填优先级。注意 pQueuePriorities 必须指向**稳定**的数组：先把每个族自己的
		//    vector 放进 QueuePriorityStorage（它不会再被搬移），再把指针挂上去。
		QueuePriorityStorage.resize(QueInfos.size());
		NumProrities = 0;
		for (size_t i = 0; i < QueInfos.size(); ++i) {
			auto& prio = QueuePriorityStorage[i];
			prio.assign(QueInfos[i].queueCount, kPrioritySpare);
			if (QueInfos[i].queueFamilyIndex == mData->GQueFamilyIndex ||
				QueInfos[i].queueFamilyIndex == mData->CQueFamilyIndex ||
				QueInfos[i].queueFamilyIndex == mData->TQueFamilyIndex)
				prio[0] = kPriorityRender;             // 主队列：最低
			QueInfos[i].pQueuePriorities = prio.data();
			NumProrities += QueInfos[i].queueCount;
		}
		// 再把前 kReservedViewportQueues 条"非主"队列标成次低（＝预留给视口的那几条）
		{
			uint32_t reserved = 0;
			for (size_t i = 0; i < QueInfos.size() && reserved < kReservedViewportQueues; ++i)
				for (uint32_t q = 0; q < QueInfos[i].queueCount && reserved < kReservedViewportQueues; ++q)
					if (QueuePriorityStorage[i][q] == kPrioritySpare) {
						QueuePriorityStorage[i][q] = kPriorityViewport;
						++reserved;
					}
			Info("[Vulkan] queue pool: {} queues across {} families; {} reserved for viewports (mid priority), render/resource/compute lowest, spares highest",
				NumProrities, QueInfos.size(), reserved);
		}
		deviceCreateInfo.queueCreateInfoCount = (uint32_t)QueInfos.size();
		deviceCreateInfo.pQueueCreateInfos = QueInfos.data();

		Debug("=== Before vkCreateDevice ===");
		Debug("descriptorHeap = {}", DescriptorHeapFeatures.descriptorHeap);
		Debug("bufferDeviceAddress = {}", supportedFeatures.bufferDeviceAddress);
		Debug("pNext chain: {} -> {}", (size_t)deviceCreateInfo.pNext,
			deviceCreateInfo.pNext ? (size_t)deviceCreateInfo.pNext : 0);
		auto res = vkCreateDevice(mData->mPhysicalDevice, &deviceCreateInfo, nullptr, &(mData->mLogicalDevice));
		if (res != VK_SUCCESS) {
			Error("Device Create failed! : {}", (uint32_t)res);
			switch (res) {
			case VK_ERROR_OUT_OF_HOST_MEMORY:
				Error("Device  Out of host memory!");
				break;
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:
				Error("Device  Out of device memory!");
				break;
			case VK_ERROR_INITIALIZATION_FAILED:
				Error("Device Initialization failed!");
				break;

			}
			return false;
		}
		mGraphicQue = getVulkanQue(this, mData->GQueFamilyIndex, "Graphic");
		if (mData->CQueFamilyIndex == -1) mData->CQueFamilyIndex = mData->GQueFamilyIndex;
		mComputeQue = getVulkanQue(this, mData->CQueFamilyIndex, "Compute");
		if (mData->TQueFamilyIndex == -1) mData->TQueFamilyIndex = mData->CQueFamilyIndex;
		mTransferQueue = getVulkanQue(this, mData->TQueFamilyIndex, "Transfer");

		Info("Queue families: Graphics={}, Compute={}, Transfer={}",
			mData->GQueFamilyIndex, mData->CQueFamilyIndex, mData->TQueFamilyIndex);

		if (!mGraphicQue) Error("Error GraphicQue is null");
		if (!mComputeQue) Error("Error ComputeQue is null");
		if (!mTransferQueue) Error("Error TransferQue is null");
		mAllocator = new VulkanMemoryAllocator();
		mAllocator->init(this);
		Debug("Well vulkan Device Create Success!");

		return true;
	}

	VulkanDevice::VulkanDevice(VkPhysicalDevice device) {
		mData = new __VkDeviceData();
		mData->mPhysicalDevice = device;

	}


	VkDevice VulkanDevice::getLogicalDevice() {
		//Export Func
		return mData->mLogicalDevice;
	}

	VkPhysicalDevice_T* VulkanDevice::getPhysicalDevice() {
		return mData->mPhysicalDevice;
	}

	uint32_t VulkanDevice::getMaxDescriptorSetSamplers() const {
		return mData->mPhysicalDeviceProperties.properties.limits.maxDescriptorSetSamplers;
	}

	uint32_t VulkanDevice::getMaxDescriptorSetStorageImages() const {
		return mData->mPhysicalDeviceProperties.properties.limits.maxDescriptorSetStorageImages;
	}

	uint32_t VulkanDevice::getMaxDescriptorSetCombinedImageSamplers() const {
		return  mData->mPhysicalDeviceProperties.properties.limits.maxDescriptorSetSampledImages;
	}

	uint32_t VulkanDevice::getMaxDescriptorSetUniformBuffers() const {
		return mData->mPhysicalDeviceProperties.properties.limits.maxDescriptorSetUniformBuffers;
	}

	float VulkanDevice::getTimestampPeriod() const {
		if (!(mData->mPhysicalDeviceProperties.properties.limits.timestampComputeAndGraphics))
			return 0.0f;   // 设备不支持图形队列时间戳
		return mData->mPhysicalDeviceProperties.properties.limits.timestampPeriod;
	}



}
