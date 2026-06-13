#pragma once
#include <cstdint>
#include <initializer_list>
#include "RHITypes.h"

#include "RHIRenderPass.h"
#include "RHIPipeline.h"
namespace FISIR {
	class RHIBuffer;
	class RHIViewport;
	class RHITexture;
	class RHIRenderPass;
	class RHIPipeline;
	class RHIResourcePack;
	class RHIFrameBuffer;


	class RHIContext {
	public:
		virtual ~RHIContext() {}

		virtual void RHIBeginSub() {};

		virtual void RHIEndSub() {};

		virtual void RHIBegin()  = 0;

		virtual void RHIEnd() = 0;

		virtual void RHIExecuteSubCommand() {};


		virtual CmdType getCmdType() const {return CmdType::None;}
	};

	class RHIRenderContext  : public RHIContext {
	public:

		virtual void RHIBeginDrawingViewport(RHIViewport* viewport, RHITexture* rhiTexture) = 0;

		virtual void RHIEndDrawingViewport(RHIRenderPass* pass) = 0;

		virtual void RHIBeginRenderPass(RHIFrameBuffer* frame) = 0;

		virtual void RHIEndRenderPass() = 0;

		virtual void RHISetGraphicsPipelineState(RHIPipeline* pipeline) = 0;

		virtual void RHIDrawPrimitive(unsigned int BaseVertextIndex, unsigned int NumPrimitives, unsigned int NumInstances) = 0;

		virtual void RHISetViewport(RHIViewport* viewport) = 0;

		virtual void RHISetScissor(uint32_t width, uint32_t height) = 0;

		virtual void RHISetDepthBias(float bias) = 0;

		virtual void RHIBindResourcePack(RHIResourcePack* pack) = 0;
		
		virtual void RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) = 0;

		virtual void RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) = 0;

		virtual void RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset = 0, uint64_t dstOffset = 0) = 0;

		virtual void RHICopyTexture(RHIBuffer* dst, RHITexture* src, TextureSize size, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcOffset = 0, TextureSize dstOffset = {0,0,0}) = 0;

		virtual CmdType getCmdType() const override {return CmdType::Render;}
	};

	class RHIComputeContext : public RHIContext {
	public:
		virtual void RHISetComputePipelineState(RHIPipeline* pipeline) = 0;

		virtual bool RHIDispatch(unsigned int groupCountX, unsigned int groupCountY, unsigned int groupCountZ) = 0;
	
		virtual CmdType getCmdType() const override { return CmdType::Compute; }

	};

	class RHITransferContext : public RHIContext {
	public:
		virtual bool RHICopyBuffer(RHIBuffer* srcBuffer, RHIBuffer* dstBuffer, uint64_t size, uint64_t srcOffset = 0, uint64_t dstOffset = 0) = 0;
	
		virtual void RHITransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions) = 0;

		virtual void RHITransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) = 0;

		virtual CmdType getCmdType() const override { return CmdType::Transfer; }

	};

}
