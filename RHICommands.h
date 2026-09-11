#pragma once

#include "Log/Logger.h"

namespace FISIR{


	enum class RHICommandT {
		End = 0,
		//render
		BeginRenderPass,
		EndRenderPass,
		DrawPrimitive,
		DrawIndex,
		DrawIndirect,
		DrawIndexedIndirect,
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
		CopyImageToBuffer,
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
		uint32_t subpassIndex;
		ClearValue clearValue;

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

	struct DrawIndirect_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIBuffer* Buffer;
		uint32_t Offset;
		uint32_t DrawCount;
		uint32_t Stride;
	};

	struct DrawIndexedIndirect_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIBuffer* Buffer;
		uint32_t Offset;
		uint32_t DrawCount;
		uint32_t Stride;
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

	struct CopyImageToBuffer_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHITexture* src;
		RHIBuffer* dst;
		uint32_t mipLevel;
		uint32_t arrayindex;
		uint32_t arraycount;
		TextureSize srcOffset{};
		uint64_t dstOffset;
		TextureSize srcSize;

	};

	struct BufferTransition_CmdInfo {
		RHICommandFlags Flags{ 0 };
		class RHIBuffer** buffer;
		size_t count;
		ResourceAccess waitForAccessDone;
		ResourceAccess beginAccessWhenDone;
		RHIUsingStage waitForStageDone;
		RHIUsingStage beginStageWhenDone;
		// 跨队列资源所有权转移：ResourceQueue 为对端队列，ResourceIsTransferOut=true 表示转出（release），
		// false 表示转入（acquire）；ResourceQueue 为 None 时二者无效（同队列转换）。
		CmdType ResourceQueue{ CmdType::None };
		bool ResourceIsTransferOut{ false };

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
		// 跨队列资源所有权转移：ResourceQueue 为对端队列，ResourceIsTransferOut=true 表示转出（release），
		// false 表示转入（acquire）；ResourceQueue 为 None 时二者无效（同队列转换）。
		CmdType ResourceQueue{ CmdType::None };
		bool ResourceIsTransferOut{ false };


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



	struct End_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIFence* fence;
		RHISemaphore** waits;
		uint32_t waitcount{ 0 };
		RHISemaphore** signals;
		uint32_t signalcount{ 0 };
	};


	struct ReserveInput_CmdInfo {
		RHICommandFlags Flags{ 0 };
	};

	constexpr size_t MaxCMDPageSize  = 256 * 1024;                       // 256KB/页：单 render pass 可容纳约 9000 次 drawcall
	constexpr size_t MaxCMDPageCount = 256;
	constexpr size_t MaxCMDPoolSize  = MaxCMDPageCount * MaxCMDPageSize; // 64MB
	constexpr size_t CmdPageMask = MaxCMDPageSize - 1;
	constexpr size_t PageReverseSize = sizeof(RHICommandT) + sizeof(End_CmdInfo);

	struct RingCommandPool {


		RingCommandPool() {
			Buffer = (uint8_t*)malloc(MaxCMDPoolSize);
			for (size_t i = 0; i < MaxCMDPageCount; i++) {
				Pages[i].Pool = this;
				Pages[i].CommandPoolPtr = (Buffer + i * MaxCMDPageSize);
				Pages[i].cmdtype = cmdType;
				FreePages.push(i);
			}
		}

		~RingCommandPool() {
			free(Buffer);
		}


		enum PageFlag {
			None = 0,
			CanRecord = 1,
			IsEnd = 2,
			IsRecording = 0x08,
		};

		struct Page {

			struct BatchInfo {
				Page* page{ nullptr };
				size_t ReadBegin{ 0 };
				size_t ReadEnd{ 0 };
				uint16_t order{ 0 };

				RHICommandT getCommandType() const {
					return page->GetCommandType(ReadBegin);
				}

				template<class T>
				bool getBatchData(T& data) {
					if (ReadBegin + sizeof(RHICommandT) + sizeof(T) > ReadEnd) return false;
					//data = *(T*)(page->CommandPoolPtr + sizeof(RHICommandT) + (ReadBegin & CmdPageMask));
					size_t offset = sizeof(RHICommandT) + (ReadBegin & CmdPageMask);
					size_t dataSize = sizeof(T);
					if (offset + dataSize <= MaxCMDPageSize) {
						memcpy(&data, page->CommandPoolPtr + offset, dataSize);
					}
					else {
						size_t firstPart = MaxCMDPageSize - offset;
						memcpy(&data, page->CommandPoolPtr + offset, firstPart);
						memcpy((uint8_t*)&data + firstPart, page->CommandPoolPtr, dataSize - firstPart);
					}
					ReadBegin += sizeof(RHICommandT) + sizeof(T);
					//if constexpr (std::is_same_v<T, End_CmdInfo>) Debug("Begin is {}", ReadBegin);
					return true;
				}
			};
			 

			RingCommandPool* Pool;
			uint8_t* CommandPoolPtr{ nullptr };
			std::atomic_size_t  Write{ 0 };
			std::atomic_size_t	PageCurrentSize{ MaxCMDPageSize };
			CmdType cmdtype{ CmdType::None };
			size_t currentReadBegin{ 0 };
			uint16_t currentBatchCommandCount{ 0 };
			uint16_t currentOrder{ 0 };
			std::atomic<PageFlag> flags = PageFlag::None;
			bool IsInRenderPass{ false };
			// 全局单调递增的「录制序号」，由 RHI 在分配页面时写入（见 VulkanRHI::RHIGetCommandPoolPage）。
			// RHI 线程按它决定提交顺序：页面之间可以存在信号量依赖（compute 页 signal、render 页
			// wait），而 Vulkan 要求 wait 引用的 signal 必须已被提交，故提交顺序必须等于录制顺序。
			uint64_t recordOrder{ 0 };

			LockFreeQue<BatchInfo> BatchQueue;

			template<class T>
			void WriteData(RHICommandT commandT, const T& data) {
				if ((!IsInRenderPass && commandT == RHICommandT::EndRenderPass) || (IsInRenderPass && commandT == RHICommandT::BeginRenderPass)) return;

				if (currentBatchCommandCount && (commandT == RHICommandT::BeginRenderPass || commandT == RHICommandT::End)) {
					BatchInfo batchInfo{ this, currentReadBegin, Write.load(std::memory_order_acquire), currentOrder++ };
					BatchQueue.push(batchInfo);
					PageFlag expected = None;
					flags.compare_exchange_strong(expected, CanRecord, std::memory_order_relaxed, std::memory_order_acquire);
					currentBatchCommandCount = 0;
					currentReadBegin = Write.load(std::memory_order_acquire);
				}

				if (commandT == RHICommandT::BeginRenderPass) IsInRenderPass = true;

				currentBatchCommandCount++;
				size_t sizeofEnumT = sizeof(RHICommandT);
				size_t sizeofData = sizeof(T);
				size_t sizeofCMD = sizeofEnumT + sizeofData;
				size_t currentSize = PageCurrentSize.load(std::memory_order_acquire);
				if (commandT != RHICommandT::End && currentSize < sizeofCMD + PageReverseSize) {
					End_CmdInfo endInfo{ CommandOutOfPageData };
					WriteData(RHICommandT::End, endInfo);
					return;
				}

				size_t CW = Write.load(std::memory_order_acquire);
				size_t WriteInMask = CW & CmdPageMask;

				if (MaxCMDPageSize - WriteInMask >= sizeofCMD) {
					*((RHICommandT*)(CommandPoolPtr + WriteInMask)) = commandT;
					memcpy(CommandPoolPtr + WriteInMask + sizeofEnumT, &data, sizeofData);
				}
				else if (MaxCMDPageSize - WriteInMask >= sizeofEnumT) {
					*((RHICommandT*)(CommandPoolPtr + WriteInMask)) = commandT;
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
					PageFlag expected = None;
					flags.compare_exchange_strong(expected, CanRecord, std::memory_order_relaxed, std::memory_order_acquire);
					currentReadBegin = Write.load(std::memory_order_acquire);
				}
				else if (commandT == RHICommandT::End) {
					BatchInfo batchInfo{ this, currentReadBegin, Write.load(std::memory_order_acquire), currentOrder++ };
					BatchQueue.push(batchInfo);
					currentBatchCommandCount = 0;
					PageFlag expected = None;
					flags.compare_exchange_strong(expected, CanRecord, std::memory_order_relaxed, std::memory_order_acquire);
					currentReadBegin = Write.load(std::memory_order_acquire);
				}

			}

			RHICommandT GetCommandType(size_t ReadBegin) const {
				size_t ReadInMask = ReadBegin & CmdPageMask;
				return *(RHICommandT*)(CommandPoolPtr + ReadInMask);
			}

		};



		Page* acquireQue() {
			size_t index = MaxCMDPageCount;
			FreePages.pop_wait(index);
			Pages[index].cmdtype = cmdType;
			return &Pages[index];
		}

		void recycleQue(Page* page) {
			size_t index = (page->CommandPoolPtr - Buffer) / MaxCMDPageSize;
			page->Write.store(0, std::memory_order_release);
			page->PageCurrentSize.store(MaxCMDPageSize, std::memory_order_release);
			page->flags.exchange(None, std::memory_order_release);
			page->currentBatchCommandCount = 0;
			page->currentReadBegin = 0;
			page->currentOrder = 0;
			page->IsInRenderPass = false;
			page->BatchQueue.clear();
			FreePages.push(index);
		}


		uint8_t* Buffer{ nullptr };
		CmdType cmdType;
		std::array<Page, MaxCMDPageCount> Pages;
		LockFreeQue<size_t> FreePages;
	};

}
