#pragma once
#include "VulkanQueue.h"
#include "../RHIResource.h"
#include "../RHITexture.h"
#include <vector>
#include <unordered_map>
	struct VkPhysicalDevice_T;
	struct VkDevice_T;
	struct VkPhysicalDeviceDescriptorHeapPropertiesEXT;
	struct VkSurfaceKHR_T;
namespace FISIR {
	class VulkanMemoryAllocator;
	class VulkanCommandPool;
	class VulkanImageViewManager;
	class VulkanImageView;
	class RHIViewport;
	class VulkanSwapChain;
	class VulkanViewport;
	struct __VkDeviceData;
	struct VulkanDeviceExtensions {
		uint64_t pack;
	};
	
	
	class VulkanDevice {
	public:
		VulkanDevice(VkPhysicalDevice_T* device);

		~VulkanDevice();

		bool Init(const std::vector<VulkanViewport*>& viewports, std::unordered_map<RHIViewport*, VulkanSwapChain*>& ViewPortSwapChainCache);

		void Destory();

		VkDevice_T* getLogicalDevice();

		VkPhysicalDevice_T* getPhysicalDevice();

		inline VulkanMemoryAllocator* getAllocator() {	return mAllocator; }

		VulkanQueue* getGraphicQueue() { return mGraphicQue; }

		VulkanQueue* getComputeQueue() { return mComputeQue; }

		VulkanQueue* getTransferQueue() { return mTransferQueue; }
		
		uint32_t getMaxDescriptorSetSamplers() const; 

		uint32_t getMaxDescriptorSetStorageImages() const;

		uint32_t getMaxDescriptorSetCombinedImageSamplers() const;

		uint32_t getMaxDescriptorSetUniformBuffers() const;

		void setResourceAccess(RHIResource* resource, ResourceAccess access);
		
		void setTextureLayout(RHITexture* texture, TextureLayout layout);

		void waitIdle();

		ResourceAccess getResourceAccess(RHIResource* resource);

		TextureLayout getTextureLayout(RHITexture* texture);

		VulkanImageView* getImageView(RHITexture* texture);
	
		void freeImageView(RHITexture* texture);

		void submitCommandBuffer(const std::vector<VkCommandBuffer_T*>& cmds, CommandPoolType poolType, const std::vector<RHISemaphore*>& SignalSemaphores, const std::vector<RHISemaphore*>& WaitSemaphores, RHIFence* Fence = nullptr);

		bool isDescriptorHeapSupported() const;


		VkPhysicalDeviceDescriptorHeapPropertiesEXT& getDescriptorHeapProperties();
	private:
		bool InitDevice(const std::vector<VulkanViewport*>& viewports, std::unordered_map<RHIViewport*, VulkanSwapChain*>& ViewPortSwapChainCache);
	private:
		__VkDeviceData* mData;
		VulkanMemoryAllocator* mAllocator;
		VulkanImageViewManager* mImageViewManager;
		VulkanQueue* mGraphicQue;
		VulkanQueue* mComputeQue;
		VulkanQueue* mTransferQueue;
	};


}