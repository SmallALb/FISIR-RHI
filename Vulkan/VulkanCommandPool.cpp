#include "VulkanCommandPool.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanFrameBuffer.h"
#include "VulkanDescriptorPool.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include "../../Log/Logger.h"


namespace FISIR{
	constexpr size_t INITIAL_PRIMARY_COMMAND_BUFFER_COUNT = 64;
	constexpr size_t INITIAL_SECONDARY_COMMAND_BUFFER_COUNT = 128;

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
		std::vector<VkCommandBuffer> PrimaryCommandBufferPool;
		std::vector<VkCommandBuffer> SecondaryCommandBufferPool;
		std::unordered_map<VkCommandBuffer, size_t> PrimaryCommandBufferUsage; 
		std::unordered_map<VkCommandBuffer, size_t> SecondaryCommandBufferUsage;
		std::queue<size_t> FreePrimaryCommandBuffers;
		std::queue<size_t> FreeSecondaryCommandBuffers;
	};

	

	VulkanCommandPool::VulkanCommandPool(VulkanDevice* device, CmdType mType, uint32_t FamilyIndex) {
		mDevice = device;
		mPoolType = mType;
		mData = new __VKCommandPoolData();
		mData->PrimaryCommandBufferPool.resize(INITIAL_PRIMARY_COMMAND_BUFFER_COUNT);
		mData->SecondaryCommandBufferPool.resize(INITIAL_SECONDARY_COMMAND_BUFFER_COUNT);
		
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
			.commandBufferCount = static_cast<uint32_t>(mData->PrimaryCommandBufferPool.size())
		};
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->PrimaryCommandBufferPool.data());

		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		allocInfo.commandBufferCount = static_cast<uint32_t>(mData->SecondaryCommandBufferPool.size());
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->SecondaryCommandBufferPool.data());
	
		for (size_t i = 0; i < mData->PrimaryCommandBufferPool.size(); ++i) {
			mData->PrimaryCommandBufferUsage[mData->PrimaryCommandBufferPool[i]] = i;
			mData->FreePrimaryCommandBuffers.push(i);
		}

		for (size_t i = 0; i < mData->SecondaryCommandBufferPool.size(); ++i) {
			mData->SecondaryCommandBufferUsage[mData->SecondaryCommandBufferPool[i]] = i;
			mData->FreeSecondaryCommandBuffers.push(i);
		}
	}
	
	VulkanCommandPool::~VulkanCommandPool() {
		if (mData) {
			vkFreeCommandBuffers(mDevice->getLogicalDevice(), mData->mPool, static_cast<uint32_t>(mData->PrimaryCommandBufferPool.size()), mData->PrimaryCommandBufferPool.data());
			vkFreeCommandBuffers(mDevice->getLogicalDevice(), mData->mPool, static_cast<uint32_t>(mData->SecondaryCommandBufferPool.size()), mData->SecondaryCommandBufferPool.data());
			vkDestroyCommandPool(mDevice->getLogicalDevice(), mData->mPool, nullptr);
			delete mData;
			mData = nullptr;
		}
	}

	CBInfo VulkanCommandPool::createCommandBuffer(CommandBufferType cbType) {
		if ((cbType == _Primary_) ? mData->FreePrimaryCommandBuffers.empty() : mData->FreeSecondaryCommandBuffers.empty()) {
			VkCommandBufferAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = mData->mPool,
				.level = (cbType == _Primary_) ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY,
				.commandBufferCount = 1
			};
			VkCommandBuffer newCB;
			if (vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, &newCB) != VK_SUCCESS) {
				Error("Failed to allocate command buffer!");
				return {};
			}
			if (cbType == _Primary_) {
				mData->PrimaryCommandBufferPool.push_back(newCB);
				mData->FreePrimaryCommandBuffers.push(mData->PrimaryCommandBufferPool.size() - 1);
			}
			else {
				mData->SecondaryCommandBufferPool.push_back(newCB);
				mData->FreeSecondaryCommandBuffers.push(mData->SecondaryCommandBufferPool.size() - 1);
			}
			(cbType == _Primary_ ? mData->PrimaryCommandBufferUsage[newCB] : mData->SecondaryCommandBufferUsage[newCB]) 
				= (cbType == _Primary_ ? mData->FreePrimaryCommandBuffers.size()-1 : mData->FreeSecondaryCommandBuffers.size()-1);
		}
		uint32_t index = (cbType == _Primary_) ? mData->FreePrimaryCommandBuffers.front() : mData->FreeSecondaryCommandBuffers.front();
		(cbType == _Primary_) ? mData->FreePrimaryCommandBuffers.pop() : mData->FreeSecondaryCommandBuffers.pop();
		
		CBInfo cbInfo((cbType == _Primary_) ? mData->PrimaryCommandBufferPool[index] : mData->SecondaryCommandBufferPool[index], cbType, mPoolType, this);

		if (cbInfo.buffer == VK_NULL_HANDLE) {
			Error("Failed to allocate command buffer!");
			return {};
		}
		return cbInfo;
	}

	void VulkanCommandPool::releaseCommandBuffer(const CBInfo& cbInfo) {
		if (cbInfo.pool != this) {
			Error("Attempting to release a command buffer that does not belong to this pool!");
			return;
		}
		vkResetCommandBuffer(cbInfo.buffer, 0);
		(cbInfo.type == _Primary_) ? mData->FreePrimaryCommandBuffers.push(mData->PrimaryCommandBufferUsage[cbInfo.buffer]) : mData->FreeSecondaryCommandBuffers.push(mData->SecondaryCommandBufferUsage[cbInfo.buffer]);

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
		ThreadFlags.resize(5, 0);
		ThreadExecutedCBs.resize(5);

		for (size_t i=0; i<Threads.size(); ++i) {
			Threads[i] = std::thread(&CommandExecuteThreadPool::ThreadLoop, this, static_cast<int>(i));
		}
	}

	CommandExecuteThreadPool::~CommandExecuteThreadPool() {
		StopTag = 1;

		for (auto& t : Threads) t.join();
	}

	void CommandExecuteThreadPool::pushCommandBatch(RingCommandPool::Page* page, std::atomic_int* threadId) {
		RingCommandPool::Page::BatchInfo batchInfo;
		if (page->BatchQueue.pop(batchInfo)) {
			NeedExecutePages.push({ batchInfo, threadId });
		}
	}

	
	CBInfo&& CommandExecuteThreadPool::getExecutedCB(uint32_t ThreadId, VulkanRenderPass** renderPass) {
		if (ThreadFlags[ThreadId].load(std::memory_order_acquire)) {
			ThreadData& data = ThreadExecutedCBs[ThreadId];
			ThreadFlags[ThreadId].store(0, std::memory_order_release);
			if (renderPass) *renderPass = data.renderPass;
			return std::move(data.ExecutedCB);
		}
		return CBInfo();
	}


	void CommandExecuteThreadPool::ThreadLoop(int ThreadID) {
		
		while(!StopTag.load()) {
			if (ThreadFlags[ThreadID].load(std::memory_order_acquire)) {
				std::this_thread::yield();
				continue;
			}

			ExecutedPageTask exeTask;
			if (NeedExecutePages.empty() || !NeedExecutePages.pop(exeTask)) {
				std::this_thread::yield();
				continue;
			}

			auto& [batchInfo, threadId] = exeTask;
			threadId->store(ThreadID, std::memory_order_release);

			RHICommandT currentCmd = batchInfo.getCommandType();
			auto cmdPool = usingManager->getCommandPool(batchInfo.page->Pool->cmdType);
			auto& [renderPass, cmdInfo] = ThreadExecutedCBs[ThreadID];
			cmdInfo = cmdPool->createCommandBuffer(CommandBufferType::_Secondary_);

			VkCommandBufferInheritanceInfo inheritanceInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
			};
			if (currentCmd == RHICommandT::BeginRenderPass) {
				BeginRenderPass_CmdInfo info;
				batchInfo.getBatchData(info);
				renderPass = static_cast<VulkanRenderPass*>(info.frame->getFrameRenderPass());
				inheritanceInfo.renderPass = static_cast<VkRenderPass>(renderPass->getRenderPassHandle());
				inheritanceInfo.framebuffer = static_cast<VkFramebuffer>(info.frame->getResourceAPIHandle());
				inheritanceInfo.subpass = 0;
			}


			VkCommandBufferBeginInfo BeginInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | (currentCmd == RHICommandT::BeginRenderPass ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : (VkCommandBufferUsageFlagBits)0),
				.pInheritanceInfo = currentCmd == RHICommandT::BeginRenderPass ? &inheritanceInfo : nullptr
			};
			vkBeginCommandBuffer(cmdInfo.buffer, &BeginInfo);
			while(currentCmd != RHICommandT::End && currentCmd != RHICommandT::EndRenderPass) {

				switch (currentCmd) {
					case RHICommandT::BeginRenderPass: {
						break;
					}
					case RHICommandT::EndRenderPass: {
						ReserveInput_CmdInfo info;
						batchInfo.getBatchData(info);
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
					case RHICommandT::BindPipeline: {
						RHIPipeline* info;
						batchInfo.getBatchData(info);
						vkCmdBindPipeline(cmdInfo.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(info->getPipelineHandle()));
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
						CmdBindResourcePack(mDevice, cmdInfo.buffer, info.Pack.ResourcePack, info.Pack.SamplerPack);
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
								.oldLayout = static_cast<VkImageLayout>(info.texture[i]->getCurrentLayout()),
								.newLayout = static_cast<VkImageLayout>(info.newLayout),
								.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
								.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
								.image = static_cast<VkImage>(info.texture[i]->getResourceAPIHandle()),
								.subresourceRange = {
									VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
								}
							};
							cmdInfo.QuoteResources[info.texture[i]] = { info.beginAccessWhenDone, info.newLayout };
						}
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
						ReserveInput_CmdInfo info;
						batchInfo.getBatchData(info);
						break;
					}
				}		
				currentCmd = batchInfo.getCommandType();
			}
			vkEndCommandBuffer(cmdInfo.buffer);
			ThreadFlags[ThreadID].store(1, std::memory_order_release);

			usingManager->reBackCommandPool(cmdPool);
		}
	}

}
