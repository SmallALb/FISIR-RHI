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
	

	class VulkanContextBase {
	public:
		virtual CBInfo getCommandBuffer() = 0;

		virtual VulkanCommandPool* getCommandPool() = 0;

	};

	class VulkanRenderContext : public RHIRenderContext, public VulkanContextBase {
	public:
		VulkanRenderContext(VulkanDevice* device, VulkanFencePool* fecePool, VulkanCommandPool* cmdPool) ;

		~VulkanRenderContext();

		virtual void RHIBegin() override;

		virtual void RHIEnd() override;

		virtual void RHIBeginDrawingViewport(RHIViewport* viewport, RHITexture* rhiTexture) override;

		virtual void RHIEndDrawingViewport(RHIRenderPass* pass) override;

		virtual void RHIBeginRenderPass(RHIRenderPass* pass) override;

		virtual void RHIEndRenderPass() override;

		virtual void RHISetGraphicsPipelineState(RHIPipeline* pipeline) override;

		virtual void RHIDrawPrimitive(unsigned int BaseVertextIndex, unsigned int NumPrimitives, unsigned int NumInstances) override;
		
		virtual CBInfo getCommandBuffer() override { return mCommandBuffer; }

		virtual VulkanCommandPool* getCommandPool() override {return mCommandPool;}

		virtual void RHISetViewport(RHIViewport* viewport) override;

		virtual void RHISetScissor(uint32_t width, uint32_t height) override;

		virtual void RHISetDepthBias(float bias) override;

		virtual void RHIBindResourcePack(RHIResourcePack* pack) override;

		virtual void RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) override;

		virtual void RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) override;


		virtual void RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset = 0, uint64_t dstOffset = 0) override;

		virtual void RHICopyTexture(RHIBuffer* src, RHITexture* dst, TextureSize size, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcOffset = 0, TextureSize dstOffset = {0,0,0}) override;


	private:
		//临时，这个帧要外部引入
		size_t currentFrameIndex = 0;
		VulkanDevice* mDevice;
		VulkanCommandPool* mCommandPool;
		VulkanFencePool* mFencePool;
		CBInfo mCommandBuffer;
		bool bIsInRenderPass{0};
	};

	class VulkanComputeContext : public RHIComputeContext, public VulkanContextBase{
	public:
		VulkanComputeContext(VulkanDevice* device, VulkanFencePool* fecePool, VulkanCommandPool* cmdPool);

		~VulkanComputeContext();

		virtual void RHIBegin() override;

		virtual void RHIEnd() override;

		virtual void RHISetComputePipelineState(RHIPipeline* pipeline) override;

		virtual bool RHIDispatch(unsigned int groupCountX, unsigned int groupCountY, unsigned int groupCountZ) override;

		virtual CBInfo getCommandBuffer() override { return mCommandBuffer; }

		virtual VulkanCommandPool* getCommandPool() override { return mCommandPool; }


	private:
		VulkanDevice* mDevice;
		VulkanCommandPool* mCommandPool;
		VulkanFencePool* mFencePool;
		CBInfo mCommandBuffer;
	};

	class VulkanTransferContext : public RHITransferContext, public VulkanContextBase {
	public:
		VulkanTransferContext(VulkanDevice* device, VulkanFencePool* fencePool, VulkanCommandPool* cmdPool);
		
		~VulkanTransferContext();

		virtual void RHIBegin() override;

		virtual void RHIEnd() override;

		virtual bool RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset = 0, uint64_t dstOffset = 0) override;

		virtual void RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions) override;

		virtual void RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) override;

		virtual CBInfo getCommandBuffer() override { return mCommandBuffer; }

		virtual VulkanCommandPool* getCommandPool() override { return mCommandPool; }


	private:
		VulkanDevice* mDevice;
		VulkanCommandPool* mCommandPool;
		VulkanFencePool* mFencePool;
		CBInfo mCommandBuffer;

	};
}
