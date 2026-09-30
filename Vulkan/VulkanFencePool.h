#pragma once
#include "../RHIFence.h"
#include "../LockFreeQue.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct VkFence_T;

namespace FISIR{
	class VulkanDevice;

	class VulkanFencePool {
	public:
		VulkanFencePool(VulkanDevice* device);

		~VulkanFencePool();

		VkFence_T* createFence(bool signaled = false, const char* name = "Unnamed Fence");
		

		void release(VkFence_T* fence);

		bool wait(VkFence_T* fence, uint64_t timeout);

		bool isSignaled(VkFence_T* fence);

		void destroyPool();
	private:
		VulkanDevice* mDevice;
		std::vector<VkFence_T**> mFences;
		LockFreeQue<VkFence_T*> mFreeFences;
		std::mutex mPoolMutex;
	};

	class VulkanFence : public RHIFence {
	public:

		VulkanFence(VulkanFencePool* pool, bool signaled = false, const char* name = "Unnamed Fence");

		virtual ~VulkanFence();

		virtual  void* getFenceHandle() const override;

		virtual Statue getFenceStage() const override;

		virtual void reset() override;

		void wait() ;

		bool waitFor(uint64_t timeout = UINT64_MAX) ;

		virtual bool isSubmited() override;

		virtual bool waitFenceSubmited(uint64_t timeout) override;

		void setSubmited();

		bool isSignaled() override;
		

#ifdef _DEBUG
	const char* getName() const {return mName;}

#endif
	void reName(const char* name);

	private:
		VulkanFencePool* fencePool;
		VkFence_T* mFence;
		std::atomic<Statue> fenceStatue;
		std::atomic<bool> isSubmitedTag{0};
		std::condition_variable CV;
		std::mutex SubmitMtx;
#ifdef _DEBUG
	const char* mName {nullptr};
#endif 

	};
}

