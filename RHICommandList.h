#pragma once

#include "RHIPipeline.h"         
#include "RHIResourcePack.h"     
#include "RHIRenderPass.h"       
#include "RHITexture.h"        
#include "RHISwapChain.h"
#include "RHIViewport.h"
#include "RHIBuffer.h"           
#include "DynamicRHI.h"          
#include "../Log/Logger.h"
#include <iostream>

namespace FISIR {
	class RHICommandListBase;


	constexpr size_t MaxStackSize = 16 * 1024 * 1024;
	constexpr size_t Mask = MaxStackSize - 1;


	enum class RHICommandT {
		None = 0,
		//render
		BeginRenderPass,
		EndRenderPass,
		DrawPrimitive,
		DrawIndex,
		//Bind
		BindPipeline,
		BindVertexBuffer,
		BindIndexBuffer,
		BindResourceAndSamplerPack,
		BindViewPort,
		BindScissor,
		//Transfer
		TransferTexture,
		TransferBuffer,
		CopyBufferToBuffer,
		CopyBufferToTexture,
		//Compute
		Dispatch

	};

	struct BeginRenderPass_CmdInfo {
		RHIFrameBuffer* frame; 
		ClearValue value;
	};

	struct DrawPrimitive_CmdInfo {
		uint32_t BaseVertexIndex; 
		uint32_t NumsPrimitives; 
		uint32_t NumInstances;
	};

	struct DrawIndex_CmdInfo {
		uint32_t BaseVerterIndex;
		uint32_t IndexCount;
		uint32_t BaseInstanceIndex;
		uint32_t InsatnceCount;
	};

	struct BindViewPort_CmdInfo {
		float x; 
		float y; 
		float width; 
		float height; 
		float maxDepth; 
		float minDepth;
	};

	struct BindScissor_CmdInfo {
		uint32_t width;
		uint32_t height;
	};
	
	struct BindVertextBuffer_CmdInfo {
		RHIBuffer* buffer; 
		uint32_t binding; 
		uint64_t offset;
	};

	struct BindIndexBuffer_CmdInfo {
		RHIBuffer* buffer;
		uint64_t offset;
	};

	struct CopyBufferToBuffer_CmdInfo {
		RHIBuffer* src;
		RHIBuffer* dst;
		uint64_t srcOffset;
		uint64_t dstOffset;
		uint64_t size;
	};

	struct CopyBufferToTexture_CmdInfo {
		RHIBuffer* src;
		RHITexture* dst;
		uint32_t mipLevel;
		uint32_t arrayindex;
		uint32_t arraycount;
		uint64_t srcOffset;
		TextureSize dstOffset{};
		TextureSize dstSize;
	};
	

	struct Dispatch_CmdInfo {
		uint32_t GroupCountX; 
		uint32_t GroupCountY;
		uint32_t GroupCountZ;
	};


	struct BindPipeline_CmdInfo {
		RHIPipeline* pipeline;
	};

	struct ReserveInput_CmdInfo {
		uint64_t reserveData {0};
	};

	struct RingCommandStack {
		uint8_t* Stack{nullptr};
		std::atomic_size_t WriteHead{0};
		std::atomic_size_t ReadHead{0};


		struct CommandBinaryRange {
			uint8_t* CommandBegin;
			uint8_t* CommandEnd;

			bool isNeedSegment() const {
				return CommandEnd < CommandBegin;
			}
		};

		RingCommandStack() {
			Stack = (uint8_t*)malloc(MaxStackSize);
		}

		~RingCommandStack() {
			free(Stack);
		}

		template<class T>
		void WriteData(RHICommandT commandT, const T& data) {
			size_t inputDataSize = sizeof(T);
			size_t totalSize = sizeof(RHICommandT) + inputDataSize;
			size_t startOffset = WriteHead.fetch_add(totalSize, std::memory_order_acq_rel);
			size_t currentReadHead = ReadHead.load(std::memory_order_acquire);
			while (stratOffset + totalSize - currentReadHead > MaxStackSize) {
				currentReadHead = ReadHead.load(std::memory_order_acquire);
				std::this_thread::yield();
			}

			size_t physicalOffset = startOffset & Mask;

			*(RHICommandT*)(Stack + physicalOffset) = commandT;
			size_t dataOffset = physicalOffset + sizeof(RHICommandT);
			if (dataOffset + inputDataSize <= MaxStackSize) memcpy(Stack + dataOffset, &data, inputDataSize);
			else {
				size_t firstPack = MaxStackSize - dataOffset;
				memcpy(Stack+ dataOffset, &data, firstPack);
				memcpy(Stack, (uint8_t*)&data+firstPack, inputDataSize - firstPack);
			}
		}

		CommandBinaryRange popCommandRange() {
			size_t localReadHead = ReadHead.load(std::memory_order_acquire);
			size_t writeHead = WriteHead.load(std::memory_order_acquire);
			
			if (localReadHead >= writeHead) return { nullptr, nullptr };
			

			size_t Batch = writeHead - localReadHead;
			size_t End = localReadHead + Batch;

			if (ReadHead.compare_exchange_weak(localReadHead, End, std::memory_order_acq_rel)) return { Stack + (localReadHead & Mask) , Stack + (End & Mask) };
			
			return {nullptr, nullptr};
		}
	};
	


	class RHICommandListBase {
	public:
		RHICommandListBase(RingCommandStack & stack): usingStack(stack) {}

		~RHICommandListBase() {}

		virtual CmdType getCommandListType() const {return CmdType::None;}

		RingCommandStack& usingStack;
	};

	class RHIComputeCommandList : public RHICommandListBase {
	public:
		RHIComputeCommandList(RingCommandStack& stack) : RHICommandListBase(stack) {}
		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ) {
			Dispatch_CmdInfo info {GroupCountX, GroupCountY, GroupCountZ};
			usingStack.WriteData(RHICommandT::Dispatch, info);
		}

		void SetPipelineState(RHIPipeline* pipeline) {
			BindPipeline_CmdInfo info {pipeline};
			usingStack.WriteData(RHICommandT::BindPipeline, info);
		}

		CmdType getCommandListType() const { return CmdType::Compute; }
	};

	class RHITransferCommandList : public RHICommandListBase {
	public:
		RHITransferCommandList(RingCommandStack& stack) : RHICommandListBase(stack) {}

		void TransitionBuffers(const BufferTransitionInfo& bufferTransitions) {
			usingStack.WriteData(RHICommandT::TransferBuffer, bufferTransitions);
		}

		void TransitionTextures(const TextureTransitionInfo& textureTransitions) {
			usingStack.WriteData(RHICommandT::TransferTexture, textureTransitions);
		}

		void CopyToBuffer(RHIBuffer* src, RHIBuffer* dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
			CopyBufferToBuffer_CmdInfo info{ src, dst, srcOffset, dstOffset, size };
			usingStack.WriteData(RHICommandT::CopyBufferToBuffer, info);
		}

		void CopyToTexture(RHIBuffer* src, RHITexture* dst, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcoffset, TextureSize dstOffset, TextureSize size) {
			CopyBufferToTexture_CmdInfo info{ src, dst, miplevel, arrayindex, arraycount, srcoffset, dstOffset, size };
			usingStack.WriteData(RHICommandT::CopyBufferToTexture, info);
		}


		CmdType getCommandListType() const { return CmdType::Transfer; }
	};

	class RHIRenderCommandList : public RHICommandListBase {
	public:
		RHIRenderCommandList(RingCommandStack& stack) : RHICommandListBase(stack) {}

		CmdType getCommandListType() const {return CmdType::Render;}

		void BeginRenderPass(RHIFrameBuffer* frame, const ClearValue& value) {
			BeginRenderPass_CmdInfo info {frame, value};
			usingStack.WriteData(RHICommandT::BeginRenderPass, info);
		}

		void EndRenderPass() {
			ReserveInput_CmdInfo info;
			usingStack.WriteData(RHICommandT::EndRenderPass, info);
		}

		void SetViewPort(float x, float y, float width, float height, float maxDepth, float minDepth) {
			BindViewPort_CmdInfo info {x, y, width, height, maxDepth, minDepth};
			usingStack.WriteData(RHICommandT::BindViewPort, info);
		}

		void SetScissor(uint32_t width, uint32_t height) {
			BindScissor_CmdInfo info {width, height};
			usingStack.WriteData(RHICommandT::BindScissor, info);
		}

		void SetPipelineState(RHIPipeline* pipeline) {
			BindPipeline_CmdInfo info{ pipeline };
			usingStack.WriteData(RHICommandT::BindPipeline, info);
		}

		void SetResourcePack(const RHIResourcePackResult& pack) {
			usingStack.WriteData(RHICommandT::BindResourceAndSamplerPack, pack);
		}

		void SetVertexBuffer(RHIBuffer* buffer, uint32_t binding, uint64_t offset) {
			BindVertextBuffer_CmdInfo info {buffer, binding, offset};
			usingStack.WriteData(RHICommandT::BindVertexBuffer, info);
		}

		void SetIndexBuffer(RHIBuffer* buffer, uint64_t offset) {
			BindIndexBuffer_CmdInfo info {buffer, offset};
			usingStack.WriteData(RHICommandT::BindIndexBuffer, info);
		}

		void DrawPrimitive(uint32_t BaseVertexIndex, uint32_t NumsPrimitives, uint32_t NumInstances) {
			DrawPrimitive_CmdInfo info {BaseVertexIndex, NumsPrimitives, NumInstances};
			usingStack.WriteData(RHICommandT::DrawPrimitive, info);
		}


		void DrawIndex(unsigned int BaseVerterIndex, unsigned int IndexCount, unsigned int BaseInstanceIndex, unsigned int InsatnceCount) {
			DrawIndex_CmdInfo info {BaseVerterIndex, IndexCount, BaseInstanceIndex, InsatnceCount};
			usingStack.WriteData(RHICommandT::DrawIndex, info);
		}

		void TransitionBuffers(const BufferTransitionInfo& bufferTransitions) {
			usingStack.WriteData(RHICommandT::TransferBuffer, bufferTransitions);
		}

		void TransitionTextures(const TextureTransitionInfo& textureTransitions) {
			usingStack.WriteData(RHICommandT::TransferTexture, textureTransitions);

		}

		void CopyToBuffer(RHIBuffer* src, RHIBuffer* dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
			CopyBufferToBuffer_CmdInfo info {src, dst, srcOffset, dstOffset, size};
			usingStack.WriteData(RHICommandT::CopyBufferToBuffer, info);
		}

		void CopyToTexture(RHIBuffer* src, RHITexture* dst, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcoffset, TextureSize dstOffset, TextureSize size) {
			CopyBufferToTexture_CmdInfo info {src, dst, miplevel, arrayindex, arraycount, srcoffset, dstOffset, size};
			usingStack.WriteData(RHICommandT::CopyBufferToTexture, info);
		}


	};

	class RHICommandListImmediate : public RHICommandListBase {
	public:

	};
	
}