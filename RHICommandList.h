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
#include "LockFreeQue.h"
#include <iostream>

namespace FISIR {
	class RHICommandListBase;



	enum class RHICommandT {
		End = 0,
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


	enum RHICommandFlag {
		//Log Flag
		CommandNone = 0x00000000,
		CommandLogDebug = 0x00000001,
		CommandLogInfo = 0x00000002,
		CommandLogWarn = 0x00000004,
		CommandLogError = 0x00000008,

		//Erro Flag
		CommandOutOfPageData = 0x00000010,
	};

	using RHICommandFlags = uint64_t;

	struct BeginRenderPass_CmdInfo {
		RHICommandFlags reserveData{ 0 };
		RHIFrameBuffer* frame; 
		ClearValue value;

	};

	struct DrawPrimitive_CmdInfo {
		RHICommandFlags Flags{ 0 };
		uint32_t BaseVertexIndex; 
		uint32_t NumsPrimitives; 
		uint32_t NumInstances;

	};

	struct DrawIndex_CmdInfo {
		RHICommandFlags Flags{ 0 };
		uint32_t BaseVertexIndex;
		uint32_t IndexCount;
		uint32_t BaseInstanceIndex;
		uint32_t InstanceCount;

	};

	struct BindViewPort_CmdInfo {
		RHICommandFlags Flags{ 0 };
		float x; 
		float y; 
		float width; 
		float height; 
		float maxDepth; 
		float minDepth;

	};

	struct BindScissor_CmdInfo {
		RHICommandFlags Flags{ 0 };
		uint32_t width;
		uint32_t height;

	};
	
	struct BindVertextBuffer_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIBuffer* buffer; 
		uint32_t binding; 
		uint64_t offset;

	};

	struct BindIndexBuffer_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIBuffer* buffer;
		uint64_t offset;

	};

	struct BindResourcePack_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIResourcePackResult Pack;
	};

	struct CopyBufferToBuffer_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIBuffer* src;
		RHIBuffer* dst;
		uint64_t srcOffset;
		uint64_t dstOffset;
		uint64_t size;

	};

	struct CopyBufferToTexture_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIBuffer* src;
		RHITexture* dst;
		uint32_t mipLevel;
		uint32_t arrayindex;
		uint32_t arraycount;
		uint64_t srcOffset;
		TextureSize dstOffset{};
		TextureSize dstSize;

	};

	struct BufferTransition_CmdInfo {
		RHICommandFlags Flags{ 0 };
		class RHIBuffer** buffer;
		size_t count;
		ResourceAccess waitForAccessDone;
		ResourceAccess beginAccessWhenDone;
		RHIUsingStage waitForStageDone;
		RHIUsingStage beginStageWhenDone;

	};

	struct TextureTransition_CmdInfo {
		RHICommandFlags Flags{ 0 };
		class RHITexture** texture;
		size_t count;
		ResourceAccess waitForAccessDone{ ResourceAccess::Undefined };
		ResourceAccess beginAccessWhenDone;
		TextureLayout oldLayout{ TextureLayout::Undefined };
		TextureLayout newLayout;
		RHIUsingStage waitForStageDone;
		RHIUsingStage beginStageWhenDone;


	};
	
	struct Dispatch_CmdInfo {
		RHICommandFlags Flags{ 0 };
		uint32_t groupCountX;
		uint32_t groupCountY;
		uint32_t groupCountZ;

	};

	struct BindPipeline_CmdInfo { 
		RHICommandFlags Flags{ 0 };
		RHIPipeline* pipeline;
	};

	struct ReserveInput_CmdInfo {
		RHICommandFlags Flags{0};
	};

	constexpr size_t MaxCMDPoolSize = 16 * 1024 * 1024;
	constexpr size_t MaxCMDPageSize = 64 * 1024;
	constexpr size_t CmdPageMask = MaxCMDPageSize - 1;
	constexpr size_t PageReverseSize = sizeof(RHICommandT) + sizeof(ReserveInput_CmdInfo);

	struct RingCommandPool{

		using PageFlags = uint32_t;

		RingCommandPool() {
			Buffer = (uint8_t*)malloc(MaxCMDPoolSize);
			for (size_t i=0; i<256; i++) {
				Pages[i].Pool = this;
				Pages[i].CommandPoolPtr = (Buffer + i * MaxCMDPageSize);
				Pages[i].cmdtype = cmdType;
			}
		}

		~RingCommandPool() {
			free(Buffer);
		}


		struct Page {
			struct BatchInfo {
				Page* page {nullptr};
				size_t ReadBegin {0};
				size_t ReadEnd {0};
				uint16_t order{ 0 };

				RHICommandT getCommandType() const {
					return page->GetCommandType(ReadBegin);
				}

				template<class T>
				bool getBatchData(T& data) {
					if (ReadBegin >= ReadEnd) return false;
					data = *(T*)(page->CommandPoolPtr + sizeof(RHICommandT) + (ReadBegin & CmdPageMask));
					ReadBegin += sizeof(RHICommandT) + sizeof(T);
					return true;
				}
			};


			RingCommandPool* Pool;
			uint8_t* CommandPoolPtr {nullptr};
			std::atomic_size_t  Write {0};
			std::atomic_size_t	PageCurrentSize {MaxCMDPageSize};
			CmdType cmdtype{ CmdType::None };
			size_t currentReadBegin {0};
			uint16_t currentBatchCommandCount{ 0 };
			uint16_t currentOrder {0};
			bool IsInRenderPass{ false };

			LockFreeQue<BatchInfo> BatchQueue;

			template<class T>
			void WriteData(RHICommandT commandT, const T& data) {
				if ((!IsInRenderPass && commandT == RHICommandT::EndRenderPass) || (IsInRenderPass && commandT == RHICommandT::BeginRenderPass)) return;
				
				if (currentBatchCommandCount && (commandT == RHICommandT::BeginRenderPass || commandT == RHICommandT::End)) {
					BatchInfo batchInfo{ this, currentReadBegin, Write.load(std::memory_order_acquire), currentOrder++ };
					BatchQueue.push(batchInfo);
					currentBatchCommandCount = 0;
					currentReadBegin = Write.load(std::memory_order_acquire);
				}
				
				if (commandT == RHICommandT::BeginRenderPass) IsInRenderPass = true;
				
				currentBatchCommandCount++;
				size_t sizeofEnumT = sizeof(RHICommandT);
				size_t sizeofData = sizeof(T);
				size_t sizeofCMD = sizeofEnumT + sizeofData;
				size_t currentSize = PageCurrentSize.load(std::memory_order_acquire);
				if (currentSize - sizeofCMD < PageReverseSize || (currentSize - sizeofCMD == PageReverseSize && commandT != RHICommandT::End)) {
					ReserveInput_CmdInfo endInfo {CommandOutOfPageData};
					WriteData(RHICommandT::End, endInfo);
					return;
				}

				size_t CW = Write.load(std::memory_order_acquire);
				size_t WriteInMask = CW & CmdPageMask;

				if (MaxCMDPageSize - WriteInMask >= sizeofCMD) {
					(RHICommandT*)(CommandPoolPtr + WriteInMask) = commandT;
					memcpy(CommandPoolPtr + WriteInMask + sizeofEnumT, &data, sizeofData);			
				}
				else if (MaxCMDPageSize - WriteInMask >= sizeofEnumT) {
					(RHICommandT*)(CommandPoolPtr + WriteInMask) = commandT;
					size_t reserveSize = MaxCMDPageSize - WriteInMask - sizeofEnumT;
					memcpy(CommandPoolPtr + WriteInMask + sizeofEnumT, &data, reserveSize);

					size_t secondPackSize = sizeofData - reserveSize;
					memcpy(CommandPoolPtr, (uint8_t*)&data + reserveSize, secondPackSize);

				}
				else {
					size_t skipSize = MaxCMDPageSize - WriteInMask;
					Write.fetch_add(skipSize, std::memory_order_acq_rel);
					WriteData(commandT, data);
				}
					
				Write.fetch_add(sizeofCMD, std::memory_order_release);
				PageCurrentSize.fetch_sub(sizeofCMD, std::memory_order_release);
				
				if (commandT == RHICommandT::EndRenderPass) {
					IsInRenderPass = false;
					BatchInfo batchInfo{ this, currentReadBegin, Write.load(std::memory_order_acquire), currentOrder++ };
					BatchQueue.push(batchInfo);
					currentBatchCommandCount = 0;
					currentReadBegin = Write.load(std::memory_order_acquire);
				}
			}

			RHICommandT GetCommandType(size_t ReadBegin) const{
				size_t ReadInMask = ReadBegin & CmdPageMask;
				return *(RHICommandT*)(CommandPoolPtr + ReadInMask);
			}

		};



		Page* acquireQue() {
			
			if (FreePages.empty()) return nullptr;
			
			size_t index = 256;
			if (!FreePages.pop(index)) return nullptr;

			return &Pages[index];
		}

		void recycleQue(Page* page) {
			size_t index = (page->CommandPoolPtr - Buffer) / MaxCMDPageSize;
			page->Write.store(0, std::memory_order_release);
			page->PageCurrentSize.store(MaxCMDPageSize, std::memory_order_release);
			FreePages.push(index);
		}


		uint8_t* Buffer{ nullptr };
		CmdType cmdType;
		std::array<Page, 256> Pages;
		LockFreeQue<size_t> FreePages;
	};
	


	class RHICommandListBase {
	public:
		RHICommandListBase(DynamicRHI* rhi): usingRHI(rhi) {
			usingPage = rhi->RHIGetCommandPoolPage(CmdType::None);
		}

		~RHICommandListBase() {
		}

		virtual CmdType getCommandListType() const {return CmdType::None;}

		RingCommandPool::Page* usingPage;
		DynamicRHI* usingRHI;
	};

	class RHIComputeCommandList : public RHICommandListBase {
	public:
		RHIComputeCommandList(DynamicRHI* rhi) : RHICommandListBase(rhi) {
			usingPage = rhi->RHIGetCommandPoolPage(CmdType::Compute);
		}
		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ) {
			Dispatch_CmdInfo info{ GroupCountX, GroupCountY, GroupCountZ };
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

			BufferTransition_CmdInfo info {0, buffer, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone};
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
		}

		void TransitionTextures(
			class RHITexture** texture,size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			TextureLayout oldLayout, TextureLayout newLayout,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone
		
		) {
			TextureTransition_CmdInfo info {0, texture, count, waitForAccessDone, beginAccessWhenDone, oldLayout, newLayout, waitForStageDone, beginStageWhenDone};
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
		}

		CmdType getCommandListType() const {return CmdType::Render;}

		void BeginRenderPass(RHIFrameBuffer* frame, const ClearValue& value) {
			BeginRenderPass_CmdInfo info {0, frame, value};
			usingPage->WriteData(RHICommandT::BeginRenderPass, info);
		}

		void EndRenderPass() {
			ReserveInput_CmdInfo info;
			usingPage->WriteData(RHICommandT::EndRenderPass, info);
		}

		void SetViewPort(float x, float y, float width, float height, float maxDepth, float minDepth) {
			BindViewPort_CmdInfo info {x, y, width, height, maxDepth, minDepth};
			usingPage->WriteData(RHICommandT::BindViewPort, info);
		}

		void SetScissor(uint32_t width, uint32_t height) {
			BindScissor_CmdInfo info {width, height};
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
			DrawPrimitive_CmdInfo info {BaseVertexIndex, NumsPrimitives, NumInstances};
			usingPage->WriteData(RHICommandT::DrawPrimitive, info);
		}


		void DrawIndex(unsigned int BaseVerterIndex, unsigned int IndexCount, unsigned int BaseInstanceIndex, unsigned int InsatnceCount) {
			DrawIndex_CmdInfo info {BaseVerterIndex, IndexCount, BaseInstanceIndex, InsatnceCount};
			usingPage->WriteData(RHICommandT::DrawIndex, info);
		}

		void TransitionBuffers(
			class RHIBuffer** buffer, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone
		) {

			BufferTransition_CmdInfo info{ 0, buffer, count, waitForAccessDone, beginAccessWhenDone, waitForStageDone, beginStageWhenDone };
			usingPage->WriteData(RHICommandT::TransferBuffer, info);
		}

		void TransitionTextures(
			class RHITexture** texture, size_t count,
			ResourceAccess waitForAccessDone, ResourceAccess beginAccessWhenDone,
			TextureLayout oldLayout, TextureLayout newLayout,
			RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone

		) {
			TextureTransition_CmdInfo info{ 0, texture, count, waitForAccessDone, beginAccessWhenDone, oldLayout, newLayout, waitForStageDone, beginStageWhenDone };
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