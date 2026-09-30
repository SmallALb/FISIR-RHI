#pragma once

#include "Log/Logger.h"

namespace FISIR{

	class RHISwapChain;

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
		//常量
		PushConstant,
		//Transfer
		TransferTexture,
		TransferBuffer,
		CopyBufferToBuffer,
		CopyBufferToTexture,
		CopyImageToBuffer,
		//Compute
		Dispatch,
		//Present
		Present                     // ← 末尾追加，不动既有取值

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
		int32_t x;
		int32_t y;
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


	// PushConstant：一次向当前管线推送常量。
	// data **内联**在命令里（不堆分配）：命令页写入后到提交前的生命周期不好管，
	// 内联最省事也最快。上限 64 字节 —— 规范只保证 128 字节可用，取一半足够覆盖
	// 本项目所有用途（层号/轮次、若干标量、一个 4×4 矩阵）。超过就截断（见命令列表侧断言）。
	constexpr uint32_t RHI_PUSH_CONSTANT_MAX_BYTES = 64;

	struct PushConstant_CmdInfo {
		RHICommandFlags    Flags{ 0 };
		uint32_t           offset;
		uint32_t           size;
		RHIUsingStageFlags usingStage;
		uint8_t            data[RHI_PUSH_CONSTANT_MAX_BYTES];
	};



	struct End_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIFence* fence;
		RHISemaphore** waits;
		uint32_t waitcount{ 0 };
		RHISemaphore** signals;
		uint32_t signalcount{ 0 };
	};

	// 呈现指令：录制线程只登记（哪个交换链、哪个帧 ID），真正的 vkQueuePresentKHR 由
	// RHI 线程在本页/本节点 vkQueueSubmit 之后执行。acquire、present、提交因此全在同一线程，
	// VkQueue 与 VkSwapchainKHR 都不会被跨线程触碰。
	struct Present_CmdInfo {
		RHICommandFlags      Flags{ 0 };
		class RHISwapChain*  swapchain{ nullptr };
		uint32_t             frameID{ 0 };
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

				// 命令负载字节数（不含 RHICommandT 头）。用于按命令边界拆分大批次，
				// 使多个工作线程能并行翻译同一渲染通道内的不同命令段。
				static size_t CommandPayloadSize(RHICommandT t) {
					switch (t) {
					case RHICommandT::End:                      return sizeof(End_CmdInfo);
					case RHICommandT::BeginRenderPass:          return sizeof(BeginRenderPass_CmdInfo);
					case RHICommandT::EndRenderPass:            return sizeof(ReserveInput_CmdInfo);
					case RHICommandT::DrawPrimitive:            return sizeof(DrawPrimitive_CmdInfo);
					case RHICommandT::DrawIndex:                return sizeof(DrawIndex_CmdInfo);
					case RHICommandT::DrawIndirect:             return sizeof(DrawIndirect_CmdInfo);
					case RHICommandT::DrawIndexedIndirect:      return sizeof(DrawIndexedIndirect_CmdInfo);
					case RHICommandT::BindPipeline:             return sizeof(BindPipeline_CmdInfo);
					case RHICommandT::BindVertexBuffer:         return sizeof(BindVertextBuffer_CmdInfo);
					case RHICommandT::BindIndexBuffer:          return sizeof(BindIndexBuffer_CmdInfo);
					case RHICommandT::BindResourceAndSamplerPack: return sizeof(BindResourcePack_CmdInfo);
					case RHICommandT::BindViewPort:             return sizeof(BindViewPort_CmdInfo);
					case RHICommandT::BindScissor:              return sizeof(BindScissor_CmdInfo);
					case RHICommandT::PushConstant:             return sizeof(PushConstant_CmdInfo);
					case RHICommandT::TransferTexture:          return sizeof(TextureTransition_CmdInfo);
					case RHICommandT::TransferBuffer:           return sizeof(BufferTransition_CmdInfo);
					case RHICommandT::CopyBufferToBuffer:       return sizeof(CopyBufferToBuffer_CmdInfo);
					case RHICommandT::CopyBufferToTexture:      return sizeof(CopyBufferToTexture_CmdInfo);
					case RHICommandT::CopyImageToBuffer:        return sizeof(CopyImageToBuffer_CmdInfo);
					case RHICommandT::Dispatch:                 return sizeof(Dispatch_CmdInfo);
					case RHICommandT::Present:                  return sizeof(Present_CmdInfo);
					default:                                    return 0;
					}
				}

				// 把 ReadBegin 前进一条命令，返回被跳过的命令类型。
				// 前进量与 getBatchData 一致（sizeof(RHICommandT)+负载），
				// 保证拆分点始终落在命令边界上。
				RHICommandT skipCommand() {
					RHICommandT t = getCommandType();
					ReadBegin += sizeof(RHICommandT) + CommandPayloadSize(t);
					return t;
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
