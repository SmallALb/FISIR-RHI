#include "VulkanCommandContext.h"
#include "VulkanDevice.h"
#include <vulkan/vulkan.h>
#include "VulkanViewport.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanTexture.h"
#include "VulkanBuffer.h"
#include "../../Log/Logger.h"
#include "VulkanDescriptorPool.h"
#include "VulkanFrameBuffer.h"
#include "VulkanFencePool.h"
#include "VulkanSwapChian.h"
#include "VulkanRHI.h"
#include <iostream>
namespace FISIR{
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

	static VkPipelineStageFlags getVulkanPipelineSatgeFlags(RHIUsingStage stage) {
		VkPipelineStageFlags flags = 0;

		if (stage == RHIUsingStage::NoneStage) {
			return 0;
		}

		// 着色器阶段
		if (stage & RHIUsingStage::VertexShaderStage) {
			flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		}

		if (stage & RHIUsingStage::FragmentShaderStage) {
			flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}

		if (stage & RHIUsingStage::TessShaderStage) {
			flags |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
				VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
		}

		if (stage & RHIUsingStage::ComputeShaderStage) {
			flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		}

		if (stage & RHIUsingStage::GeometryShaderStage) {
			flags |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
		}

		// 管线阶段
		if (stage & RHIUsingStage::PipelinTopStage) {
			flags |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		}

		if (stage & RHIUsingStage::PipelineBottomStage) {
			flags |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		}

		if (stage & RHIUsingStage::PipelineVertexInputStage) {
			flags |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		}

		if (stage & RHIUsingStage::PipelineBeforeFragmentStage) {
			flags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		}

		if (stage & RHIUsingStage::PipelineAfterFragmentStage) {
			flags |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		}
		if (stage & RHIUsingStage::PipelineTransferStage){
			flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		return flags;
		
	}

	static VkImageAspectFlags getVulkanAspectFlagsForUsing(TextureUseForFlags usefor) {
		return usefor & TextureUseForDepthStencilAttachment ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	}


	static ResourceAccess getAccessFromLayout(TextureLayout layout) {
		switch (layout) {
		case TextureLayout::ColorAttachmentOptimal:
		case TextureLayout::DepthStencilAttachmentOptimal:
			return ResourceAccess::ShaderReadWrite;
		case TextureLayout::ShaderReadOnlyOptimal:
			return ResourceAccess::ShaderReadOnly;
		case TextureLayout::TransferSrcOptimal:
			return ResourceAccess::TransferSrc;
		case TextureLayout::TransferDstOptimal:
			return ResourceAccess::TransferDst;
		default:
			return ResourceAccess::Undefined;
		}
	
	}

	/*
		RenderContext
	*/
	
	thread_local std::unique_ptr<ThreadCommanPoolListener> VulkanRenderContext::commandPool = nullptr;
	VulkanRenderContext::VulkanRenderContext(VulkanRHI* rhi, VulkanDevice* device) : VulkanContextBase(device) {
		if (VulkanRenderContext::commandPool == nullptr) VulkanRenderContext::commandPool.reset(rhi->choiceCommandPool(CmdType::Render));
		
		usingCommandBuffer = VulkanRenderContext::commandPool->mCommandPool->createCommandBuffer(CommandBufferType::_Primary_);
	}

	VulkanRenderContext::~VulkanRenderContext() {
	
	}

	void VulkanRenderContext::RHIBegin() {
		//Warn("Try Begin Command!");
		VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(usingCommandBuffer.buffer, &beginInfo);
		if (!usingCommandBuffer.buffer) {
			Error("CommandBuffer is Null ");
			//Warn("RHI Begin!");
		}
	}

	void VulkanRenderContext::RHIEnd() {
		//Debug("RHIEnd Begin");
		if (vkEndCommandBuffer(usingCommandBuffer.buffer) != VK_SUCCESS) Error("Vulkan Command End Failed");
		//Debug("RHIEnd finished");

	}

	void VulkanRenderContext::RHIBeginDrawingViewport(RHIViewport* viewport, RHITexture* rhiTexture) {
		
	}	

	void VulkanRenderContext::RHIEndDrawingViewport(RHIRenderPass* pass) {

	}

	void VulkanRenderContext::RHIBeginRenderPass(RHIFrameBuffer* frame, const ClearValue& value) {
		currentFrameBuffer = frame;
		//Debug("Begin Frame Render, Buffer Handle 0x{:x}", (size_t)currentFrameBuffer);
		std::vector<VkClearValue> clearValues;
		auto& [isColorC, c, isDepthStencilC, dc, s]  = value;

		if (isColorC) {
			VkClearValue val {
				.color = {c.R, c.G, c.B, c.A}
			};
			clearValues.push_back(val);		
		}

		if (isDepthStencilC) {
			VkClearValue val{
				.depthStencil = {dc, s}
			};
			clearValues.push_back(val);
		}
		auto renderpass = currentFrameBuffer->getFrameRenderPass();

		VkRenderPassBeginInfo info {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = static_cast<VkRenderPass>(renderpass->getRenderPassHandle()),
			.framebuffer = static_cast<VkFramebuffer>(currentFrameBuffer->getResourceAPIHandle()),
			.renderArea = {
				.offset = {0, 0},
				.extent = {currentFrameBuffer->getFrameWidth(), currentFrameBuffer->getFrameHeight()},
			},
			.clearValueCount = (uint32_t)(clearValues.size()),
			.pClearValues = clearValues.data(),
		};
 		vkCmdBeginRenderPass(usingCommandBuffer.buffer, &info, VK_SUBPASS_CONTENTS_INLINE);
	}

	void VulkanRenderContext::RHIEndRenderPass() {
		vkCmdEndRenderPass(usingCommandBuffer.buffer);

		auto& textures = currentFrameBuffer->getFrameTextures();
		auto renderpasss = currentFrameBuffer->getFrameRenderPass();
		for (size_t i = 0; i<textures.size(); i++) {
			TextureLayout layout = renderpasss->getAttachmentFinalLayout(i);
			//textures[i]->setWait();
			//usingCommandBuffer.QuoteResources[textures[i]] = {getAccessFromLayout(layout), layout};
			static_cast<VulkanTexture*>(textures[i])->transitionLayout(layout);
		}
	}

	void VulkanRenderContext::RHISetGraphicsPipelineState(RHIPipeline* pipeline) {
		vkCmdBindPipeline(usingCommandBuffer.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)pipeline->getPipelineHandle());
	}

	void VulkanRenderContext::RHIDrawPrimitive(unsigned int BaseVertextIndex, unsigned int NumPrimitives, unsigned int NumInstances) {
		vkCmdDraw(usingCommandBuffer.buffer, NumPrimitives, NumInstances, BaseVertextIndex, 0);
	}


	void VulkanRenderContext::RHISetViewport(float x, float y, float width, float height, float maxDepth, float minDepth) {
		VkViewport viewport {
			.x = x,
			.y = y,
			.width = width,
			.height = height,
			.minDepth = minDepth,
			.maxDepth = maxDepth,
		};
		vkCmdSetViewport(usingCommandBuffer.buffer, 0, 1, &viewport);
	}

	void VulkanRenderContext::RHISetScissor(uint32_t width, uint32_t height) {
		VkRect2D exten {{0, 0}, {width, height}};
		vkCmdSetScissor(usingCommandBuffer.buffer, 0, 1, &exten);
	}

	void VulkanRenderContext::RHISetDepthBias(float bias) {
		vkCmdSetDepthBias(usingCommandBuffer.buffer, 0.0, 0.0, 0.0);
	}

	void VulkanRenderContext::RHIBindResourcePack(RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack) {
		CmdBindResourcePack(mDevice, usingCommandBuffer.buffer, Resourcepack, Samplerpack);
	}

	void VulkanRenderContext::RHIBindVertexBuffer(RHIBuffer* buffer, uint32_t binding, uint64_t offset) {
		auto vkbuffer = static_cast<VkBuffer>(buffer->getResourceAPIHandle());
		vkCmdBindVertexBuffers(usingCommandBuffer.buffer, binding, 1, &vkbuffer, &offset);
	}

	void VulkanRenderContext::RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
		std::vector<VkImageMemoryBarrier> Barriers;
		for (auto& [texture, waitForAccessDone, beginAccessWhenDone, oldlayout, newlayout] : textureTransitions) {
			if (usingCommandBuffer.QuoteResources.contains(texture) && usingCommandBuffer.QuoteResources[texture].access == beginAccessWhenDone && usingCommandBuffer.QuoteResources[texture].layout == newlayout) continue;
			VkImageMemoryBarrier barrier {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = getVulkanAccessFlags(waitForAccessDone),
				.dstAccessMask = getVulkanAccessFlags(beginAccessWhenDone),
				.oldLayout = oldlayout != TextureLayout::Undefined ? getVulkanImageLayout(oldlayout) : getVulkanImageLayout(texture->getCurrentLayout()),
				.newLayout = getVulkanImageLayout(newlayout),
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = static_cast<VkImage>(texture->getResourceAPIHandle()),
				.subresourceRange = {getVulkanAspectFlagsForUsing(texture->getTextureUseFor()), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
			};
			Barriers.push_back(barrier);
			usingCommandBuffer.QuoteResources[texture] = { beginAccessWhenDone, newlayout };
			//texture->setWait();
		}
		if (Barriers.empty()) return;
		vkCmdPipelineBarrier(usingCommandBuffer.buffer, 
			getVulkanPipelineSatgeFlags(waitForStageDone), 
			getVulkanPipelineSatgeFlags(beginStageWhenDone),
			0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(Barriers.size()), Barriers.data());
		
		
	}

	void VulkanRenderContext::RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
		std::vector<VkBufferMemoryBarrier> Barriers;
		for (auto& [buffer, waitForAccessDone, beginAccessWhenDone] : bufferTransitions) {
			usingCommandBuffer.QuoteResources[buffer] = {beginAccessWhenDone};
			VkBufferMemoryBarrier barrier {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = getVulkanAccessFlags(waitForAccessDone),
				.dstAccessMask = getVulkanAccessFlags(beginAccessWhenDone),
				.buffer = static_cast<VkBuffer>(buffer->getResourceAPIHandle()),
				.offset = 0,
				.size = buffer->getSize()
			};
			//buffer->setWait();
			Barriers.push_back(barrier);
		}
		vkCmdPipelineBarrier(usingCommandBuffer.buffer, 
			getVulkanPipelineSatgeFlags(waitForStageDone),
			getVulkanPipelineSatgeFlags(beginStageWhenDone),
			0, 0, nullptr, static_cast<uint32_t>(Barriers.size()), Barriers.data(), 0, nullptr);
	
	}

	void VulkanRenderContext::RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset, uint64_t dstOffset) {
		VkBufferCopy2 sizeinfo {
			.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
			.srcOffset = srcOffset,
			.dstOffset = dstOffset,
			.size = static_cast<uint32_t>(size),
		};
		
		VkCopyBufferInfo2 copyinfo {
			.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
			.srcBuffer = static_cast<VkBuffer>(srcBuffer->getResourceAPIHandle()),
			.dstBuffer = static_cast<VkBuffer>(dstBuffer->getResourceAPIHandle()),
			.pRegions = &sizeinfo,
		};

		vkCmdCopyBuffer2(usingCommandBuffer.buffer, &copyinfo);
	
	}

	void VulkanRenderContext::RHICopyTexture(RHIBuffer* src, RHITexture* dst, TextureSize size, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcOffset, TextureSize dstOffset) {
	
		if (usingCommandBuffer.QuoteResources[dst].layout != TextureLayout::TransferDstOptimal) {
			TextureTransitionInfo tranInfo {
				.texture = dst,
				.waitForAccessDone = usingCommandBuffer.QuoteResources.contains(dst) ? usingCommandBuffer.QuoteResources[dst].access : ResourceAccess::Undefined,
				.beginAccessWhenDone = ResourceAccess::TransferDst,
				.oldLayout = usingCommandBuffer.QuoteResources.contains(dst) ? usingCommandBuffer.QuoteResources[dst].layout : TextureLayout::Undefined,
				.newLayout = TextureLayout::TransferDstOptimal,
			};
			RHITransitionTextures({tranInfo}, RHIUsingStage::PipelinTopStage, RHIUsingStage::PipelineTransferStage);
		
		}
		uint32_t bytesPerPixel = 4;
		uint32_t rowLength = size.width;
		uint32_t alignedRowLength = ((rowLength + 3) & ~3);

		Debug("RHICopyTexture: size.width={}, size.height={}, alignedRowLength={}, bufferRowLength={}, bufferImageHeight={}",
			size.width, size.height, alignedRowLength, alignedRowLength, size.height);
		VkBufferImageCopy2 sizeinfo {
			.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
			.bufferOffset= srcOffset,
			.bufferRowLength = alignedRowLength,
			.bufferImageHeight = size.height,
			.imageSubresource = {
				.aspectMask = getVulkanAspectFlagsForUsing(dst->getTextureUseFor()),
				.mipLevel = miplevel,
				.baseArrayLayer = arrayindex,
				.layerCount = arraycount,
			},
			.imageOffset = {dstOffset.x, dstOffset.y, dstOffset.z},
			.imageExtent = {size.width, size.height, size.depth},
		};
		
		VkCopyBufferToImageInfo2 copyinfo {
			.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
			.srcBuffer = static_cast<VkBuffer>(src->getResourceAPIHandle()),
			.dstImage = static_cast<VkImage>(dst->getResourceAPIHandle()),
			.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.regionCount = 1,
			.pRegions = &sizeinfo,
		};
		vkCmdCopyBufferToImage2(usingCommandBuffer.buffer, &copyinfo);
	}

	void* VulkanRenderContext::changeOtherHandle(const std::type_info& typ) {
		if (typ == typeid(VulkanContextBase))
			return static_cast<VulkanContextBase*>(this);
		return nullptr;
	}

	/*
		ComputeContext
	*/
	

	VulkanComputeContext::~VulkanComputeContext() {

	}

	void VulkanComputeContext::RHIBegin()
	{
	}

	void VulkanComputeContext::RHIEnd()
	{
	}

	void VulkanComputeContext::RHISetComputePipelineState(RHIPipeline* pipeline)
	{
	}

	bool VulkanComputeContext::RHIDispatch(unsigned int groupCountX, unsigned int groupCountY, unsigned int groupCountZ)
	{
		return false;
	}



	/*
		TransferContext
	*/

	VulkanTransferContext::~VulkanTransferContext() {

	}

	void VulkanTransferContext::RHIBegin()
	{
	}

	void VulkanTransferContext::RHIEnd()
	{
	}

	bool VulkanTransferContext::RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset, uint64_t dstOffset)
	{
		return false;
	}

	void VulkanTransferContext::RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions)
	{
	}

	void VulkanTransferContext::RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) {
	}


}