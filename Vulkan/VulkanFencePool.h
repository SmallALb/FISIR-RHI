#pragma once
#include "../RHIFence.h"
#include "../LockFreeQue.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct VkFence_T;

namespace FISIR{
	
	class VulkanDevice;

	class VulkanFence : public RHIFence {
	public:
		VulkanFence(VulkanDevice* device, bool signaled = false, const char* name = "Unnamed Fence");

		virtual ~VulkanFence();

		virtual  void* getFenceHandle() const override;

		virtual  void reset() override;

		virtual void wait() override;

		virtual  bool isSignaled() override;

		virtual  bool waitFor(uint64_t timeout = UINT64_MAX) override;

#ifdef _DEBUG
	const char* getName() const {return mName;}

#endif
	void reName(const char* name);

	private:
		VulkanDevice* mDevice;
		VkFence_T* mFence;
		std::mutex fenceMtx;

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

