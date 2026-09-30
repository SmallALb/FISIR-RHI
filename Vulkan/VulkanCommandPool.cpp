#include "VulkanCommandPool.h"

#include <cstdio>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDebugNameSet.h"
#include "VulkanDescriptorPool.h"
#include "VulkanDevice.h"
#include "VulkanFencePool.h"
#include "VulkanFrameBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"
#include "VulkanRenderPass.h"
#include "VulkanSwapChian.h"


namespace FISIR{
	constexpr uint32_t INITIAL_PRIMARY_COMMAND_BUFFER_COUNT = 4;
	constexpr uint32_t INITIAL_SECONDARY_COMMAND_BUFFER_COUNT = 8;

	static VkImageAspectFlags getVulkanAspectFlagsForUsing(TextureUseForFlags usefor) {
		return usefor & TextureUseForDepthStencilAttachment ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	}
	static VkImageLayout getVulkanImageLayout(TextureLayout layout) {
		switch (layout) {
		case TextureLayout::Undefined:
			return VK_IMAGE_LAYOUT_UNDEFINED;
		case TextureLayout::ColorAttachmentOptimal:
			return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case TextureLayout::DepthStencilAttachmentOptimal:
			return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		case TextureLayout::ShaderReadOnlyOptimal:
			return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case TextureLayout::TransferSrcOptimal:
			return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		case TextureLayout::TransferDstOptimal:
			return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		case TextureLayout::Storage:
			return VK_IMAGE_LAYOUT_GENERAL;
		default:
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	static VkImageUsageFlags getVulkanImageUsage(TextureUseForFlags	flags) {
		VkImageUsageFlags usage = 0;
		if (flags & TextureUseFor::TextureUseForColorAttachment) {
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
		if (flags & TextureUseFor::TextureUseForDepthStencilAttachment) {
			usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		if (flags & TextureUseFor::TextureUseForShaderReadOnly) {
			usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if (flags & TextureUseFor::TextureUseForTransferSrc) {
			usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if (flags & TextureUseFor::TextureUseForTransferDst) {
			usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		if (flags & TextureUseFor::TextureUseForStorage) {
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if (flags & TextureUseFor::TextureUseForInputAttachment) {
			usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
		}
		return usage;
	}

	static VkAccessFlags getVulkanAccessFlags(ResourceAccess access) {
		switch (access) {
		case ResourceAccess::Undefined:
			return VK_ACCESS_NONE;
		case ResourceAccess::ShaderReadOnly:
			return VK_ACCESS_SHADER_READ_BIT;
		case ResourceAccess::ShaderWriteOnly:
			return VK_ACCESS_SHADER_WRITE_BIT;
		case ResourceAccess::ShaderReadWrite:
			return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		case ResourceAccess::TransferSrc:
			return VK_ACCESS_TRANSFER_READ_BIT;
		case ResourceAccess::TransferDst:
			return VK_ACCESS_TRANSFER_WRITE_BIT;
		case ResourceAccess::ColorAttachmentWrite:
			return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		default:
			return VK_ACCESS_NONE;
		}

	}

	static VkPipelineStageFlags getVulkanStageFlags(RHIUsingStage stage) {
		switch (stage) {
		case RHIUsingStage::VertexShaderStage:
			return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		case RHIUsingStage::FragmentShaderStage:
			return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		case RHIUsingStage::ComputeShaderStage:
			return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		case RHIUsingStage::PipelineTransferStage:
			return VK_PIPELINE_STAGE_TRANSFER_BIT;
		case RHIUsingStage::ColorAttachmentOutputStage:
			return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		case RHIUsingStage::NoneStage:
			return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		default:
			Warn("Unknown RHIUsingStage: %d, falling back to NONE", (int)stage);
			return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		}
	}

	static uint32_t getQueFamilyIndex(VulkanDevice* device, CmdType poolType) {
		VulkanQueue* queue = nullptr;

		switch (poolType) {
		case CmdType::Render:
			queue= device->getGraphicQueue(); break;
		case CmdType::Transfer:
			queue= device->getTransferQueue(); break;
		case CmdType::Compute:
			queue = device->getComputeQueue(); break;
		}
		if (!queue) return 0;
		return queue->getFamilyIndex();
	}

#ifdef _DEBUG
	static const char* getCmdTypeName(CmdType type) {
		switch (type) {
		case CmdType::Render:   return "Render";
		case CmdType::Compute:  return "Compute";
		case CmdType::Transfer: return "Transfer";
		default:                return "None";
		}
	}

	static void setCommandBufferName(VulkanDevice* device, VkCommandBuffer cb, CmdType poolType, CommandBufferType level, size_t index) {
		char name[64];
		snprintf(name, sizeof(name), "CmdBuffer_%s_%s_%zu",
			getCmdTypeName(poolType),
			level == _Primary_ ? "Primary" : "Secondary",
			index);
		setVkObjectName(device->getLogicalDevice(), (uint64_t)cb, VK_OBJECT_TYPE_COMMAND_BUFFER, name);
	}
#endif // _DEBUG

	struct __VKCommandPoolData {
		VkCommandPool mPool;
		std::vector<VkCommandBuffer*> PrimaryCommandBufferPool;
		std::vector<VkCommandBuffer*> SecondaryCommandBufferPool;
		LockFreeQue<size_t> FreePrimaryCommandBuffers;
		LockFreeQue<size_t> FreeSecondaryCommandBuffers;
	};

	

	VulkanCommandPool::VulkanCommandPool(VulkanDevice* device, CmdType mType, uint32_t FamilyIndex) {
		mDevice = device;
		mPoolType = mType;
		mData = new __VKCommandPoolData();
		
		VkCommandPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | (mType == CmdType::Transfer ? VK_COMMAND_POOL_CREATE_TRANSIENT_BIT : (VkCommandPoolCreateFlags)0),
			.queueFamilyIndex = FamilyIndex == UINT32_MAX ? getQueFamilyIndex(mDevice, mType) : FamilyIndex,
		};
		vkCreateCommandPool(mDevice->getLogicalDevice(), &poolInfo, nullptr, &mData->mPool);

		VkCommandBufferAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = mData->mPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = INITIAL_PRIMARY_COMMAND_BUFFER_COUNT
		};
		mData->PrimaryCommandBufferPool.push_back(new VkCommandBuffer[INITIAL_PRIMARY_COMMAND_BUFFER_COUNT]);
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->PrimaryCommandBufferPool[0]);
#ifdef _DEBUG
		for (uint32_t i = 0; i < INITIAL_PRIMARY_COMMAND_BUFFER_COUNT; ++i) {
			setCommandBufferName(mDevice, mData->PrimaryCommandBufferPool[0][i], mType, _Primary_, i);
		}
#endif
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		allocInfo.commandBufferCount = INITIAL_SECONDARY_COMMAND_BUFFER_COUNT;
		mData->SecondaryCommandBufferPool.push_back(new VkCommandBuffer[INITIAL_SECONDARY_COMMAND_BUFFER_COUNT]);
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->SecondaryCommandBufferPool[0]);
#ifdef _DEBUG
		for (uint32_t i = 0; i < INITIAL_SECONDARY_COMMAND_BUFFER_COUNT; ++i) {
			setCommandBufferName(mDevice, mData->SecondaryCommandBufferPool[0][i], mType, _Secondary_, i);
		}
#endif
		for (size_t i = 0; i < mData->PrimaryCommandBufferPool.size(); ++i) {
			mData->FreePrimaryCommandBuffers.push(i);
		}

		for (size_t i = 0; i < mData->SecondaryCommandBufferPool.size(); ++i) {
			mData->FreeSecondaryCommandBuffers.push(i);
		}
	}
	
	VulkanCommandPool::~VulkanCommandPool() {
		if (mData) {
			for (auto& page : mData->PrimaryCommandBufferPool) {
				vkFreeCommandBuffers(mDevice->getLogicalDevice(), mData->mPool, INITIAL_PRIMARY_COMMAND_BUFFER_COUNT, page);
				delete[] page;
			}
			for (auto& page : mData->SecondaryCommandBufferPool) {
				vkFreeCommandBuffers(mDevice->getLogicalDevice(), mData->mPool, INITIAL_SECONDARY_COMMAND_BUFFER_COUNT, page);
				delete[] page;
			}
			vkDestroyCommandPool(mDevice->getLogicalDevice(), mData->mPool, nullptr);
			delete mData;
			mData = nullptr;
		}
	}

	CBInfo VulkanCommandPool::createCommandBuffer(CommandBufferType cbType) {
		auto& Que = (cbType == _Primary_) ? mData->FreePrimaryCommandBuffers : mData->FreeSecondaryCommandBuffers;
		auto& Pool = (cbType == _Primary_) ? mData->PrimaryCommandBufferPool : mData->SecondaryCommandBufferPool;
		auto Count = (cbType == _Primary_) ? INITIAL_PRIMARY_COMMAND_BUFFER_COUNT : INITIAL_SECONDARY_COMMAND_BUFFER_COUNT;
		size_t index = SIZE_MAX;


		if (Que.pop(index)) {
			vkResetCommandBuffer(Pool[index / Count][index % Count], 0);
			return CBInfo(Pool[index / Count][index % Count], cbType, mPoolType, this, index);
		}

		std::lock_guard<std::mutex> lock(mCommandBufferMutex);
		if (Que.pop(index)) {
			vkResetCommandBuffer(Pool[index / Count][index % Count], 0);
			return CBInfo(Pool[index / Count][index % Count], cbType, mPoolType, this, index);
		}
		
		VkCommandBufferAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = mData->mPool,
			.level = (cbType == _Primary_) ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY,
			.commandBufferCount = Count
		};
		VkCommandBuffer* newCB = new VkCommandBuffer[Count];
		if (vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, newCB) != VK_SUCCESS) {
			Error("Failed to allocate command buffer!");
			return {};
		}
		
		Pool.push_back(newCB);
		size_t NewPageBegin = Count * (Pool.size() - 1);
		for (int i= NewPageBegin+1; i< NewPageBegin+Count; i++) Que.push(i);
#ifdef _DEBUG
		for (uint32_t i = 0; i < Count; ++i) {
			setCommandBufferName(mDevice, newCB[i], mPoolType, cbType, NewPageBegin + i);
		}
#endif // _DEBUG

		index = NewPageBegin;
		return CBInfo(Pool[index / Count][index % Count], cbType, mPoolType, this , index);
	}

	void VulkanCommandPool::releaseCommandBuffer(const CBInfo& cbInfo) {
		//std::lock_guard<std::mutex> lock(mCommandBufferMutex);
		if (cbInfo.pool != this) {
			Error("Attempting to release a command buffer that does not belong to this pool!");
			return;
		}
		(cbInfo.type == _Primary_) ? 
			mData->FreePrimaryCommandBuffers.push(cbInfo.index) : 
			mData->FreeSecondaryCommandBuffers.push(cbInfo.index);
	}


	std::vector<VulkanCommandPool*>& VulkanCommandPoolManager::getPool(CmdType type) {
		switch (type) {
		case CmdType::Render:
			return RenderCommandPools;
		case CmdType::Compute:
			return ComputeCommandPools;
		case CmdType::Transfer:
			return TransferCommandPools;
		}
	}

	LockFreeQue<VulkanCommandPool*>& VulkanCommandPoolManager::getQue(CmdType type) {
		switch (type) {
		case CmdType::Render:
			return FreeRenderCommandPools;
		case CmdType::Compute:
			return FreeComputeCommandPools;
		case CmdType::Transfer:
			return FreeTransferCommandPools;
		}
	}

	std::mutex& VulkanCommandPoolManager::getMutex(CmdType type) {
		switch (type) {
		case CmdType::Render:
			return RenderCommandPoolsMTX;
		case CmdType::Compute:
			return ComputeCommandPoolsMTX;
		case CmdType::Transfer:
			return TransferCommandPoolsMTX;
		}
	}

	VulkanCommandPoolManager::VulkanCommandPoolManager(VulkanDevice* device) : mDevice(device) {
		RenderCommandPools.resize(10, nullptr);
		ComputeCommandPools.resize(10, nullptr);
		TransferCommandPools.resize(10, nullptr);

		for (auto& pool : RenderCommandPools) {
			pool = new VulkanCommandPool(mDevice, CmdType::Render);
			FreeRenderCommandPools.push(pool);
		}

		for (auto& pool : ComputeCommandPools) {
			pool = new VulkanCommandPool(mDevice, CmdType::Compute);
			FreeComputeCommandPools.push(pool);
		}

		for (auto& pool : TransferCommandPools) {
			pool = new VulkanCommandPool(mDevice, CmdType::Transfer);
			FreeTransferCommandPools.push(pool);
		}
	}

	VulkanCommandPoolManager::~VulkanCommandPoolManager() {
		
		for (auto& pool : RenderCommandPools) {
			delete pool;
		}

		for (auto& pool : ComputeCommandPools) {
			delete pool;
		}

		for (auto& pool : TransferCommandPools) {
			delete pool;
		}

		std::lock_guard<std::mutex> lock(MapMutex);
		for (auto& [familyindex, pool] : FamilyIndexToPool) delete pool;	

	}

	VulkanCommandPool* VulkanCommandPoolManager::getCommandPool(CmdType type) {
		auto& Pool = getPool(type);
		auto& Que = getQue(type);
		auto& Mtx = getMutex(type);

		VulkanCommandPool* res = nullptr;

		if (Que.pop(res)) {
			return res;
		}

		std::lock_guard<std::mutex> lock(Mtx);

		if (Que.pop(res)) {
			return res;
		}

		res = new VulkanCommandPool(mDevice, type);
		Pool.push_back(res);

		return res;
	}



	void VulkanCommandPoolManager::reBackCommandPool(VulkanCommandPool* pool) {
		if (!pool) return;
		auto& Que = getQue(pool->mPoolType);
		Que.push(pool);
	}

	CommandExecuteThreadPool::CommandExecuteThreadPool(VulkanDevice* device, VulkanCommandPoolManager* VkCmdPoolManager) : usingManager(VkCmdPoolManager), mDevice(device) {
		Threads.resize(5);
		for (size_t i=0; i<Threads.size(); ++i) {
			Threads[i] = std::thread(&CommandExecuteThreadPool::ThreadLoop, this, static_cast<int>(i));
		}
	}

	CommandExecuteThreadPool::~CommandExecuteThreadPool() {
		StopTag = 1;
		Debug("Thread Pool Stop");
		NeedExecutePages.stopQue();
		for (auto& t : Threads) t.join();
	}

	void CommandExecuteThreadPool::pushCommandBatch(RingCommandPool::Page::BatchInfo batch, ExecuteResultData* result, std::atomic_uint32_t* finishCount) {
		if (batch.page == nullptr || batch.ReadBegin >= batch.ReadEnd) {
			Error("pushCommandBatch invalid batch: page={}, [{},{})",
				(void*)batch.page, batch.ReadBegin, batch.ReadEnd);
			__debugbreak();
			return;   // 或断言
		}
		NeedExecutePages.push({ batch, result, finishCount });
	}

	void CommandExecuteThreadPool::ThreadLoop(uint32_t ThreadID) {
		while(!StopTag.load()) {
			ExecutedPageTask exeTask;

			if (!NeedExecutePages.pop_wait(exeTask) || StopTag.load()) {
				continue;
			}

			if ((size_t)exeTask.Batch.page == 0xDDDDDDDDDDDDDDDD) {
				//WTF R U GET ?????
				__debugbreak();
			}

			auto& [batchInfo, result, finishCount] = exeTask;

			RHICommandT currentCmd = batchInfo.getCommandType();
			// 绑定名顺序必须与 ExecuteResultData 的成员声明顺序严格一致。
			auto& [framebuffer, clearval, renderPassEndTag, commandsEndTag, subpassIndex, cmdInfo, fence, waits, signals, presents, inheritFrameBuffer, inheritSubpass] = *result;
			auto cmdPool = usingManager->getCommandPool(batchInfo.page->Pool->cmdType);
			cmdInfo = cmdPool->createCommandBuffer(CommandBufferType::_Secondary_);

			VkCommandBufferInheritanceInfo inheritanceInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
			};
			bool isRenderPassContinuation = false;
			RHIPipeline* currentPipeline = nullptr;   // 当前绑定的管线（PushConstant 取它的布局）
			RHIResourcePackResult boundPack{};        // 当前绑定的资源包（降级路径按需绑定用）
			bool hasBoundPack = false;
			bool packBound = false;                   // 本命令缓冲里是否已用当前管线绑过 set
			RHIPipeline* lastBindPipeline = nullptr;  // 上次绑 set 用的管线
			bool pipelineValid = false;               // 当前管线句柄是否有效（空句柄绝不交给驱动）
			if (inheritFrameBuffer) {
				// 拆分后的续接段：继承信息由 RHI 线程预置，本段不以 BeginRenderPass 开头。
				inheritanceInfo.renderPass = static_cast<VkRenderPass>(inheritFrameBuffer->getFrameRenderPass()->getRenderPassHandle());
				inheritanceInfo.framebuffer = static_cast<VkFramebuffer>(inheritFrameBuffer->getResourceAPIHandle());
				inheritanceInfo.subpass = inheritSubpass;
				isRenderPassContinuation = true;
			} else if (currentCmd == RHICommandT::BeginRenderPass) {
				BeginRenderPass_CmdInfo info{};
				if (batchInfo.getBatchData(info)) {
					framebuffer = static_cast<VulkanFrameBuffer*>(info.frame);
					if ((size_t)framebuffer >= 0x10000) {
						inheritanceInfo.renderPass = static_cast<VkRenderPass>(framebuffer->getFrameRenderPass()->getRenderPassHandle());
						inheritanceInfo.framebuffer = static_cast<VkFramebuffer>(info.frame->getResourceAPIHandle());
						inheritanceInfo.subpass = info.subpassIndex;
						subpassIndex = info.subpassIndex;
						clearval = info.clearValue;
						isRenderPassContinuation = true;
					} else {
						Error("Thread {}: Invalid frameBuffer ptr=0x{:x} (currentCmd={}, ReadBegin={}, ReadEnd={}, page=0x{:x})",
							ThreadID, (size_t)framebuffer, (int)currentCmd, batchInfo.ReadBegin, batchInfo.ReadEnd, (size_t)batchInfo.page);
						framebuffer = nullptr;
					}
				} else {
					Error("Thread {}: getBatchData failed for BeginRenderPass (ReadBegin={}, ReadEnd={})",
						ThreadID, batchInfo.ReadBegin, batchInfo.ReadEnd);
					framebuffer = nullptr;
				}
			}

			VkCommandBufferBeginInfo BeginInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | (isRenderPassContinuation ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : (VkCommandBufferUsageFlags)0),
				.pInheritanceInfo = &inheritanceInfo
			};
			vkBeginCommandBuffer(cmdInfo.buffer, &BeginInfo);
			while(batchInfo.ReadBegin < batchInfo.ReadEnd) {

				// ── 降级路径：把 descriptor set 的绑定推迟到真正要画之前 ──────────────
				// 经典 DescriptorSet 路径下，vkCmdBindDescriptorSets 传的 pipelineLayout 必须与
				// 「即将用这个 set 绘制的那条管线」的布局兼容，而 push constant 范围也参与这个判定。
				// 于是两条要求同时成立才行：
				//   · 用当前管线的布局（不能用资源包自建的 —— 它没有 push constant 范围，实测
				//     HZBBuild 会报 "set 0 is not compatible with the pipeline layout bound"）；
				//   · 又必须是「将要用它」的那条管线（录制顺序可能是「先绑包、后绑管线」，
				//     Nanite 的 HZB 建塔就是这样；而像 ImGui 之后紧跟呈现包的情况，绑定时刻的
				//     currentPipeline 还停留在上一条管线上）。
				// 推迟到 draw/dispatch 之前取 currentPipeline，就与录制顺序无关了。
				// 描述符堆路径不需要这一层（堆与管线布局无绑定关系）。
				if (!mDevice->isDescriptorHeapSupported() && hasBoundPack && currentPipeline &&
				    (!packBound || lastBindPipeline != currentPipeline)) {
					const bool needDescriptor =
						currentCmd == RHICommandT::DrawPrimitive ||
						currentCmd == RHICommandT::DrawIndex ||
						currentCmd == RHICommandT::DrawIndirect ||
						currentCmd == RHICommandT::DrawIndexedIndirect ||
						currentCmd == RHICommandT::Dispatch;
					if (needDescriptor) {
						VkPipelineBindPoint bindPoint = (batchInfo.page->Pool->cmdType == CmdType::Compute)
							? VK_PIPELINE_BIND_POINT_COMPUTE
							: VK_PIPELINE_BIND_POINT_GRAPHICS;
						CmdBindResourcePack(mDevice, cmdInfo.buffer, currentPipeline,
							boundPack.ResourcePack, boundPack.SamplerPack, (uint32_t)bindPoint);
						packBound = true;
						lastBindPipeline = currentPipeline;
					}
				}

				// 管线没建出来时（vkCreate*Pipelines 失败）draw/dispatch 一律跳过：
				// 给驱动喂缺失的管线状态，轻则验证层报错、重则驱动段错误（手机上是后者 ——
				// 现场只会看到一个 vkCmdBindPipeline 的栈，看不出「管线没建出来」这个真因）。
				if (!pipelineValid &&
					(currentCmd == RHICommandT::DrawPrimitive || currentCmd == RHICommandT::DrawIndex ||
					 currentCmd == RHICommandT::DrawIndirect || currentCmd == RHICommandT::DrawIndexedIndirect ||
					 currentCmd == RHICommandT::Dispatch)) {
					batchInfo.skipCommand();   // 必须跳过负载：否则读游标不前进（死循环）
					currentCmd = batchInfo.getCommandType();
					continue;
				}

				switch (currentCmd) {
					case RHICommandT::BeginRenderPass: {
						break;
					}
					case RHICommandT::EndRenderPass: {
						ReserveInput_CmdInfo info;
						batchInfo.getBatchData(info);
						renderPassEndTag = true;
						break;
					}
					case RHICommandT::DrawPrimitive: {
						DrawPrimitive_CmdInfo info;
						batchInfo.getBatchData(info);
						vkCmdDraw(cmdInfo.buffer, info.NumsPrimitives, info.NumInstances, info.BaseVertexIndex, 0);
						break;
					}
					case RHICommandT::DrawIndex: {
						DrawIndex_CmdInfo info;
						batchInfo.getBatchData(info);
						vkCmdDrawIndexed(cmdInfo.buffer, info.IndexCount, info.InstanceCount, info.BaseVertexIndex, 0, info.BaseInstanceIndex);
						break;
					}
					case RHICommandT::DrawIndirect: {
						DrawIndirect_CmdInfo info;
						batchInfo.getBatchData(info);
						vkCmdDrawIndirect(cmdInfo.buffer, static_cast<VkBuffer>(info.Buffer->getResourceAPIHandle()), info.Offset, info.DrawCount, info.Stride);
						break;
					}
					case RHICommandT::DrawIndexedIndirect: {
						DrawIndexedIndirect_CmdInfo info;
						batchInfo.getBatchData(info);
						vkCmdDrawIndexedIndirect(cmdInfo.buffer, static_cast<VkBuffer>(info.Buffer->getResourceAPIHandle()), info.Offset, info.DrawCount, info.Stride);
						break;
					}

					case RHICommandT::BindPipeline: {
						BindPipeline_CmdInfo info;
						batchInfo.getBatchData(info);
						VkPipelineBindPoint bindPoint = info.pipeline->isComputePipeline()
							? VK_PIPELINE_BIND_POINT_COMPUTE
							: VK_PIPELINE_BIND_POINT_GRAPHICS;
						const VkPipeline vkPipeline = static_cast<VkPipeline>(info.pipeline->getPipelineHandle());
						if (vkPipeline == VK_NULL_HANDLE) {
							// 管线没建出来（创建失败）：绝不能把它交给驱动去绑 —— 那是 vkCmdBindPipeline
							// 里的空指针解引用（手机上表现为驱动段错误，栈里只有 vkCmdBindPipeline）。
							// 这里降级成「本帧这批命令不画」并报错，让真正的失败原因（见管线创建处的日志）露出来。
							Error("[Vulkan] 绑定了空管线句柄（管线创建失败？），跳过该命令批次");
							pipelineValid = false;
							break;
						}
						vkCmdBindPipeline(cmdInfo.buffer, bindPoint, vkPipeline);
						pipelineValid = true;
						currentPipeline = info.pipeline;   // PushConstant 要用它的管线布局
						break;
					}
					case RHICommandT::PushConstant: {
						PushConstant_CmdInfo info;
						batchInfo.getBatchData(info);
						CmdPushConstant(mDevice, cmdInfo.buffer, currentPipeline,
							info.offset, info.size, info.data, info.usingStage);
						break;
					}
					case RHICommandT::BindVertexBuffer: {
						BindVertextBuffer_CmdInfo info;
						batchInfo.getBatchData(info);
						VkBuffer buffer = static_cast<VkBuffer>(info.buffer->getResourceAPIHandle());
						vkCmdBindVertexBuffers(cmdInfo.buffer, info.binding, 1, &buffer, &info.offset);
						break;
					}
					case RHICommandT::BindIndexBuffer: {
						BindIndexBuffer_CmdInfo info;
						batchInfo.getBatchData(info);
						VkBuffer buffer = static_cast<VkBuffer>(info.buffer->getResourceAPIHandle());
						vkCmdBindIndexBuffer(cmdInfo.buffer, buffer, info.offset, VK_INDEX_TYPE_UINT32);
						break;
					}
					case RHICommandT::BindViewPort: {
						BindViewPort_CmdInfo info;
						batchInfo.getBatchData(info);
						VkViewport viewport{
							info.x, info.y, info.width, info.height, info.minDepth, info.maxDepth
						};
						vkCmdSetViewport(cmdInfo.buffer, 0, 1, &viewport);
						break;
					}
					case RHICommandT::BindScissor: {
						BindScissor_CmdInfo info;
						batchInfo.getBatchData(info);
						VkRect2D scissor{
							{info.x, info.y}, {info.width, info.height}
						};
						vkCmdSetScissor(cmdInfo.buffer, 0, 1, &scissor);
						break;
					}
					case RHICommandT::BindResourceAndSamplerPack: {
						BindResourcePack_CmdInfo info;
						batchInfo.getBatchData(info);
						boundPack = info.Pack;
						hasBoundPack = true;
						if (mDevice->isDescriptorHeapSupported()) {
							// 描述符堆路径：与管线布局无关，立即绑定。
							VkPipelineBindPoint bindPoint = (batchInfo.page->Pool->cmdType == CmdType::Compute)
								? VK_PIPELINE_BIND_POINT_COMPUTE
								: VK_PIPELINE_BIND_POINT_GRAPHICS;
							CmdBindResourcePack(mDevice, cmdInfo.buffer, currentPipeline, info.Pack.ResourcePack, info.Pack.SamplerPack, (uint32_t)bindPoint);
						}
						else {
							// 降级路径：真正的 vkCmdBindDescriptorSets 推迟到 draw/dispatch 前（见循环开头那段）
							packBound = false;
						}
						break;
					}
					case RHICommandT::TransferTexture: {
						TextureTransition_CmdInfo info;
						batchInfo.getBatchData(info);

						uint32_t curFamily = getQueFamilyIndex(mDevice, batchInfo.page->Pool->cmdType);
						uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED;
						uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED;
						VkAccessFlags srcAccess = getVulkanAccessFlags(info.waitForAccessDone);
						VkAccessFlags dstAccess = getVulkanAccessFlags(info.beginAccessWhenDone);
						VkPipelineStageFlags srcStage = getVulkanStageFlags(info.waitForStageDone);
						VkPipelineStageFlags dstStage = getVulkanStageFlags(info.beginStageWhenDone);

						if (info.ResourceQueue != CmdType::None) {
							if (info.ResourceIsTransferOut) {
								// release（转出）：从当前队列移交资源到 ResourceQueue 队列。
								// dst 侧工作尚未发生，release 的 dstAccess 必须为 0。
								srcFamily = curFamily;
								dstFamily = getQueFamilyIndex(mDevice, info.ResourceQueue);
								dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
								dstAccess = 0;
							} else {
								// acquire（转入）：从 ResourceQueue 队列接管资源到当前队列。
								// src 侧工作在另一队列完成，本队列（目标队列）仅能见到 TOP_OF_PIPE，
								// 且 acquire 的 srcAccess 必须为 0。
								srcFamily = getQueFamilyIndex(mDevice, info.ResourceQueue);
								dstFamily = curFamily;
								srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
								srcAccess = 0;
							}
						}

						std::vector<VkImageMemoryBarrier> barriers(info.count);
						for (size_t i = 0; i < info.count; ++i) {
							// oldLayout 优先级：调用者显式传入 > 本批内最新 transition > 纹理追踪值。
							// 显式传入优先，因为 renderpass 的 finalLayout 不回写纹理追踪值，
							// 离屏渲染目标的追踪值可能滞后于实际布局。
							TextureLayout oldLayout = info.oldLayout;
							if (oldLayout == TextureLayout::Undefined) {
								auto it = cmdInfo.QuoteResources.find(info.texture[i]);
								if (it != cmdInfo.QuoteResources.end()) oldLayout = it->second.layout;
								else oldLayout = info.texture[i]->getCurrentLayout();
							}

							barriers[i] = {
								.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
								.srcAccessMask = srcAccess,
								.dstAccessMask = dstAccess,
								.oldLayout = getVulkanImageLayout(oldLayout),
								.newLayout = getVulkanImageLayout(info.newLayout),
								.srcQueueFamilyIndex = srcFamily,
								.dstQueueFamilyIndex = dstFamily,
								.image = static_cast<VkImage>(info.texture[i]->getResourceAPIHandle()),
								// 子资源范围必须覆盖**整张纹理**：原先写死
								// {COLOR, 0, 1, 0, 1} —— 对 1 mip / 1 层的 2D 纹理恰好等价，
								// 但立方体贴图（arrayLayers == 6）只会转第 0 层，
								// 其余层仍是 Undefined，随后 vkCmdCopyBufferToImage /
								// 采样都会撞上「descriptor/命令要求的 layout 与实际不符」。
								// 顺带把 aspect 也按用途推导，深度附件纹理不再被当成 COLOR。
								.subresourceRange = {
									getVulkanAspectFlagsForUsing(info.texture[i]->getTextureUseFor()),
									0,
									info.texture[i]->getMipLevelCount(),
									0,
									info.texture[i]->getLayerCount(),
								}
							};
							cmdInfo.QuoteResources[info.texture[i]] = { info.beginAccessWhenDone, info.newLayout };
						}
						free(info.texture);
						vkCmdPipelineBarrier(
							cmdInfo.buffer,
							srcStage, dstStage,
							0, 0, nullptr, 0, nullptr, info.count, barriers.data()
						);
						break;
					}
					case RHICommandT::TransferBuffer: {
						BufferTransition_CmdInfo info;
						batchInfo.getBatchData(info);

						uint32_t curFamily = getQueFamilyIndex(mDevice, batchInfo.page->Pool->cmdType);
						uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED;
						uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED;
						VkAccessFlags srcAccess = getVulkanAccessFlags(info.waitForAccessDone);
						VkAccessFlags dstAccess = getVulkanAccessFlags(info.beginAccessWhenDone);
						VkPipelineStageFlags srcStage = getVulkanStageFlags(info.waitForStageDone);
						VkPipelineStageFlags dstStage = getVulkanStageFlags(info.beginStageWhenDone);

						if (info.ResourceQueue != CmdType::None) {
							if (info.ResourceIsTransferOut) {
								// release（转出）：从当前队列移交资源到 ResourceQueue 队列。
								srcFamily = curFamily;
								dstFamily = getQueFamilyIndex(mDevice, info.ResourceQueue);
								dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
								dstAccess = 0;
							} else {
								// acquire（转入）：从 ResourceQueue 队列接管资源到当前队列。
								srcFamily = getQueFamilyIndex(mDevice, info.ResourceQueue);
								dstFamily = curFamily;
								srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
								srcAccess = 0;
							}
						}

						std::vector<VkBufferMemoryBarrier> barriers(info.count);
						for (size_t i = 0; i < info.count; ++i) {
							barriers[i] = {
								.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
								.srcAccessMask = srcAccess,
								.dstAccessMask = dstAccess,
								.srcQueueFamilyIndex = srcFamily,
								.dstQueueFamilyIndex = dstFamily,
								.buffer = static_cast<VkBuffer>(info.buffer[i]->getResourceAPIHandle()),
								.offset = 0,
								.size = info.buffer[i]->getSize()
							};
							cmdInfo.QuoteResources[info.buffer[i]] = { info.beginAccessWhenDone, TextureLayout::Undefined };
						}
						free(info.buffer);
						vkCmdPipelineBarrier(
							cmdInfo.buffer,
							srcStage, dstStage,
							0, 0, nullptr, info.count, barriers.data(), 0, nullptr
						);
						break;
					}
					case RHICommandT::CopyBufferToBuffer: {
						CopyBufferToBuffer_CmdInfo info;
						batchInfo.getBatchData(info);
						VkBuffer srcBuffer = static_cast<VkBuffer>(info.src->getResourceAPIHandle());
						VkBuffer dstBuffer = static_cast<VkBuffer>(info.dst->getResourceAPIHandle());
						VkBufferCopy copyRegion{
							.srcOffset = info.srcOffset,
							.dstOffset = info.dstOffset,
							.size = info.size
						};
						vkCmdCopyBuffer(cmdInfo.buffer, srcBuffer, dstBuffer, 1, &copyRegion);
						break;
					}
					case RHICommandT::CopyBufferToTexture: {
						CopyBufferToTexture_CmdInfo info;
						batchInfo.getBatchData(info);
						VkBuffer srcBuffer = static_cast<VkBuffer>(info.src->getResourceAPIHandle());
						VkImage dstImage = static_cast<VkImage>(info.dst->getResourceAPIHandle());
						// CopyToTexture 的源缓冲按定义是紧密排布（接口里没有源行距参数），
						// 所以 VkBufferImageCopy 的 bufferRowLength / bufferImageHeight 必须给 0
						// （0 = 按 imageExtent 紧密读取）。原先把宽度向上取整到 4 的倍数，只有当宽度
						// 本来就是 4 的倍数时才恰好等价；否则 GPU 会按更大的行距跨行读源缓冲 —— 越界
						// 读 + 图像错行。（ImGui 的字体图集宽度不保证是 4 的倍数，正好会踩中。）
						VkBufferImageCopy region{
							.bufferOffset = info.srcOffset,
							.bufferRowLength = 0,
							.bufferImageHeight = 0,
							.imageSubresource = {
								.aspectMask = getVulkanAspectFlagsForUsing(info.dst->getTextureUseFor()),
								.mipLevel = info.mipLevel,
								.baseArrayLayer = info.arrayindex,
								.layerCount = info.arraycount,
							},
							.imageOffset = {info.dstOffset.x, info.dstOffset.y, info.dstOffset.z},
							.imageExtent = {info.dstSize.width, info.dstSize.height, info.dstSize.depth}
						};
						vkCmdCopyBufferToImage(cmdInfo.buffer, srcBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
						break;
					}
					case RHICommandT::CopyImageToBuffer: {
						CopyImageToBuffer_CmdInfo info;
						batchInfo.getBatchData(info);
						VkImage srcImage = static_cast<VkImage>(info.src->getResourceAPIHandle());
						VkBuffer dstBuffer = static_cast<VkBuffer>(info.dst->getResourceAPIHandle());
						// bufferRowLength / bufferImageHeight = 0 表示紧密打包（tightly packed），
						// 行距 = imageExtent.width，对于 RGBA_8（4 字节）天然满足 4 字节对齐。
						VkBufferImageCopy region{
							.bufferOffset = info.dstOffset,
							.bufferRowLength = 0,
							.bufferImageHeight = 0,
							.imageSubresource = {
								.aspectMask = getVulkanAspectFlagsForUsing(info.src->getTextureUseFor()),
								.mipLevel = info.mipLevel,
								.baseArrayLayer = info.arrayindex,
								.layerCount = info.arraycount,
							},
							.imageOffset = {info.srcOffset.x, info.srcOffset.y, info.srcOffset.z},
							.imageExtent = {info.srcSize.width, info.srcSize.height, info.srcSize.depth}
						};
						vkCmdCopyImageToBuffer(cmdInfo.buffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstBuffer, 1, &region);
						break;
					}
					case RHICommandT::Dispatch: {
						Dispatch_CmdInfo info;
						batchInfo.getBatchData(info);
						vkCmdDispatch(cmdInfo.buffer, info.groupCountX, info.groupCountY, info.groupCountZ);
						break;
					}
					case RHICommandT::Present: {
						Present_CmdInfo info;
						batchInfo.getBatchData(info);
						if (info.swapchain) result->presents.emplace_back(info.swapchain, info.frameID);
						break;
					}
					case RHICommandT::End: {
						End_CmdInfo info;

						batchInfo.getBatchData(info);
						fence.store(static_cast<VulkanFence*>(info.fence), std::memory_order_release);
						if (info.waits != nullptr) {
							waits = std::vector<RHISemaphore*>(info.waits, info.waits + info.waitcount);
							free(info.waits);
						}
						if (info.signals != nullptr) {
							signals = std::vector<RHISemaphore*>(info.signals, info.signals + info.signalcount);
							free(info.signals);
						}
						commandsEndTag.store(true, std::memory_order_release);
						break;
					}
				}		
				currentCmd = batchInfo.getCommandType();
			}
			vkEndCommandBuffer(cmdInfo.buffer);
			finishCount->fetch_add(1, std::memory_order_release);

			usingManager->reBackCommandPool(cmdPool);
		}
	}

}
