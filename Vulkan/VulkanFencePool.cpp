#include "VulkanFencePool.h"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "../../Log/Logger.h"
#include "VulkanDebugNameSet.h"

namespace FISIR{
	VulkanFence::VulkanFence(VulkanDevice* device, bool signaled, const char* name) {
		VkFenceCreateInfo fenceCreateInfo{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.pNext = nullptr,
			.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : (VkFenceCreateFlags)0
		};

		if (vkCreateFence(device->getLogicalDevice(), &fenceCreateInfo, nullptr, &mFence) != VK_SUCCESS) {	
			Error("Failed to create Vulkan Fence!");
			mFence = nullptr;
		}
		mDevice = device;
#ifdef _DEBUG
		reName(name);
#endif // _DEBUG

	}
	
	VulkanFence::~VulkanFence() {
		if (mFence) vkDestroyFence(mDevice->getLogicalDevice(), mFence, nullptr);
	}

	void* VulkanFence::getFenceHandle() const {
		return mFence;
	}

	RHIFence::Statue VulkanFence::getFenceStage() const {
		return fenceStatue.load();
	}

	void VulkanFence::reset() {
		if (mFence) {
			vkResetFences(mDevice->getLogicalDevice(), 1, &mFence);
			fenceStatue.store(Statue::UnSignaled);
		}
	}
	
	void VulkanFence::wait() {
		fenceStatue.store(Statue::Pendding);
		auto res = (vkWaitForFences(mDevice->getLogicalDevice(), 1, &mFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
		if (res) fenceStatue.store(Statue::Signaled);
	}
	
	bool VulkanFence::isSignaled() {
		auto res = (vkWaitForFences(mDevice->getLogicalDevice(), 1, &mFence, VK_TRUE, 0) == VK_SUCCESS);
		if (res) fenceStatue.store(Statue::Signaled);
		return res;
	}

	bool VulkanFence::waitFor(uint64_t timeout) {
		fenceStatue.store(Statue::Pendding);
		auto res = (vkWaitForFences(mDevice->getLogicalDevice(), 1, &mFence, VK_TRUE, timeout) == VK_SUCCESS);
		if (res) fenceStatue.store(Statue::Signaled);
		return res;
	}

	void VulkanFence::reName(const char* name) {
#ifdef _DEBUG
		Debug("Renaming Fence from '{}' to '{}'", mName ? mName : "Unnamed Fence", name ? name : "Unnamed Fence");
		mName = name;
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mFence, VK_OBJECT_TYPE_FENCE, name ? name : "Unnamed Fence");

		//vkSetDebugUtilsObjectNameEXT(mDevice->getLogicalDevice(), &nameInfo);
#endif // _DEBUG
	}

	
	VulkanFencePool::VulkanFencePool(VulkanDevice* device, uint32_t initialSize) {
		mDevice = device;
		mFences.reserve(initialSize);
		for (int i=0; i<initialSize; i++) {
			VulkanFence* fence = new VulkanFence(device, true);
			mFences.push_back(fence);
			mFreeFences.push(fence);
		}
	}
	
	VulkanFencePool::~VulkanFencePool() {
		
	}

	VulkanFence* VulkanFencePool::createFence(bool signaled, const char* name) {
		std::lock_guard<std::mutex>	lock(mPoolMutex);

		VulkanFence* fence = nullptr;
		if (mFreeFences.empty()) {
			fence = new VulkanFence(mDevice, signaled, name);
			mFences.push_back(fence);
		}
		else {
			if (!mFreeFences.pop(fence)) {
				Error("Failed to pop a free fence from the pool!");
				return nullptr;
			}
			if (name != nullptr) fence->reName(name);
			if (!signaled) {
				fence->wait();
				fence->reset();

			}
		}

		return fence;
	}
	
	void VulkanFencePool::release(VulkanFence* fence) {
		mFreeFences.push(fence);
	}
	
	void VulkanFencePool::waitAndRelease(VulkanFence* fence, uint64_t timeout) {
		fence->waitFor(timeout);
		release(fence);
	}
	
	void VulkanFencePool::destroyPool() {
		std::lock_guard<std::mutex> lock(mPoolMutex);
		for (auto& fence : mFences) {

			if (!fence->isSignaled()) {
				Debug("Waiting for fence {} to be signaled before destruction...", fence->getName());
				bool signaled = fence->waitFor(2*1000000000);
				if (!signaled) {
					Error("Fence {} did not signal in time before destruction!", fence->getName());
				}
			}
			delete fence;
		}
	}
}
