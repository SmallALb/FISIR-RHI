#pragma once

#include "../LockFreeQue.h"
#include <cstdint>
#include <mutex>
#include <vector>
#include "../RHISemaphore.h"
struct VkSemaphore_T;

namespace FISIR{
	class VulkanDevice;

	class VulkanSemaphore : public RHISemaphore {
	public:
		
		VulkanSemaphore(VulkanDevice* device, const char* name = nullptr, FenceType type = FenceType::Binary);

		~VulkanSemaphore();

		virtual void setWaitingStage(RHIUsingStageFlags stage) override;

		virtual RHIUsingStageFlags getWaitingStage() const override;

		virtual void* getSemaphoreHandle() const override;

		virtual bool wait(uint64_t timeout) override;

		virtual FenceType getSemaphoreType() const override;

		virtual void setExternalSignal(bool ext) override;
		virtual bool isExternalSignal() const override;


#ifdef _DEBUG
		const char* getName() const { return mName; }

#endif
		void reName(const char* name);

		uint64_t getNextSignalValue();

		uint64_t getCurrentValue();

		std::atomic_uint64_t nextSignalValue {1};
		std::atomic_uint64_t currentValue{0};
		std::atomic<uint64_t> nextWaitValue{ 1 };

	private:
		VulkanDevice* mDevice;
		VkSemaphore_T* mSemaphore;
		FenceType mSemaphoreType;
		RHIUsingStageFlags mWaittingBit { ALLStage };
		bool mExternalSignal{ false };

#ifdef _DEBUG
	const char* mName;
#endif // _DEBUG

	};



	class VulkanSemaphorePool {
	public:
		VulkanSemaphorePool(VulkanDevice* device, uint32_t initalSize = 16);

		~VulkanSemaphorePool();

		VulkanSemaphore* createSemaphore(const char* name = nullptr, FenceType typ = FenceType::Binary);

		void release(VulkanSemaphore* semaphore);


		private:
			VulkanDevice* mDevice;
			std::vector<VulkanSemaphore*> mBinarySemaphores;
			std::vector<VulkanSemaphore*> mTimeLineSemaphores;
			LockFreeQue<VulkanSemaphore*> mFreeBinarySemaphores;
			LockFreeQue<VulkanSemaphore*> mFreeTimeLineSemaphores;
			std::mutex mBinaryPoolMutex, mTimeLinePoolMutex;
	};
}
