#include "VulkanQueue.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDebugNameSet.h"
#include "VulkanDevice.h"

namespace FISIR{

	static VkPipelineStageFlags getVkFlags(RHIUsingStageFlags stage) {
		if (stage == RHIUsingStage::ALLStage) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

		VkPipelineStageFlags res = 0;

		if (stage & RHIUsingStage::VertexShaderStage)
			res |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		if (stage & RHIUsingStage::FragmentShaderStage)
			res |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		if (stage & RHIUsingStage::TessShaderStage)
			res |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
		if (stage & RHIUsingStage::ComputeShaderStage)
			res |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		if (stage & RHIUsingStage::GeometryShaderStage)
			res |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
		if (stage & RHIUsingStage::PipelinTopStage)
			res |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		if (stage & RHIUsingStage::PipelineBottomStage)
			res |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		if (stage & RHIUsingStage::PipelineVertexInputStage)
			res |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		if (stage & RHIUsingStage::PipelineBeforeFragmentStage)
			res |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		if (stage & RHIUsingStage::PipelineAfterFragmentStage)
			res |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		if (stage & RHIUsingStage::PipelineTransferStage)
			res |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		if (stage & RHIUsingStage::ColorAttachmentOutputStage)
			res |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		return res;
	}

	static std::atomic_uint  QueIndexOfFamilyIndex[32] {0};

	struct __VkQueData {
		VkQueue mQue;
	};


	VulkanQueue::VulkanQueue(VulkanDevice* device, uint32_t FamilyIndex, const char* DebugName):
	mFamilyIndex(FamilyIndex), mDevice(device) {
		mData = new __VkQueData();
		mQueueIndex = QueIndexOfFamilyIndex[FamilyIndex]++;
		vkGetDeviceQueue(mDevice->getLogicalDevice(), mFamilyIndex, mQueueIndex, &mData->mQue);
		if (!mData->mQue) {Error("Can Not Create Que: {}", DebugName); return;}
#ifdef _DEBUG
		mDebugName = std::string(DebugName);
		Debug("{} Vk Que Create!", mDebugName);
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mData->mQue, VK_OBJECT_TYPE_QUEUE, (DebugName ? DebugName : "VulkanBuffer"));

#endif // DEBUG
	
	}

	VulkanQueue::~VulkanQueue() {
		QueIndexOfFamilyIndex[mFamilyIndex]--;
		delete mData;
	}

	void VulkanQueue::Submit(const std::vector<VkCommandBuffer_T*>& cmds, const std::vector<RHISemaphore*>& SignalSemaphores, const std::vector<RHISemaphore*>& WaitSemaphores, VkFence Fence) {
		if (mData->mQue == VK_NULL_HANDLE) {
			Error("Queue is VK_NULL_HANDLE!");
			return;
		}

		// 允许空命令缓冲：用于「仅 signal 回收围栏」的空提交（围栏解耦）。
		if (cmds.size() == 0 && Fence == nullptr) {
			Error("Command buffers are null!");
			return;
		}

		std::vector<VkSemaphore> semaphoresToWait, semaphoresToSignal;
		std::vector<uint64_t> ToWaitsValues, ToSignalValues;
		semaphoresToWait.reserve(SignalSemaphores.size());
		semaphoresToSignal.reserve(WaitSemaphores.size());
		std::vector<VkPipelineStageFlags> waitStages;
		waitStages.reserve(WaitSemaphores.size());

		bool hasTimeLine = 0;
		for (auto& semaphore : SignalSemaphores) {
			semaphoresToSignal.push_back(static_cast<VkSemaphore>(semaphore->getSemaphoreHandle()));
			hasTimeLine |= semaphore->getSemaphoreType() == FenceType::TimeLine;
			uint64_t signalVal = semaphore->getSemaphoreType() == FenceType::TimeLine ? static_cast<VulkanSemaphore*>(semaphore)->getNextSignalValue() : 1;
			ToSignalValues.push_back(signalVal);
		}
		for (auto& semaphore : WaitSemaphores) {
			semaphoresToWait.push_back(static_cast<VkSemaphore>(semaphore->getSemaphoreHandle()));
			hasTimeLine |= semaphore->getSemaphoreType() == FenceType::TimeLine;
			waitStages.push_back(getVkFlags(semaphore->getWaitingStage()));
			uint64_t signalVal = semaphore->getSemaphoreType() == FenceType::TimeLine ? static_cast<VulkanSemaphore*>(semaphore)->getCurrentValue() : 1;
			ToWaitsValues.push_back(signalVal);
		}


		VkTimelineSemaphoreSubmitInfo timelineinfo{
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.pNext = nullptr,
			.waitSemaphoreValueCount = static_cast<uint32_t>(ToWaitsValues.size()),
			.pWaitSemaphoreValues = ToWaitsValues.data(),
			.signalSemaphoreValueCount = static_cast<uint32_t>(ToSignalValues.size()),
			.pSignalSemaphoreValues = ToSignalValues.data(),
		};

		VkSubmitInfo info{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = hasTimeLine ? &timelineinfo : nullptr,
			.waitSemaphoreCount = (uint32_t)semaphoresToWait.size(),
			.pWaitSemaphores = (semaphoresToWait.empty()) ? nullptr : semaphoresToWait.data(),
			.pWaitDstStageMask = (waitStages.empty()) ? nullptr : waitStages.data(),
			.commandBufferCount = (uint32_t)cmds.size(),
			.pCommandBuffers = cmds.data(),
			.signalSemaphoreCount = (uint32_t)semaphoresToSignal.size(),
			.pSignalSemaphores = (semaphoresToSignal.empty()) ? nullptr : semaphoresToSignal.data(),
		};

		VkResult submitRes = vkQueueSubmit(mData->mQue, 1, &info, Fence ? Fence : VK_NULL_HANDLE);
		if (submitRes != VK_SUCCESS) {
			Error("Failed to submit command buffer to queue! VkResult={}, fence : 0x{:x}", (int)submitRes, (uint64_t)Fence);
			if (submitRes == VK_ERROR_DEVICE_LOST) {
				mDevice->markDeviceLost();
				Error("VK_ERROR_DEVICE_LOST: device lost, stopping RHI submission to prevent memory explosion.");
			}
			return;
		}

	}

	VkQueue_T* VulkanQueue::getQueueHandle() const {
		return mData->mQue;
	}

	
}
