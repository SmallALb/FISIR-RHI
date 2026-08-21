#include "VulkanSemaphorePool.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDevice.h"


namespace FISIR {
	VulkanSemaphorePool::VulkanSemaphorePool(VulkanDevice* device, uint32_t initalSize) {
		mDevice = device;
		mSemaphores.reserve(initalSize);

	}
	
	VulkanSemaphorePool::~VulkanSemaphorePool() {
		std::lock_guard<std::mutex> lock(mPoolMutex);
		for (auto& semaphore : mSemaphores) {
			delete semaphore;
		}
	}
	
	VulkanSemaphore* VulkanSemaphorePool::createSemaphore(const char* name) {
		std::lock_guard<std::mutex> lock(mPoolMutex);
		VulkanSemaphore* semaphore = nullptr;
		if (mFreeSemaphores.empty()) {
			semaphore = new VulkanSemaphore(mDevice, name);
			mSemaphores.push_back(semaphore);
			return semaphore;
		}
		else {
			if (!mFreeSemaphores.pop(semaphore)) {
				Error("Failed to pop semaphore from free semaphores queue!");
				return nullptr;
			}
			semaphore->reName(name);
			return semaphore;
		}
		if (!semaphore) Error("Get A Null semaphore");
		return semaphore;
	}

	void VulkanSemaphorePool::release(VulkanSemaphore* semaphore) {
		mFreeSemaphores.push(semaphore);
	}

	void VulkanSemaphorePool::reset() {
		std::lock_guard<std::mutex> lock(mPoolMutex);
		for (auto& semaphore : mSemaphores) {
			mFreeSemaphores.push(semaphore);
		}
	}
	
	VulkanSemaphore::VulkanSemaphore(VulkanDevice* device, const char* name) {
		mDevice = device;
		VkSemaphoreCreateInfo info {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.flags = 0,
		};
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
	void* VulkanSemaphore::getSemaphoreHandle() const {
		return mSemaphore;
	}

	void VulkanSemaphore::wait() {
		VkSemaphoreWaitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.pNext = nullptr,
			.flags = 0,
			.semaphoreCount = 1,
			.pSemaphores = &mSemaphore,
			.pValues = nullptr
		};
		vkWaitSemaphores(mDevice->getLogicalDevice(), &waitInfo, UINT64_MAX);
	}

	void VulkanSemaphore::reName(const char* name) {
#ifdef _DEBUG
		mName = name;
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.pNext = nullptr,
			.objectType = VK_OBJECT_TYPE_SEMAPHORE,
			.objectHandle = (uint64_t)mSemaphore,
			.pObjectName = mName ? mName : "Unnamed Semaphore"
		};

		//vkSetDebugUtilsObjectNameEXT(mDevice->getLogicalDevice(), &nameInfo);
#endif
	}
}
