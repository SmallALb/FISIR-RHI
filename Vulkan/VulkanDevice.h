#pragma once
#include "VulkanQueue.h"
#include "../RHIResource.h"
#include "../RHITexture.h"
#include <vector>
#include <unordered_map>
#include <atomic>
	struct VkPhysicalDevice_T;
	struct VkDevice_T;
	struct VkPhysicalDeviceDescriptorHeapPropertiesEXT;
	struct VkSurfaceKHR_T;
	struct VkFence_T;
	using VkDeviceSize = uint64_t;
namespace FISIR {
	struct DescriptorSizes {
		// 对齐值
		VkDeviceSize resourceHeapAlignment = 0;
		VkDeviceSize samplerHeapAlignment = 0;
		VkDeviceSize bufferAlignment = 0;
		VkDeviceSize imageAlignment = 0;
		VkDeviceSize samplerAlignment = 0;

		// 三类描述符的字节大小
		uint32_t bufferDescriptorSize = 0;   // UBO, SSBO, TexelBuffer, Dynamic UBO/SSBO
		uint32_t imageDescriptorSize = 0;   // SampledImage, StorageImage, InputAttachment
		uint32_t samplerDescriptorSize = 0;   // Sampler, CombinedImageSampler 中的采样器部分

		// 堆的限制
		VkDeviceSize maxResourceHeapSize = 0;
		VkDeviceSize maxSamplerHeapSize = 0;
		VkDeviceSize minResourceReserved = 0;
		VkDeviceSize minSamplerReserved = 0;
		uint32_t     maxEmbeddedSamplers = 0;
		bool isInit = 0;
	};

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

		bool Init();

		// 运行期（Init 之后）新建视口用：在该 surface 支持呈现、且**设备创建时就已建好队列**的
		// 队列族里挑一个（图形 → 计算 → 传输）。Init 那套"每视口一个独占呈现队列"必须在
		// vkCreateDevice 时定下来，事后加不了队列，所以运行时只能复用既有队列。
		// 返回 -1 = 三个队列族都不支持该 surface 呈现。
		int32_t FindPresentQueueFamilyForSurface(VkSurfaceKHR_T* surface);

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

		// GPU 时间戳查询的时钟周期（纳秒/刻度），用于把 vkCmdWriteTimestamp 读回的
		// 刻度差换算为真实耗时。设备不支持时间戳时返回 0。
		float getTimestampPeriod() const;

		void setResourceAccess(RHIResource* resource, ResourceAccess access);
		
		void setTextureLayout(RHITexture* texture, TextureLayout layout);

		void waitIdle();

		ResourceAccess getResourceAccess(RHIResource* resource);

		TextureLayout getTextureLayout(RHITexture* texture);

		void submitCommandBuffer(const std::vector<VkCommandBuffer_T*>& cmds, CmdType poolType, const std::vector<RHISemaphore*>& SignalSemaphores, const std::vector<RHISemaphore*>& WaitSemaphores, VkFence_T* Fence = nullptr);

		// 设备丢失：vkQueueSubmit 返回 VK_ERROR_DEVICE_LOST 时由 VulkanQueue 置位，
		// RHI/资源/Prepare 线程据此停止提交与分配，避免在已丢失设备上无界分配命令缓冲。
		void markDeviceLost() { mDeviceLost.store(true, std::memory_order_release); }
		bool isDeviceLost() const { return mDeviceLost.load(std::memory_order_acquire); }

		bool isDescriptorHeapSupported() const;

		bool isSwapchainMaintenance1Supported() const;

		DescriptorSizes& getHeapSizeInfo();

		VkPhysicalDeviceDescriptorHeapPropertiesEXT& getDescriptorHeapProperties();
	private:
		void QueryDescriptorSizes();

		bool InitDevice();
	private:
		__VkDeviceData* mData;
		VulkanMemoryAllocator* mAllocator;
		VulkanQueue* mGraphicQue{nullptr};
		VulkanQueue* mComputeQue{nullptr};
		VulkanQueue* mTransferQueue{nullptr};
		std::atomic_bool mDeviceLost{false};
		DescriptorSizes HeapSizes;
	};


}