#pragma once

#include <iostream>

#include "DynamicRHI.h"
#include "LockFreeQue.h"
#include "Log/Logger.h"
#include "RHIBuffer.h"
#include "RHIPipeline.h"
#include "RHIRenderPass.h"
#include "RHIResourcePack.h"
#include "RHISwapChain.h"
#include "RHITexture.h"
#include "RHIViewport.h"

namespace FISIR {
	class RHICommandListBase;




	class RHICommandListBase {
	public:
		RHICommandListBase(DynamicRHI* rhi): usingRHI(rhi) {
		}

		~RHICommandListBase() {
		}

		virtual CmdType getCommandListType() const {return CmdType::None;}

		void End(RHIFence* fence = nullptr, const std::vector<RHISemaphore*>& waits = {}, const std::vector<RHISemaphore*>& toSignals = {}, RHISwapChain* swapchain = nullptr , uint32_t swapChainID = UINT32_MAX ) {
			RHISemaphore** copyDataW;
			if (waits.size()) {
				copyDataW = (RHISemaphore**)malloc(waits.size() * sizeof(RHISemaphore*));
				memcpy(copyDataW, waits.data(), waits.size() * sizeof(RHISemaphore*));
			}
			else copyDataW = nullptr;

			RHISemaphore** copyDataS;
			if (toSignals.size()) {
				copyDataS = (RHISemaphore**)malloc(toSignals.size() * sizeof(RHISemaphore*));
				memcpy(copyDataS, toSignals.data(), toSignals.size() * sizeof(RHISemaphore*));
			}
			else copyDataS = nullptr;
			End_CmdInfo info {0, fence, copyDataW, static_cast<uint32_t>(waits.size()), copyDataS, static_cast<uint32_t>(toSignals.size()), swapchain, swapChainID};
			usingPage->WriteData(RHICommandT::End, info);
		}

		RingCommandPool::PageFlag getPageFlag() const {
			if (usingPage) return usingPage->flags.load(std::memory_order_acquire);
			else return RingCommandPool::PageFlag::None;
		}


		RingCommandPool::Page* usingPage = nullptr;
		DynamicRHI* usingRHI;
	};

	class RHIComputeCommandList : public RHICommandListBase {
	public:
		RHIComputeCommandList(DynamicRHI* rhi) : RHICommandListBase(rhi) {
			usingPage = rhi->RHIGetCommandPoolPage(CmdType::Compute);
		}
		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ) {
			Dispatch_CmdInfo info{0, GroupCountX, GroupCountY, GroupCountZ };
			usingPage->WriteData(RHICommandT::Dispatch, info);
		}

		void SetPipelineState(RHIPipeline* pipeline) {
			BindPipeline_CmdInfo info{ 0, pipeline };
			usingPage->WriteData(RHICommandT::BindPipeline, info);
		}

		CmdType getCommandListType() const { return CmdType::Compute; }
	};

	class RHITransferCommandList : public RHICommandListBase {
	public:
		RHITransferCommandList(DynamicRHI* rhi) : RHICommandListBase(rhi) {
			usingPage = rhi->RHIGetCommandPoolPage(CmdType::Transfer);
		}

		void TransitionBuffers(
			class RHIBuffer** buffer, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone
		) {
			RHIBuffer** copyData = (RHIBuffer**)malloc(count * sizeof(RHIBuffer*));
			memcpy(copyData, buffer, count * sizeof(RHIBuffer*));
			BufferTransition_CmdInfo info {0, copyData, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone};
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
		}

		void TransitionTextures(
			class RHITexture** texture,size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			TextureLayout oldLayout, TextureLayout newLayout,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone

		) {
			RHITexture** copyData = (RHITexture**)malloc(count * sizeof(RHITexture*));
			memcpy(copyData, texture, count * sizeof(RHITexture*));
			TextureTransition_CmdInfo info {0, copyData, count, waitForAccessDone, beginAccessWhenDone, oldLayout, newLayout, waitForStageDone, beginStageWhenDone};
			usingPage->WriteData(RHICommandT::TransferTexture, info);
		}

		void CopyToBuffer(RHIBuffer* src, RHIBuffer* dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
			CopyBufferToBuffer_CmdInfo info{0, src, dst, srcOffset, dstOffset, size };
			usingPage->WriteData(RHICommandT::CopyBufferToBuffer, info);
		}

		void CopyToTexture(RHIBuffer* src, RHITexture* dst, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcoffset, TextureSize dstOffset, TextureSize size) {
			CopyBufferToTexture_CmdInfo info{0, src, dst, miplevel, arrayindex, arraycount, srcoffset, dstOffset, size };
			usingPage->WriteData(RHICommandT::CopyBufferToTexture, info);
		}


		CmdType getCommandListType() const { return CmdType::Transfer; }
	};

	class RHIRenderCommandList : public RHICommandListBase {
	public:
		RHIRenderCommandList(DynamicRHI* rhi) : RHICommandListBase(rhi) {
			usingPage = rhi->RHIGetCommandPoolPage(CmdType::Render);
			if (!usingPage) Error("Page get Failed!");
		}

		CmdType getCommandListType() const {return CmdType::Render;}

		void BeginRenderPass(RHIFrameBuffer* frame, uint32_t subpassIndex, const ClearValue& value) {
			BeginRenderPass_CmdInfo info {0, frame, subpassIndex, value};
			usingPage->WriteData(RHICommandT::BeginRenderPass, info);
		}

		void EndRenderPass() {
			ReserveInput_CmdInfo info;
			usingPage->WriteData(RHICommandT::EndRenderPass, info);
		}

		void SetViewPort(float x, float y, float width, float height, float maxDepth, float minDepth) {
			BindViewPort_CmdInfo info {0, x, y, width, height, maxDepth, minDepth};
			usingPage->WriteData(RHICommandT::BindViewPort, info);
		}

		void SetScissor(uint32_t width, uint32_t height) {
			BindScissor_CmdInfo info {0, width, height};
			usingPage->WriteData(RHICommandT::BindScissor, info);
		}

		void SetPipelineState(RHIPipeline* pipeline) {
			BindPipeline_CmdInfo info{0, pipeline };
			usingPage->WriteData(RHICommandT::BindPipeline, info);
		}

		void SetResourcePack(const RHIResourcePackResult& pack) {
			BindResourcePack_CmdInfo info{ 0, pack };
			usingPage->WriteData(RHICommandT::BindResourceAndSamplerPack, info);
		}

		void SetVertexBuffer(RHIBuffer* buffer, uint32_t binding, uint64_t offset) {
			BindVertextBuffer_CmdInfo info {0, buffer, binding, offset};
			usingPage->WriteData(RHICommandT::BindVertexBuffer, info);
		}

		void SetIndexBuffer(RHIBuffer* buffer, uint64_t offset) {
			BindIndexBuffer_CmdInfo info {0, buffer, offset};
			usingPage->WriteData(RHICommandT::BindIndexBuffer, info);
		}

		void DrawPrimitive(uint32_t BaseVertexIndex, uint32_t NumsPrimitives, uint32_t NumInstances) {
			DrawPrimitive_CmdInfo info {0, BaseVertexIndex, NumsPrimitives, NumInstances};
			usingPage->WriteData(RHICommandT::DrawPrimitive, info);
		}


		void DrawIndex(unsigned int BaseVerterIndex, unsigned int IndexCount, unsigned int BaseInstanceIndex, unsigned int InsatnceCount) {
			DrawIndex_CmdInfo info {0, BaseVerterIndex, IndexCount, BaseInstanceIndex, InsatnceCount};
			usingPage->WriteData(RHICommandT::DrawIndex, info);
		}

		void TransitionBuffers(
			class RHIBuffer** buffer, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone
		) {
			RHIBuffer** copyData = (RHIBuffer**)malloc(count * sizeof(RHIBuffer*));
			memcpy(copyData, buffer, count * sizeof(RHIBuffer*));
			BufferTransition_CmdInfo info{ 0, copyData, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone };
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
		}

		void TransitionTextures(
			class RHITexture** texture, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			TextureLayout oldLayout, TextureLayout newLayout,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone

		) {
			RHITexture** copyData = (RHITexture**)malloc(count * sizeof(RHITexture*));
			memcpy(copyData, texture, count * sizeof(RHITexture*));
			TextureTransition_CmdInfo info{ 0, copyData, count, waitForAccessDone, beginAccessWhenDone, oldLayout, newLayout, waitForStageDone, beginStageWhenDone };
			usingPage->WriteData(RHICommandT::TransferTexture, info);
		}

		void CopyToBuffer(RHIBuffer* src, RHIBuffer* dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
			CopyBufferToBuffer_CmdInfo info {0, src, dst, srcOffset, dstOffset, size};
			usingPage->WriteData(RHICommandT::CopyBufferToBuffer, info);
		}

		void CopyToTexture(RHIBuffer* src, RHITexture* dst, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcoffset, TextureSize dstOffset, TextureSize size) {
			CopyBufferToTexture_CmdInfo info {0, src, dst, miplevel, arrayindex, arraycount, srcoffset, dstOffset, size};
			usingPage->WriteData(RHICommandT::CopyBufferToTexture, info);
		}


	};

	class RHICommandListImmediate : public RHICommandListBase {
	public:

	};

}