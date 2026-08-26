#pragma once

#include <cstdint>
#include <string>
#include "VulkanCommandPool.h"
#include "VulkanSemaphorePool.h"
#include "VulkanFencePool.h"
struct VkSemaphore_T;
struct VkQueue_T;
struct VkFence_T;
namespace FISIR {
	
	class VulkanDevice;
	class VulkanSemaphore;
	struct __VkQueData;
	class VulkanQueue {
	public:

#ifdef _DEBUG
	std::string mDebugName;
#endif // DEBUG
		
		VulkanQueue(VulkanDevice* device, uint32_t FamilyIndex, const char* DebugName = nullptr);
	
		~VulkanQueue();

		void Submit(const std::vector<VkCommandBuffer_T*>& cmds, const std::vector<RHISemaphore*>& SignalSemaphores, const std::vector<RHISemaphore*>& WaitSemaphores, VkFence_T* Fence = nullptr);

		inline uint32_t getFamilyIndex() const {return mFamilyIndex;}

		inline uint32_t getQueueIndex() const {return mQueueIndex;}

		VkQueue_T* getQueueHandle() const;
	public:
		__VkQueData* mData;
	private:
		uint32_t mFamilyIndex;
		uint32_t mQueueIndex{0};
		VulkanDevice* mDevice;
	};

}