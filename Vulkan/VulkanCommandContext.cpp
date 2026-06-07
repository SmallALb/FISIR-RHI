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