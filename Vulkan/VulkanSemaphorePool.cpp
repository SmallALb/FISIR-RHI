#include "VulkanSemaphorePool.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDevice.h"


namespace FISIR {

	extern void setVkObjectName(VkDevice device, uint64_t objectHandle, VkObjectType objectType, const char* name);

	VulkanSemaphorePool::VulkanSemaphorePool(VulkanDevice* device, uint32_t initalSize) {
		mDevice = device;
		mBinarySemaphores.reserve(initalSize);
		mTimeLineSemaphores.reserve(initalSize);
	}
	
	VulkanSemaphorePool::~VulkanSemaphorePool() {
		std::lock_guard<std::mutex> lockB(mBinaryPoolMutex);
		std::lock_guard<std::mutex> lockT(mTimeLinePoolMutex);
		for (auto& semaphore : mBinarySemaphores) {
			delete semaphore;
		}

		for (auto& semaphore : mTimeLineSemaphores) {
			delete semaphore;
		}
	}
	
	VulkanSemaphore* VulkanSemaphorePool::createSemaphore(const char* name, FenceType typ) {

		auto& Heap = typ == FenceType::Binary ? mBinarySemaphores : mTimeLineSemaphores;
		auto& Que = typ == FenceType::Binary ? mFreeBinarySemaphores : mFreeTimeLineSemaphores;
		auto& mtx = typ == FenceType::Binary ? mBinaryPoolMutex : mTimeLinePoolMutex;

		VulkanSemaphore* semaphore = nullptr;

		if (Que.pop(semaphore)) {
			semaphore->reName(name);
			return semaphore;
		}

		std::lock_guard<std::mutex> lock(mtx);
		if (Que.pop(semaphore)) {
			semaphore->reName(name);
			return semaphore;
		}

		semaphore = new VulkanSemaphore(mDevice, name, typ);
		Heap.push_back(semaphore);

		return semaphore;
	}

	void VulkanSemaphorePool::release(VulkanSemaphore* semaphore) {
		auto& Que = semaphore->getSemaphoreType() == FenceType::Binary ? mFreeBinarySemaphores : mFreeTimeLineSemaphores;
		Que.push(semaphore);
	}

	
	VulkanSemaphore::VulkanSemaphore(VulkanDevice* device, const char* name, FenceType type) {
		mDevice = device;
		mSemaphoreType = type;
		VkSemaphoreTypeCreateInfo typeInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			.initialValue = 0,
		};

		VkSemaphoreCreateInfo info {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.flags = 0,
		};

		if (mSemaphoreType == FenceType::TimeLine) info.pNext = &typeInfo;

		if (vkCreateSemaphore(mDevice->getLogicalDevice(), &info, nullptr, &mSemaphore) != VK_SUCCESS) {
			Error("Failed to create Vulkan Semaphore!");
		}
#ifdef DEBUG
		reName(name);
#endif // DEBUG

	}
	VulkanSemaphore::~VulkanSemaphore() {
		if (mSemaphore) vkDestroySemaphore(mDevice->getLogicalDevice(), mSemaphore, nullptr);
	}
	void VulkanSemaphore::setWaitingStage(RHIUsingStageFlags stage) {
		mWaittingBit = stage;
	}

	RHIUsingStageFlags VulkanSemaphore::getWaitingStage() const {
		return mWaittingBit;
	}


	void* VulkanSemaphore::getSemaphoreHandle() const {
		return mSemaphore;
	}

	bool VulkanSemaphore::wait(uint64_t timeout) {
		if (mSemaphoreType != FenceType::TimeLine) {
			Error("You can't wait Binary Semaphore In Cpu");
			return false;
		}
		// 等待「尚未被消费过的最小信号值」。这里绝不能拿 nextSignalValue 做快路径短路：
		// 它在提交时（RHI 线程调用 getNextSignalValue）就自增，只代表信号「已被承诺」，
		// 不代表 GPU「已完成」——用它短路会让 CPU 在 GPU 尚未完成时提前返回，同步形同虚设。
		// vkWaitSemaphores 对已达到的值会立即返回 VK_SUCCESS，因此无需额外快路径。
		auto waitvalue = nextWaitValue.load(std::memory_order_acquire);

		VkSemaphoreWaitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.pNext = nullptr,
			.flags = 0,
			.semaphoreCount = 1,
			.pSemaphores = &mSemaphore,
			.pValues = &waitvalue
		};

		auto res = vkWaitSemaphores(mDevice->getLogicalDevice(), &waitInfo, timeout) == VK_SUCCESS;
		if (res) {
			// currentValue 记录「CPU 已确认到达的最大值」，VulkanQueue::Submit 用它作为
			// 时间线信号量在 GPU 侧的等待值（见该处注释）。
			// 注意：store 只能用 relaxed / release / seq_cst——acq_rel 是「读改写」专用序，
			// 用在 store 上会被 STL 的 _Check_store_memory_order 断言挡下（Debug 下直接 abort）。
			if (currentValue.load(std::memory_order_acquire) < waitvalue)
				currentValue.store(waitvalue, std::memory_order_release);

			uint64_t expect = waitvalue;
			nextWaitValue.compare_exchange_strong(expect, waitvalue+1, std::memory_order_acq_rel);
			return true;
		}

		return false;
	}

	FenceType VulkanSemaphore::getSemaphoreType() const {
		return mSemaphoreType;
	}

	void VulkanSemaphore::reName(const char* name) {
#ifdef _DEBUG
		mName = name;
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mSemaphore, VK_OBJECT_TYPE_SEMAPHORE, name ? name : "Unnamed Fence");

#endif
	}

	uint64_t VulkanSemaphore::getNextSignalValue() {
		return nextSignalValue.fetch_add(1, std::memory_order_acq_rel);
	}

	uint64_t VulkanSemaphore::getCurrentValue() {
		return currentValue.load(std::memory_order_acquire);
	}



}
