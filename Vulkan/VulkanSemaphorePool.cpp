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
		auto waitvalue = nextWaitValue.load(std::memory_order_acquire);

		if (nextSignalValue.load(std::memory_order_acquire) >= waitvalue) {
			uint64_t expect = waitvalue;
			nextWaitValue.compare_exchange_strong(expect, waitvalue + 1, std::memory_order_acq_rel);
			return true;
		}

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
			uint64_t current = getCurrentValue();
			if (current < waitvalue) currentValue.store(waitvalue, std::memory_order_acq_rel);
			
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
