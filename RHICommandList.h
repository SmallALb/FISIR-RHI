#pragma once

#include <iostream>
#include <cstring>
#include <type_traits>

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
#include "RHICommands.h"

namespace FISIR {

	class RHICommandListBase {
	public:
		RHICommandListBase(DynamicRHI* rhi): usingRHI(rhi) {
		}

		~RHICommandListBase() {
		}

		virtual CmdType getCommandListType() const {return CmdType::None;}

		// Present：本页提交给 RHI 后，由提交线程（RHI 线程）在本页 vkQueueSubmit 之后执行
		//（见 VulkanRHI 的 presents 落地）。**必须写在 End() 之前** —— Present 落在最后一个
		// 批次里，随该页一起被采集；写在 End() 之后就落在页尾哨兵之外，永远不会被读到。
		void Present(class RHISwapChain* swapchain, uint32_t frameID) {
			Present_CmdInfo info{};
			info.swapchain = swapchain;
			info.frameID   = frameID;
			usingPage->WriteData(RHICommandT::Present, info);
		}

		void End(RHIFence* fence = nullptr,
		         const std::vector<RHISemaphore*>& waits = {},
		         const std::vector<RHISemaphore*>& toSignals = {}
		         ) {
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
			End_CmdInfo info {0, fence, copyDataW, static_cast<uint32_t>(waits.size()), copyDataS, static_cast<uint32_t>(toSignals.size())};
			usingPage->WriteData(RHICommandT::End, info);
		}

		RingCommandPool::PageFlag getPageFlag() const {
			if (usingPage) return usingPage->flags.load(std::memory_order_acquire);
			else return RingCommandPool::PageFlag::None;
		}

		// 向**当前已绑定**的管线推送常量（图形/计算命令列表都可用，放在基类避免重复）。
		// 数据内联进命令，所以调用方传完即可放手；超过 64 字节的部分会被丢弃并告警。
		void PushConstant(const void* data, uint32_t size,
		                  RHIUsingStageFlags stage, uint32_t offset = 0) {
			if (!data || size == 0) return;
			if (size > RHI_PUSH_CONSTANT_MAX_BYTES) {
				Warn("PushConstant: requested {} bytes > limit {}, truncated", size, RHI_PUSH_CONSTANT_MAX_BYTES);
				size = RHI_PUSH_CONSTANT_MAX_BYTES;
			}
			PushConstant_CmdInfo info{};
			info.offset = offset;
			info.size = size;
			info.usingStage = stage;
			memcpy(info.data, data, size);
			usingPage->WriteData(RHICommandT::PushConstant, info);
		}

		// 模板便利重载：PushConstant(某个 POD)，自动取 sizeof 与首地址
		template<typename T>
		void PushConstant(const T& value, RHIUsingStageFlags stage, uint32_t offset = 0) {
			static_assert(std::is_trivially_copyable_v<T>, "PushConstant accepts only trivially copyable PODs");
			PushConstant(&value, (uint32_t)sizeof(T), stage, offset);
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

		void SetResourcePack(const RHIResourcePackResult& pack) {
			BindResourcePack_CmdInfo info{ 0, pack };
			usingPage->WriteData(RHICommandT::BindResourceAndSamplerPack, info);
		}

		// 跨队列资源所有权转移（release/acquire）。compute 写 buffer 后、graphics 读之前，
		// 需要在本队列（compute）释放所有权并在对端（render）接管，否则异队列族读取会挂起。
		void TransitionBuffers(
			class RHIBuffer** buffer, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone,
			CmdType ResourceQueue = CmdType::None, bool ResourceIsTransferOut = false
		) {
			RHIBuffer** copyData = (RHIBuffer**)malloc(count * sizeof(RHIBuffer*));
			memcpy(copyData, buffer, count * sizeof(RHIBuffer*));
			BufferTransition_CmdInfo info {0, copyData, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone, ResourceQueue, ResourceIsTransferOut};
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
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
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone,
			CmdType ResourceQueue = CmdType::None, bool ResourceIsTransferOut = false
		) {
			RHIBuffer** copyData = (RHIBuffer**)malloc(count * sizeof(RHIBuffer*));
			memcpy(copyData, buffer, count * sizeof(RHIBuffer*));
			BufferTransition_CmdInfo info {0, copyData, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone, ResourceQueue, ResourceIsTransferOut};
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
		}

		void TransitionTextures(
			class RHITexture** texture,size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			TextureLayout oldLayout, TextureLayout newLayout,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone,
			CmdType ResourceQueue = CmdType::None, bool ResourceIsTransferOut = false

		) {
			RHITexture** copyData = (RHITexture**)malloc(count * sizeof(RHITexture*));
			memcpy(copyData, texture, count * sizeof(RHITexture*));
			TextureTransition_CmdInfo info {0, copyData, count, waitForAccessDone, beginAccessWhenDone, oldLayout, newLayout, waitForStageDone, beginStageWhenDone, ResourceQueue, ResourceIsTransferOut};
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

		void CopyImageToBuffer(RHITexture* src, RHIBuffer* dst, uint32_t mipLevel, uint32_t arrayindex, uint32_t arraycount, TextureSize srcOffset, uint64_t dstOffset, TextureSize size) {
			CopyImageToBuffer_CmdInfo info{0, src, dst, mipLevel, arrayindex, arraycount, srcOffset, dstOffset, size };
			usingPage->WriteData(RHICommandT::CopyImageToBuffer, info);
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
			BindScissor_CmdInfo info {0, 0, 0, width, height};
			usingPage->WriteData(RHICommandT::BindScissor, info);
		}

		// 带左上角偏移的裁剪矩形。ImGui 的每个 DrawCmd 都带自己的 ClipRect，
		// 只给 w/h 的 SetScissor 无法表达（子窗口/滚动区会溢出绘制）。
		void SetScissorRect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
			BindScissor_CmdInfo info {0, x, y, width, height};
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

		void DrawIndirect(RHIBuffer* indirectBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride) {
			DrawIndirect_CmdInfo info{0, indirectBuffer, offset, drawCount, stride};
			usingPage->WriteData(RHICommandT::DrawIndirect, info);
		}

		void DrawIndexedIndirect(RHIBuffer* indirectBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride) {
			DrawIndexedIndirect_CmdInfo info{0, indirectBuffer, offset, drawCount, stride};
			usingPage->WriteData(RHICommandT::DrawIndexedIndirect, info);
		}

		void TransitionBuffers(
			class RHIBuffer** buffer, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone,
			CmdType ResourceQueue = CmdType::None, bool ResourceIsTransferOut = false
		) {
			RHIBuffer** copyData = (RHIBuffer**)malloc(count * sizeof(RHIBuffer*));
			memcpy(copyData, buffer, count * sizeof(RHIBuffer*));
			BufferTransition_CmdInfo info{ 0, copyData, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone, ResourceQueue, ResourceIsTransferOut };
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
		}

		void TransitionTextures(
			class RHITexture** texture, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			TextureLayout oldLayout, TextureLayout newLayout,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone,
			CmdType ResourceQueue = CmdType::None, bool ResourceIsTransferOut = false

		) {
			RHITexture** copyData = (RHITexture**)malloc(count * sizeof(RHITexture*));
			memcpy(copyData, texture, count * sizeof(RHITexture*));
			TextureTransition_CmdInfo info{ 0, copyData, count, waitForAccessDone, beginAccessWhenDone, oldLayout, newLayout, waitForStageDone, beginStageWhenDone, ResourceQueue, ResourceIsTransferOut };
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

		void CopyImageToBuffer(RHITexture* src, RHIBuffer* dst, uint32_t mipLevel, uint32_t arrayindex, uint32_t arraycount, TextureSize srcOffset, uint64_t dstOffset, TextureSize size) {
			CopyImageToBuffer_CmdInfo info {0, src, dst, mipLevel, arrayindex, arraycount, srcOffset, dstOffset, size};
			usingPage->WriteData(RHICommandT::CopyImageToBuffer, info);
		}

		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ) {
			Dispatch_CmdInfo info{ 0, GroupCountX, GroupCountY, GroupCountZ };
			usingPage->WriteData(RHICommandT::Dispatch, info);
		}


	};

}