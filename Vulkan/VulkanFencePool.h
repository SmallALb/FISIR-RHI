#pragma once

#include "../LockFreeQue.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct VkFence_T;

namespace FISIR{
	
	class VulkanDevice;

	class VulkanFence {
	public:
		VulkanFence(VulkanDevice* device, bool signaled = false, const char* name = "Unnamed Fence");

		virtual ~VulkanFence();

		VkFence_T* getVkFence();

		void reset();

		void wait();

		bool isSignaled();

		bool waitFor(uint64_t timeout = UINT64_MAX);

#ifdef _DEBUG
	const char* getName() const {return mName;}

#endif
	void reName(const char* name);

	private:
		VulkanDevice* mDevice;
		VkFence_T* mFence;

#ifdef _DEBUG
	const char* mName {nullptr};
#endif 

	};

	class VulkanFencePool {
	public:
		VulkanFencePool(VulkanDevice* device, uint32_t initialSize = 8);

		~VulkanFencePool();

		VulkanFence* createFence(bool signaled = false, const char* name = "Unnamed Fence");
		void release(VulkanFence* fence);
		
		void waitAndRelease(VulkanFence* fence, uint64_t timeout = UINT64_MAX);

		void destroyPool();
	private:
		VulkanDevice* mDevice;
		std::vector<VulkanFence*> mFences;
		LockFreeQue<VulkanFence*> mFreeFences;
		std::mutex mPoolMutex;
	};
}

