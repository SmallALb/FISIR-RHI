
#include "VulkanDevice.h"

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
// 测试开关：强制所有路径走传统 DescriptorSet 降级实现，
// 无视设备对 VK_EXT_descriptor_heap 的实际支持。
// 此开关必须放在唯一事实源 isDescriptorHeapSupported() 上，
// 才能让「管线创建 / ResourcePack 创建 / 绑定 / Buffer usage」
// 全部一致地走 Set 路径。测试完成后删除此宏。
// ============================================================
//#define FISIR_FORCE_DESCRIPTOR_SET 1

namespace FISIR{
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
		case (VkAccessFlagBits)(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT):
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

	bool VulkanDevice::Init(const std::vector<VulkanViewport*>& viewports, std::unordered_map<RHIViewport*, VulkanSwapChain*>& ViewPortSwapChainCache) {
		uint32_t queueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(mData->mPhysicalDevice, &queueCount, nullptr);
		mData->mQueueFamilyProperties.resize(queueCount);
		vkGetPhysicalDeviceQueueFamilyProperties(mData->mPhysicalDevice, &queueCount, mData->mQueueFamilyProperties.data());
		
		if (!InitDevice(viewports, ViewPortSwapChainCache)) return false;

#ifdef _DEBUG
		__SetDebugUtilsObjectName = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(GetGlobalInstance(), "vkSetDebugUtilsObjectNameEXT");
#endif

		if (isDescriptorHeapSupported()) {
			mData->mDescriptorHeapProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
			mData->mPhysicalDeviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			mData->mPhysicalDeviceProperties.pNext = &mData->mDescriptorHeapProperties;
		}
		vkGetPhysicalDeviceProperties2(mData->mPhysicalDevice, &mData->mPhysicalDeviceProperties);

		return true;
	}

	void VulkanDevice::Destory() {
		vkDeviceWaitIdle(mData->mLogicalDevice);
		
		delete mAllocator;

		for (auto &[id, Que] : FamilyIndexToQue) {
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
#if FISIR_FORCE_DESCRIPTOR_SET
		// 测试模式：强制降级到 DescriptorSet
		return false;
#else
		return mData->DescriptorHeapSupport;
#endif
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

	void VulkanDevice::QueryDescriptorSizes(){
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

	bool VulkanDevice::InitDevice(const std::vector<VulkanViewport*>& viewports, std::unordered_map<RHIViewport*, VulkanSwapChain*>& ViewPortSwapChainCache) {
		// VK_KHR_swapchain_maintenance1 depends on VK_KHR_surface_maintenance1 — probe both.
		const bool swapchainMaintenance1ExtensionSupported =
			isDeviceExtensionSupported(mData->mPhysicalDevice, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
			isDeviceExtensionSupported(mData->mPhysicalDevice, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);

		std::vector<const char*> extensions {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		};

		VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1Features {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		};

		VkPhysicalDeviceBufferDeviceAddressFeatures supportedFeatures {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
			.pNext = &swapchainMaintenance1Features,
		};

		VkPhysicalDeviceDescriptorHeapFeaturesEXT DescriptorHeapFeatures {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
			.pNext = &supportedFeatures
		};

		VkPhysicalDeviceFeatures2 deviceFeatures2 {
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

		if (enableSwapchainMaintenance1) {
			extensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
			extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
			swapchainMaintenance1Features.swapchainMaintenance1 = VK_TRUE;
		} else {
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
			supportedFeatures.pNext = enableSwapchainMaintenance1 ? &swapchainMaintenance1Features : nullptr;
			deviceCreateInfo.pNext = &DescriptorHeapFeatures;
		}
		else {
			Warn("Device Not Support Descriptor Buffer, Fallback To Normal Descriptor Set!");
			deviceCreateInfo.pNext = enableSwapchainMaintenance1 ? &swapchainMaintenance1Features : nullptr;
		}

		deviceCreateInfo.enabledExtensionCount = uint32_t(extensions.size()),
		deviceCreateInfo.ppEnabledExtensionNames = extensions.data(),

		mData->GQueFamilyIndex = -1;
		mData->CQueFamilyIndex = -1;
		mData->TQueFamilyIndex = -1;
		uint32_t NumProrities = 0;
		std::vector<VkDeviceQueueCreateInfo> QueInfos;
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

		std::unordered_set<uint32_t> PresentQueIndex;
		for (auto viewport : viewports) {
			bool Done = 0;
			for (uint32_t FamilyIndex = 0; FamilyIndex < mData->mQueueFamilyProperties.size(); FamilyIndex++) 
				if (FamilyIndex != mData->GQueFamilyIndex && FamilyIndex != mData->CQueFamilyIndex && FamilyIndex != mData->TQueFamilyIndex && !PresentQueIndex.contains(FamilyIndex)){
					VkBool32 supportPresent = 0;
					vkGetPhysicalDeviceSurfaceSupportKHR(mData->mPhysicalDevice, FamilyIndex, viewport->getVkSurface(), &supportPresent);
					if (supportPresent) {
						PresentQueIndex.insert(FamilyIndex);
						ViewPortSwapChainCache[viewport] = new VulkanSwapChain(viewport, FamilyIndex);
						const auto& Prpos = mData->mQueueFamilyProperties[FamilyIndex];
						VkDeviceQueueCreateInfo Que = {
							.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
							.queueFamilyIndex = FamilyIndex,
							.queueCount = Prpos.queueCount
						};
						QueInfos.push_back(Que);
						NumProrities += Prpos.queueCount;
						break;
					}

					else if(!PresentQueIndex.empty()){
						for (auto& i : PresentQueIndex) {
							vkGetPhysicalDeviceSurfaceSupportKHR(mData->mPhysicalDevice, i, viewport->getVkSurface(), &supportPresent);
							if (supportPresent) {
								PresentQueIndex.insert(i);
								ViewPortSwapChainCache[viewport] = new VulkanSwapChain(viewport, i);
								break;
							}
						}
					}
				
					if (!supportPresent) {
						
						if (mData->GQueFamilyIndex != -1)vkGetPhysicalDeviceSurfaceSupportKHR(mData->mPhysicalDevice, mData->GQueFamilyIndex, viewport->getVkSurface(), &supportPresent);
						if (supportPresent) {ViewPortSwapChainCache[viewport] = new VulkanSwapChain(viewport, mData->GQueFamilyIndex);break;}

						if (mData->CQueFamilyIndex != -1)vkGetPhysicalDeviceSurfaceSupportKHR(mData->mPhysicalDevice, mData->CQueFamilyIndex, viewport->getVkSurface(), &supportPresent);
						if (supportPresent) {ViewPortSwapChainCache[viewport] = new VulkanSwapChain(viewport, mData->CQueFamilyIndex);break;}


						if (mData->TQueFamilyIndex != -1)vkGetPhysicalDeviceSurfaceSupportKHR(mData->mPhysicalDevice, mData->TQueFamilyIndex, viewport->getVkSurface(), &supportPresent);
						if (supportPresent) {ViewPortSwapChainCache[viewport] = new VulkanSwapChain(viewport, mData->TQueFamilyIndex);break;}

					}

					Error("Can Not Find  a Present Que For Viewport: 0x{:x}", (size_t)viewport);
				}
		}


		if (mData->CQueFamilyIndex == -1) {
			Error("Error Device The Graphic Que haven't found");
			return false;
		}
		std::vector<float> QueuePriorities(NumProrities);
		float* CurrentQuePriority = QueuePriorities.data();
		
		
		for (auto& info : QueInfos) {
			info.pQueuePriorities = CurrentQuePriority;
			const auto& Props = mData->mQueueFamilyProperties[info.queueFamilyIndex];
			for (uint32_t i = 0; i < Props.queueCount; i++) *CurrentQuePriority++ = 1.0f;
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
			switch(res) {
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



}
