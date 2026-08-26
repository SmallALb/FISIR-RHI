#include "VulkanCommandPool.h"

#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
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
		default:
			return VK_ACCESS_NONE;
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

		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		allocInfo.commandBufferCount = INITIAL_SECONDARY_COMMAND_BUFFER_COUNT;
		mData->SecondaryCommandBufferPool.push_back(new VkCommandBuffer[INITIAL_SECONDARY_COMMAND_BUFFER_COUNT]);
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->SecondaryCommandBufferPool[0]);
	
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


		if (Que.pop(index)) return CBInfo(Pool[index/Count][index%Count], cbType, mPoolType, this, index);

		std::lock_guard<std::mutex> lock(mCommandBufferMutex);
		if (Que.pop(index)) return CBInfo(Pool[index / Count][index % Count], cbType, mPoolType, this, index);
		
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

		index = NewPageBegin;
		return CBInfo(Pool[index / Count][index % Count], cbType, mPoolType, this , index);
	}

	void VulkanCommandPool::releaseCommandBuffer(const CBInfo& cbInfo) {
		//std::lock_guard<std::mutex> lock(mCommandBufferMutex);
		if (cbInfo.pool != this) {
			Error("Attempting to release a command buffer that does not belong to this pool!");
			return;
		}
		 //vkResetCommandBuffer(cbInfo.buffer, 0);
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

	VulkanCommandPool* VulkanCommandPoolManager::getCommandPool(uint32_t FamilyIndex) {
		std::lock_guard<std::mutex> lock(MapMutex);
		if (FamilyIndexToPool.contains(FamilyIndex)) return FamilyIndexToPool[FamilyIndex];
		FamilyIndexToPool[FamilyIndex] = new VulkanCommandPool(mDevice, CmdType::None, FamilyIndex);
		return FamilyIndexToPool[FamilyIndex];
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
		if ((size_t)batch.page == 0xDDDDDDDDDDDDDDDD) {
			//WTF R U GET ?????
			__debugbreak();
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
			auto cmdPool = usingManager->getCommandPool(batchInfo.page->Pool->cmdType);
			auto& [framebuffer, clearval, renderPassEndTag, commandsEndTag, subpassIndex, cmdInfo, fence, waits, signals] = *result;
			cmdInfo = cmdPool->createCommandBuffer(CommandBufferType::_Secondary_);

			VkCommandBufferInheritanceInfo inheritanceInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
			};
			if (currentCmd == RHICommandT::BeginRenderPass) {
				BeginRenderPass_CmdInfo info{};
				if (batchInfo.getBatchData(info)) {
					framebuffer = static_cast<VulkanFrameBuffer*>(info.frame);
					if ((size_t)framebuffer >= 0x10000) {
						inheritanceInfo.renderPass = static_cast<VkRenderPass>(framebuffer->getFrameRenderPass()->getRenderPassHandle());
						inheritanceInfo.framebuffer = static_cast<VkFramebuffer>(info.frame->getResourceAPIHandle());
						inheritanceInfo.subpass = info.subpassIndex;
						subpassIndex = info.subpassIndex;
						clearval = info.clearValue;
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
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | (currentCmd == RHICommandT::BeginRenderPass ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : (VkCommandBufferUsageFlags)0),
				.pInheritanceInfo = &inheritanceInfo
			};
			vkBeginCommandBuffer(cmdInfo.buffer, &BeginInfo);
			while(batchInfo.ReadBegin < batchInfo.ReadEnd) {

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
						VkPipelineBindPoint bindPoint = (batchInfo.page->Pool->cmdType == CmdType::Compute)
							? VK_PIPELINE_BIND_POINT_COMPUTE
							: VK_PIPELINE_BIND_POINT_GRAPHICS;
						vkCmdBindPipeline(cmdInfo.buffer, bindPoint, static_cast<VkPipeline>(info.pipeline->getPipelineHandle()));
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
							{0, 0}, {info.width, info.height}
						};
						vkCmdSetScissor(cmdInfo.buffer, 0, 1, &scissor);
						break;
					}
					case RHICommandT::BindResourceAndSamplerPack: {
						BindResourcePack_CmdInfo info;
						batchInfo.getBatchData(info);
						VkPipelineBindPoint bindPoint = (batchInfo.page->Pool->cmdType == CmdType::Compute)
							? VK_PIPELINE_BIND_POINT_COMPUTE
							: VK_PIPELINE_BIND_POINT_GRAPHICS;
						CmdBindResourcePack(mDevice, cmdInfo.buffer, info.Pack.ResourcePack, info.Pack.SamplerPack, (uint32_t)bindPoint);
						break;
					}
					case RHICommandT::TransferTexture: {
						TextureTransition_CmdInfo info;
						batchInfo.getBatchData(info);
						std::vector<VkImageMemoryBarrier> barriers(info.count);
						for (size_t i = 0; i < info.count; ++i) {
							barriers[i] = {
								.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
								.srcAccessMask = getVulkanAccessFlags(info.waitForAccessDone),
								.dstAccessMask = getVulkanAccessFlags(info.beginAccessWhenDone),
								.oldLayout = getVulkanImageLayout(info.texture[i]->getCurrentLayout()),
								.newLayout = getVulkanImageLayout(info.newLayout),
								.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
								.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
								.image = static_cast<VkImage>(info.texture[i]->getResourceAPIHandle()),
								.subresourceRange = {
									VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
								}
							};
							cmdInfo.QuoteResources[info.texture[i]] = { info.beginAccessWhenDone, info.newLayout };
						}
						free(info.texture);
						vkCmdPipelineBarrier(
							cmdInfo.buffer, 
							VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 
							0, 0, nullptr, 0, nullptr, info.count, barriers.data()
						);
						break;
					}
					case RHICommandT::TransferBuffer: {
						BufferTransition_CmdInfo info;
						batchInfo.getBatchData(info);
						std::vector<VkBufferMemoryBarrier> barriers(info.count);
						for (size_t i = 0; i < info.count; ++i) {
							barriers[i] = {
								.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
								.srcAccessMask = getVulkanAccessFlags(info.waitForAccessDone),
								.dstAccessMask = getVulkanAccessFlags(info.beginAccessWhenDone),
								.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
								.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
								.buffer = static_cast<VkBuffer>(info.buffer[i]->getResourceAPIHandle()),
								.offset = 0,
								.size = info.buffer[i]->getSize()
							};
							cmdInfo.QuoteResources[info.buffer[i]] = { info.beginAccessWhenDone, TextureLayout::Undefined };
						}
						free(info.buffer);
						vkCmdPipelineBarrier(
							cmdInfo.buffer, 
							VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 
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
						uint32_t rowLength = info.dstSize.width;
						uint32_t alignedRowLength = ((rowLength + 3) & ~3);
						VkBufferImageCopy region{
							.bufferOffset = info.srcOffset,
							.bufferRowLength = alignedRowLength,
							.bufferImageHeight = info.dstSize.height,
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
					case RHICommandT::Dispatch: {
						Dispatch_CmdInfo info;
						batchInfo.getBatchData(info);
						vkCmdDispatch(cmdInfo.buffer, info.groupCountX, info.groupCountY, info.groupCountZ);
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
