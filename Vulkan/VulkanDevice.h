#pragma once
#include "VulkanQueue.h"
#include "../RHIResource.h"
#include "../RHITexture.h"

	struct VkPhysicalDevice_T;
	struct VkDevice_T;
	struct VkPhysicalDeviceDescriptorHeapPropertiesEXT;

namespace FISIR {
	class VulkanMemoryAllocator;
	class VulkanCommandPool;

	struct __VkDeviceData;
	struct VulkanDeviceExtensions {
		uint64_t pack;
	};
	
	
	class VulkanDevice {
	public:
		VulkanDevice(VkPhysicalDevice_T* device);

		~VulkanDevice();

		bool Init();

		void Destory();

		VkDevice_T* getLogicalDevice();

		VkPhysicalDevice_T* getPhysicalDevice();

		inline VulkanMemoryAllocator* getAllocator() {	return mAllocator; }

		VulkanQueue* getGraphicQueue() { return mGraphicQue; }

		VulkanQueue* getComputeQueue() { return mComputeQue; }

		VulkanQueue* getTransferQueue() { return mTransferQueue; }

		VulkanQueue* getPresentQueue() { return mPresentQue; }
		
		uint32_t getMaxDescriptorSetSamplers() const; 

		uint32_t getMaxDescriptorSetStorageImages() const;

		uint32_t getMaxDescriptorSetCombinedImageSamplers() const;

		uint32_t getMaxDescriptorSetUniformBuffers() const;

		void setResourceAccess(RHIResource* resource, ResourceAccess access);
		
		void setTextureLayout(RHITexture* texture, TextureLayout layout);

		void waitIdle();

		ResourceAccess getResourceAccess(RHIResource* resource);

		TextureLayout getTextureLayout(RHITexture* texture);

		void submitCommandBuffer(const std::vector<VkCommandBuffer_T*>& cmds, CommandPoolType poolType, std::initializer_list<VulkanSemaphore*> SignalSemaphores = {}, std::initializer_list<VulkanSemaphore*> WaitSemaphores = {}, VulkanFence* Fence = nullptr);

		bool isDescriptorHeapSupported() const;


		VkPhysicalDeviceDescriptorHeapPropertiesEXT& getDescriptorHeapProperties();
	private:
		bool InitDevice();
	private:
		__VkDeviceData* mData;
		VulkanMemoryAllocator* mAllocator;
		VulkanQueue* mGraphicQue;
		VulkanQueue* mComputeQue;
		VulkanQueue* mTransferQueue;
		VulkanQueue* mPresentQue;
	};


}