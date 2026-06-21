#pragma once

#include <array>
#include <vector>

#include "../RHIContext.h"
#include "VulkanCommandPool.h"
#include "VulkanFencePool.h"
#include "../RHIResourcePack.h"
namespace FISIR{
	static constexpr size_t MAX_PENDING_FRAMES = 3;
	class VulkanPipeline;
	class VulkanDevice;
	class VulkanRHI;
	class RHIFrameBuffer;

	class VulkanContextBase {
	public:
		VulkanContextBase(VulkanDevice* device) : mDevice(device) {}

		CBInfo&& getBackCBInfo() {return std::move(usingCommandBuffer);}

		CBInfo usingCommandBuffer;
		VulkanDevice* mDevice;
	};

	class VulkanRenderContext : public RHIRenderContext, public VulkanContextBase {
	public:
		thread_local static std::unique_ptr<ThreadCommanPoolListener> commandPool;
		VulkanRenderContext(VulkanRHI* rhi, VulkanDevice* device);

		~VulkanRenderContext();

		virtual void RHIBegin() override;

		virtual void RHIEnd() override;

		virtual void RHIBeginDrawingViewport(RHIViewport* viewport, RHITexture* rhiTexture) override;

		virtual void RHIEndDrawingViewport(RHIRenderPass* pass) override;

		virtual void RHIBeginRenderPass(RHIFrameBuffer* frame, const ClearValue& value) override;

		virtual void RHIEndRenderPass() override;

		virtual void RHISetGraphicsPipelineState(RHIPipeline* pipeline) override;

		virtual void RHIDrawPrimitive(unsigned int BaseVertextIndex, unsigned int NumPrimitives, unsigned int NumInstances) override;
		

		virtual void RHISetViewport(float x, float y, float width, float height, float maxDepth, float minDepth) override;

		virtual void RHISetScissor(uint32_t width, uint32_t height) override;

		virtual void RHISetDepthBias(float bias) override;

		virtual void RHIBindResourcePack(RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack) override;

		virtual void RHIBindVertexBuffer(RHIBuffer* buffer, uint32_t binding, uint64_t offset) override;

		virtual void RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) override;

		virtual void RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) override;

		virtual void RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset = 0, uint64_t dstOffset = 0) override;

		virtual void RHICopyTexture(RHIBuffer* src, RHITexture* dst, TextureSize size, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcOffset = 0, TextureSize dstOffset = {0,0,0}) override;

		virtual void* changeOtherHandle(const std::type_info& typ) override;
	
		RHIFrameBuffer* currentFrameBuffer{nullptr};

	};

	class VulkanComputeContext : public RHIComputeContext, public VulkanContextBase{
	public:

		VulkanComputeContext(VulkanRHI* rhi, VulkanDevice* device) : VulkanContextBase(device) {}

		~VulkanComputeContext();

		virtual void RHIBegin() override;

		virtual void RHIEnd() override;

		virtual void RHISetComputePipelineState(RHIPipeline* pipeline) override;

		virtual bool RHIDispatch(unsigned int groupCountX, unsigned int groupCountY, unsigned int groupCountZ) override;


	};

	class VulkanTransferContext : public RHITransferContext, public VulkanContextBase {
	public:

		VulkanTransferContext(VulkanRHI* rhi, VulkanDevice* device) : VulkanContextBase(device) {}
		
		~VulkanTransferContext();

		virtual void RHIBegin() override;

		virtual void RHIEnd() override;

		virtual bool RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset = 0, uint64_t dstOffset = 0) override;

		virtual void RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions) override;

		virtual void RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) override;

	};
}
