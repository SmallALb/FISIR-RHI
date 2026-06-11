#pragma once

#include "../LockFreeQue.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct VkSemaphore_T;

namespace FISIR{
	class VulkanDevice;

	class VulkanSemaphore {
	public:
		VulkanSemaphore(VulkanDevice* device, const char* name = nullptr);

		~VulkanSemaphore();

		VkSemaphore_T* getSemaphore() const;

#ifdef _DEBUG
		const char* getName() const { return mName; }

#endif
		void reName(const char* name);

	private:
		VulkanDevice* mDevice;
		VkSemaphore_T* mSemaphore;
#ifdef _DEBUG
	const char* mName;
#endif // _DEBUG

	};



	class VulkanSemaphorePool {
	public:
		VulkanSemaphorePool(VulkanDevice* device, uint32_t initalSize = 16);

		~VulkanSemaphorePool();

		VulkanSemaphore* createSemaphore(const char* name = nullptr);

		void release(VulkanSemaphore* semaphore);

		void reset();

		private:
			VulkanDevice* mDevice;
			std::vector<VulkanSemaphore*> mSemaphores;
			LockFreeQue<VulkanSemaphore*> mFreeSemaphores;
			std::mutex mPoolMutex;
	};
}
