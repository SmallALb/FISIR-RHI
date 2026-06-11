#include "VulkanQueue.h"
#include "VulkanDevice.h"
#include <vulkan/vulkan.h>
#include "../../Log/Logger.h"
#include "VulkanDebugNameSet.h"

namespace FISIR{

	static std::atomic_uint  QueIndexOfFamilyIndex[32] {0};

	struct __VkQueData {
		VkQueue mQue;
	};

	VulkanQueue::VulkanQueue(VulkanDevice* device, uint32_t FamilyIndex, const char* DebugName):
	mFamilyIndex(FamilyIndex), mDevice(device) {
		mData = new __VkQueData();
#ifdef _DEBUG
		mDebugName = std::string(DebugName);
		Debug("{} Vk Que Create!", mDebugName);
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mData->mQue, VK_OBJECT_TYPE_QUEUE, (DebugName ? DebugName : "VulkanBuffer"));

#endif // DEBUG
	
		mQueueIndex = QueIndexOfFamilyIndex[FamilyIndex]++;
		vkGetDeviceQueue(mDevice->getLogicalDevice(), mFamilyIndex, mQueueIndex, &mData->mQue);
	}

	VulkanQueue::~VulkanQueue() {
		delete mData;
	}

	void VulkanQueue::Submit(const std::vector<VkCommandBuffer_T*>& cmds, std::initializer_list<VulkanSemaphore*> SignalSemaphores, std::initializer_list<VulkanSemaphore*> WaitSemaphores, VulkanFence* Fence) {
		
		// 验证 queue 是否有效
		if (mData->mQue == VK_NULL_HANDLE) {
			Error("Queue is VK_NULL_HANDLE!");
			return;
		}

		// 验证 command buffer 是否有效
		if (cmds.size() == 0) {
			Error("Command buffers are null!");
			return;
		}

		std::vector<VkSemaphore> semaphoresToWait, semaphoresToSignal;
		semaphoresToWait.reserve(SignalSemaphores.size());
		semaphoresToSignal.reserve(WaitSemaphores.size());
		std::vector<VkPipelineStageFlags> waitStages;
		waitStages.reserve(WaitSemaphores.size());



		for (auto& semaphore : SignalSemaphores) 
			semaphoresToSignal.push_back(semaphore->getSemaphore());
		for (auto& semaphore : WaitSemaphores) {
			semaphoresToWait.push_back(semaphore->getSemaphore());
			waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}

		VkSubmitInfo info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = (uint32_t)semaphoresToWait.size(),
			.pWaitSemaphores = (semaphoresToWait.empty()) ? nullptr : semaphoresToWait.data(),
			.pWaitDstStageMask = (waitStages.empty()) ? nullptr : waitStages.data(),
			.commandBufferCount = (uint32_t)cmds.size(),
			.pCommandBuffers = cmds.data(),
			.signalSemaphoreCount = (uint32_t)semaphoresToSignal.size(),
			.pSignalSemaphores = (semaphoresToSignal.empty()) ? nullptr : semaphoresToSignal.data(),
		};

		if (vkQueueSubmit(mData->mQue, 1, &info, Fence ? Fence->getVkFence() : VK_NULL_HANDLE) != VK_SUCCESS) {
			Error("Failed to submit command buffer to queue!");
			return;
		}
		Debug("Command buffer submitted to queue successfully!");

	}

	
}
