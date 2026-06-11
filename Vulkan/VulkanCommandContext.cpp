#include "VulkanCommandContext.h"
#include "VulkanDevice.h"
#include <vulkan/vulkan.h>
#include "VulkanViewport.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanTexture.h"
#include "VulkanBuffer.h"
#include "../../Log/Logger.h"
#include <vulkan/vulkan.h>
#include "VulkanDescriptorPool.h"
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


	/*
		RenderContext
	*/
	VulkanRenderContext::VulkanRenderContext(VulkanDevice* device, VulkanFencePool* fecePool, VulkanCommandPool* cmdPool) {
		mDevice = device;
		mFencePool = fecePool;
		mCommandPool = cmdPool;
		
	}

	VulkanRenderContext::~VulkanRenderContext() {
		//if (!isDestroyed) destroy();
	}

	void VulkanRenderContext::RHIBegin() {
		mCommandBuffer = mCommandPool->createCommandBuffer(_Primary_);
		VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(mCommandBuffer.buffer, &beginInfo);
		Debug("VK RHI Begin RenderCmd");
	}

	void VulkanRenderContext::RHIEnd() {
		vkEndCommandBuffer(mCommandBuffer.buffer);
		Debug("VK RHI End RenderCmd");
	}

	void VulkanRenderContext::RHIBeginDrawingViewport(RHIViewport* viewport, RHITexture* rhiTexture) {
		
	}	

	void VulkanRenderContext::RHIEndDrawingViewport(RHIRenderPass* pass) {

	}

	void VulkanRenderContext::RHIBeginRenderPass(RHIRenderPass* pass) {
		VkRenderPassBeginInfo info {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = (VkRenderPass)pass->getRenderPassHandle()
		};
		vkCmdBeginRenderPass(mCommandBuffer.buffer, &info, VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR);
	}

	void VulkanRenderContext::RHIEndRenderPass() {
		vkCmdEndRenderPass(mCommandBuffer.buffer);

	}

	void VulkanRenderContext::RHISetGraphicsPipelineState(RHIPipeline* pipeline) {
		vkCmdBindPipeline(mCommandBuffer.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)pipeline->getPipelineHandle());
	}

	void VulkanRenderContext::RHIDrawPrimitive(unsigned int BaseVertextIndex, unsigned int NumPrimitives, unsigned int NumInstances) {
		vkCmdDraw(mCommandBuffer.buffer, NumPrimitives, NumInstances, BaseVertextIndex, 0);
	}

	void VulkanRenderContext::RHISetViewport(RHIViewport* viewport) {
		vkCmdSetViewport(mCommandBuffer.buffer, 0, 1, nullptr);
	}

	void VulkanRenderContext::RHISetScissor(uint32_t width, uint32_t height) {
		VkRect2D exten {{0, 0}, {width, height}};
		vkCmdSetScissor(mCommandBuffer.buffer, 0, 1, &exten);
	}

	void VulkanRenderContext::RHISetDepthBias(float bias) {
		vkCmdSetDepthBias(mCommandBuffer.buffer, 0.0, 0.0, 0.0);
	}

	void VulkanRenderContext::RHIBindResourcePack(RHIResourcePack* pack) {
		CmdBindResourcePack(mDevice, mCommandBuffer.buffer, pack);
	}

	void VulkanRenderContext::RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
		std::vector<VkImageMemoryBarrier> Barriers;
		for (auto& [texture, waitForAccessDone, beginAccessWhenDone, oldlayout, newlayout] : textureTransitions) {
			mCommandBuffer.QuoteResources[texture] = { beginAccessWhenDone, newlayout };
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
		}
		vkCmdPipelineBarrier(mCommandBuffer.buffer, 
			getVulkanPipelineSatgeFlags(waitForStageDone), 
			getVulkanPipelineSatgeFlags(beginStageWhenDone),
			0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(Barriers.size()), Barriers.data());

		
	}

	void VulkanRenderContext::RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
		std::vector<VkBufferMemoryBarrier> Barriers;
		for (auto& [buffer, waitForAccessDone, beginAccessWhenDone] : bufferTransitions) {
			mCommandBuffer.QuoteResources[buffer] = {beginAccessWhenDone};
			VkBufferMemoryBarrier barrier {
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = getVulkanAccessFlags(waitForAccessDone),
				.dstAccessMask = getVulkanAccessFlags(beginAccessWhenDone),
				.buffer = static_cast<VkBuffer>(buffer->getResourceAPIHandle()),
				.offset = 0,
				.size = buffer->getSize()
			};
			Barriers.push_back(barrier);
		}
		vkCmdPipelineBarrier(mCommandBuffer.buffer, 
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

		vkCmdCopyBuffer2(mCommandBuffer.buffer, &copyinfo);
	
	}

	void VulkanRenderContext::RHICopyTexture(RHIBuffer* src, RHITexture* dst, TextureSize size, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcOffset, TextureSize dstOffset) {
		if (mCommandBuffer.QuoteResources[dst].layout != TextureLayout::TransferDstOptimal) {
			TextureTransitionInfo tranInfo {
				.texture = dst,
				.waitForAccessDone = mCommandBuffer.QuoteResources.contains(dst) ? mCommandBuffer.QuoteResources[dst].access : ResourceAccess::Undefined,
				.beginAccessWhenDone = ResourceAccess::TransferDst,
				.oldLayout = mCommandBuffer.QuoteResources.contains(dst) ? mCommandBuffer.QuoteResources[dst].layout : TextureLayout::Undefined,
				.newLayout = TextureLayout::TransferDstOptimal,
			};
			RHITransitionTextures({tranInfo}, RHIUsingStage::PipelinTopStage, RHIUsingStage::PipelineTransferStage);
		
		}
		VkBufferImageCopy2 sizeinfo {
			.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
			.bufferOffset= srcOffset,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
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
		vkCmdCopyBufferToImage2(mCommandBuffer.buffer, &copyinfo);
	}



	/*
		ComputeContext
	*/
	VulkanComputeContext::VulkanComputeContext(VulkanDevice* device, VulkanFencePool* fecePool, VulkanCommandPool* cmdPool) {
		mDevice = device;
		mFencePool = fecePool;
		mCommandPool = cmdPool;

	}

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
	VulkanTransferContext::VulkanTransferContext(VulkanDevice* device, VulkanFencePool* fencePool, VulkanCommandPool* cmdPool) {
		mDevice = device;
		mFencePool = fencePool;
		mCommandPool = cmdPool;
	}

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

	void VulkanTransferContext::RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions)
	{
	}



}