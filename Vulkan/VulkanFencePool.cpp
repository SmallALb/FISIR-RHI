#include "VulkanFencePool.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDebugNameSet.h"
#include "VulkanDevice.h"

namespace FISIR{
	constexpr size_t FencePageSize = 8;

	VulkanFence::VulkanFence(VulkanFencePool* pool, bool signaled, const char* name) {
		
		
		mFence = pool->createFence(signaled, name);
		fencePool = pool;
		fenceStatue.store(signaled ? Statue::Signaled : Statue::UnSignaled);		
#ifdef _DEBUG
		reName(name);
#endif // _DEBUG

	}
	
	VulkanFence::~VulkanFence() {
		if (!isSubmited()) fencePool->release(mFence);
	}

	void* VulkanFence::getFenceHandle() const {
		return mFence;
	}

	RHIFence::Statue VulkanFence::getFenceStage() const {
		return fenceStatue.load();
	}

	void VulkanFence::reset() {

		if (fenceStatue.load(std::memory_order_acquire) != Statue::Signaled && !isSubmited()) return;
		if (mFence) {
			isSubmitedTag.store(0, std::memory_order_release);

#ifdef _DEBUG
			mFence = fencePool->createFence(false, mName);
#else
			mFence = fencePool->createFence(false);
#endif
			fenceStatue.store(Statue::UnSignaled, std::memory_order_release);
		}
	}
	
	void VulkanFence::wait() {
		if (fenceStatue.load(std::memory_order_acquire) == Statue::Signaled) return;
		waitFenceSubmited(UINT64_MAX);
		auto res = fencePool->wait(mFence, UINT64_MAX);
		if (res) fenceStatue.store(Statue::Signaled);
	}
	
	bool VulkanFence::isSignaled() {
		auto res = fencePool->isSignaled(mFence);
		if (res) fenceStatue.store(Statue::Signaled);
		return res;
	}

	bool VulkanFence::waitFor(uint64_t timeout) {
		auto start = std::chrono::steady_clock::now();
		auto resOfSubmit = waitFenceSubmited(timeout);
		if (!resOfSubmit) return false;
		
		if(timeout != UINT64_MAX) {
			auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
			if (duration >= timeout) return false;
			timeout -= duration;
		}

		auto res = fencePool->wait(mFence, timeout);
		if (res) fenceStatue.store(Statue::Signaled);
		return res;
	}

	bool VulkanFence::isSubmited() {
		return isSubmitedTag.load(std::memory_order_acquire);
	}

	bool VulkanFence::waitFenceSubmited(uint64_t timeout) {
		std::unique_lock<std::mutex> lock(SubmitMtx);
		bool Status = 0;
		if (timeout == UINT64_MAX) {
			CV.wait(lock, [this] {return isSubmitedTag.load(std::memory_order_acquire); });
			return true;
		}
		else Status = CV.wait_for(lock, std::chrono::nanoseconds(timeout), [this] {return isSubmitedTag.load(std::memory_order_acquire); });
		return Status;
		//isSubmitedTag.wait(0);
		//return 1;
	}

	void VulkanFence::setSubmited() {
		{
			std::lock_guard<std::mutex> lock(SubmitMtx);
			isSubmitedTag.store(1, std::memory_order_release);
		}
		CV.notify_all();
	}

	void VulkanFence::reName(const char* name) {
#ifdef _DEBUG
		//Debug("Renaming Fence from '{}' to '{}'", mName ? mName : "Unnamed Fence", name ? name : "Unnamed Fence");
		mName = name;

		//vkSetDebugUtilsObjectNameEXT(mDevice->getLogicalDevice(), &nameInfo);
#endif // _DEBUG
	}

	
	VulkanFencePool::VulkanFencePool(VulkanDevice* device) {
		mDevice = device;
		mFences.push_back(new VkFence[FencePageSize]);
		for (int i=0; i< FencePageSize; i++) {
			VkFenceCreateInfo fenceCreateInfo{
				.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				.pNext = nullptr,
				.flags = VK_FENCE_CREATE_SIGNALED_BIT
			};
			
			vkCreateFence(mDevice->getLogicalDevice(), &fenceCreateInfo, nullptr, &mFences[0][i]);
			setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mFences[0][i], VK_OBJECT_TYPE_FENCE, "Unnamed Fence");

			mFreeFences.push(mFences[0][i]);
		}
	}
	
	VulkanFencePool::~VulkanFencePool() {
	
	}

	VkFence_T* VulkanFencePool::createFence(bool signaled, const char* name) {

		VkFence fence = nullptr;
		
		if (mFreeFences.pop(fence)) {
			if (!signaled) vkResetFences(mDevice->getLogicalDevice(), 1, &fence);
			setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)fence, VK_OBJECT_TYPE_FENCE, name ? name : "Unnamed Fence");
			return fence;
		}

		std::lock_guard<std::mutex> lock(mPoolMutex);
		mFences.push_back(new VkFence[FencePageSize]);
		for (int i=0; i< FencePageSize; i++){
			VkFenceCreateInfo fenceCreateInfo{
					.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
					.pNext = nullptr,
					.flags = VK_FENCE_CREATE_SIGNALED_BIT
			};
			vkCreateFence(mDevice->getLogicalDevice(), &fenceCreateInfo, nullptr, &mFences.back()[i]);
			if (i != 0) mFreeFences.push(mFences.back()[i]);
		}

		fence = mFences.back()[0];
		if (!signaled) vkResetFences(mDevice->getLogicalDevice(), 1, &fence);
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)fence, VK_OBJECT_TYPE_FENCE, name ? name : "Unnamed Fence");

		return fence;
	}


	void VulkanFencePool::release(VkFence_T* fence) {
		mFreeFences.push(fence);
	}

	bool VulkanFencePool::wait(VkFence_T* fence, uint64_t timeout) {
		return vkWaitForFences(mDevice->getLogicalDevice(), 1, &fence, VK_TRUE, timeout) == VK_SUCCESS;
	}

	bool VulkanFencePool::isSignaled(VkFence_T* fence) {
		return vkGetFenceStatus(mDevice->getLogicalDevice(), fence) == VK_SUCCESS;
	}

	void VulkanFencePool::destroyPool() {
		std::lock_guard<std::mutex> lock(mPoolMutex);
		for (auto& fences : mFences) {
			if (vkGetFenceStatus(mDevice->getLogicalDevice(), *fences) != VK_SUCCESS) {
				Warn("Waitting fences to be Signal");
				auto res = vkWaitForFences(mDevice->getLogicalDevice(), FencePageSize, fences, VK_TRUE, 2*1e9);
				if (res != VK_SUCCESS) Warn("Fence did not signal in time before destruction!");
				for (int i=0; i<FencePageSize; i++) vkDestroyFence(mDevice->getLogicalDevice(), fences[i], nullptr);
			}
			delete[] fences;
		}
	}
}
