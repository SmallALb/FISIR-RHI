#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <initializer_list>
#include <thread>
#include "LockFreeQue.h"
#include <array>
#include <semaphore>
namespace FISIR {
    class RHIResourcePack;
	class RHIFrameBuffer;
	class RHIFence;
	class RHIBuffer;
	class RHITexture;
	class RHIPipeline;
	class RHISemaphore;
	class RHISwapChain;
	class RHIViewport;

    enum class CmdType { None = 0, Render, Compute, Transfer };

    enum ShaderTYP {
        __VERTEXSHADER__,
        __FRAGMENTSHADER__,
        __TESSSHADER__,
        __GEOMETRY__,
        __COMPUTESHADER__,
        ShaderTYPCOUNT
    };

    enum APIOperation {
        _NONE_OP_,
        _NOT_Equal_,
        _Equal_,
        _Equal_Less_,
        _Equal_Greate_,
        _Less_,
        _Greate_,
        _Always_
    };

    enum class Type { NLL, Buffer, Texture, Sampler, RenderPass, Pipeline, FrameBuffer };
    
    enum class TextureCOLORType {
        RGB_8, RGB_16, RGB_32, RGBA_8, RGBA_16, RGBA_32, R_8, Depth24_Stencil8
    };

    enum class TextureType { TEXTURE1D, TEXTURE2D, TEXTURE3D, TEXTUREARRAY };
    
    enum  BufferLayout {
        UndefinedBuffer = 0x00,
        VertexBuffer =0x01,
        IndexBuffer =0x02,
        UniformBuffer =0x04,
        StorageBuffer =0x08,
        TransferSrcBuffer = 0x10,
        TransferDstBuffer = 0x20,
        IndirectBuffer = 0x40
     };

     using BufferLayoutFlags = uint32_t;

    enum MemType : uint32_t {
        MemTypNone = 0,
        MemTypeDeviceLocal = 0x00000001,
        MemTypHostVisable = 0x00000002,
        MemTypHostCoherent = 0x00000004,
        MemTypHostCached = 0x00000008,
    };

    enum class ResourceAccess { Undefined, ShaderReadOnly, ShaderWriteOnly, ShaderReadWrite, TransferSrc, TransferDst };
    
    enum class TextureLayout { Undefined, ColorAttachmentOptimal, DepthStencilAttachmentOptimal, ShaderReadOnlyOptimal, TransferSrcOptimal, TransferDstOptimal, Storage, Present };
    
    enum class TopologyType { Point, Line, LineStrip, Triangle, TriangleStrip, TriangleFan };
    
    enum class PolygonMode : uint8_t { Fill, Line, Point };
    
    enum class FrontFace : uint8_t { CCW, CW };
    
    enum class CullMode : uint8_t { None, FRONT, BACK };
    
    enum class RHIDescriptorTyp : uint8_t { Sampler, Image, SamplerImage, UniformBuffer, StorageBuffer};
    
    enum RHIUsingStage {
        NoneStage = 0x0,
        VertexShaderStage = 0x01,
        FragmentShaderStage = 0x02,
        TessShaderStage = 0x04,
        ComputeShaderStage = 0x08,
        GeometryShaderStage = 0x10,
        PipelinTopStage = 0x20,
        PipelineBottomStage = 0x40,
        PipelineVertexInputStage = 0x80,
        PipelineBeforeFragmentStage = 0x100,
        PipelineAfterFragmentStage = 0x200,
        PipelineTransferStage = 0x400
    };

    using RHIUsingStageFlags = uint32_t;
    
    enum TextureUseFor {
        TextureUseForNone = 0,
        TextureUseForColorAttachment = 1,
        TextureUseForDepthStencilAttachment = 1 << 1,
        TextureUseForShaderReadOnly = 1 << 2,
        TextureUseForTransferSrc = 1 << 3,
        TextureUseForTransferDst = 1 << 4,
        TextureUseForStorage = 1 << 5,
        TextureUseForInputAttachment = 1 << 6,
        TextureUseForDefault = TextureUseForColorAttachment | TextureUseForDepthStencilAttachment | TextureUseForShaderReadOnly,
        TextureUseForAll = TextureUseForColorAttachment | TextureUseForDepthStencilAttachment | TextureUseForShaderReadOnly | TextureUseForTransferSrc | TextureUseForTransferDst | TextureUseForStorage | TextureUseForInputAttachment,
    };
    
    using TextureUseForFlags = uint32_t;


    enum class SamplerFilter {
        NEAREST,
        LINEAR,
        CUBIC,
    };

    enum class SamplerOverFoundMode {
        REPEAT = 0,
        MIRRORED_REPEAT,
        CLAMP_TO_EDGE,
        MIRROR_CLAMP_TO_EDGE,
        CLAMP_TO_BORDER
    };

    enum class RenderTargetLoadAction : unsigned char {
        None,
        Load,
        Clear,

        Count,
        CountBits = 2,
    };

    enum class RenderTargetStoreAction : unsigned char {
        None,
        Store,
        MultisampleResolve,

        Count,
        CountBits = 2,
    };


    struct ColorInfo{
        struct {
            float R;
            float G;
            float B;
            float A;
        };
        float Values[4];
    };


    struct ClearValue {
        bool ColorClear {1};
        ColorInfo colorinfo;
        bool DepthStencilClear{ 1 };
        float depthclearval {0.0};
        unsigned int stencilVal {0};
    };

    struct TextureSize {
        uint32_t height;
        uint32_t width;
        uint32_t depth;

        union {
            int32_t x;
            int32_t y;
            int32_t z;
        };

        bool operator ==(const TextureSize& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct TextureInfo {
        TextureSize         size;
        TextureCOLORType    colorType;
        TextureType         type;
        TextureUseForFlags  useFor{ TextureUseForStorage | TextureUseForShaderReadOnly };
        uint16_t            mipLevels{ 1 };
        uint16_t            arrayLayers{ 1 };
        uint32_t            sampleCount{ 1 };
    };


    struct SamplerInfo {
        SamplerFilter           enlagerFilter { SamplerFilter::NEAREST };
        SamplerFilter           minFilter { SamplerFilter::NEAREST };
        SamplerFilter           mipMapMode {SamplerFilter::NEAREST};
        SamplerOverFoundMode    u{ SamplerOverFoundMode::REPEAT };
        SamplerOverFoundMode    v{ SamplerOverFoundMode::REPEAT };
        SamplerOverFoundMode    w { SamplerOverFoundMode::REPEAT};
        float                   mipLodBias {1.0};
        bool                    anisotropyEnable {0};
        float                   maxAnisotropy {1.0};
        bool                    compareEnable {0};
        APIOperation            compareOP {_NONE_OP_};
        float                   maxLop {1.0};
        float                   minLop {1.0};
        bool                    unNormalized {0};
    };

    struct BufferInfo {
        void*               data_CPU = nullptr;
        uint64_t            size = 0;
        uint64_t            stride = 0;
        BufferLayoutFlags   bufferlayout = UndefinedBuffer;
        MemType             memoryType = MemType::MemTypNone;
    };



    struct RHIResourcePackResult {
        RHIResourcePack* ResourcePack{ nullptr };
        RHIResourcePack* SamplerPack{ nullptr };

    };




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



	struct End_CmdInfo {
		RHICommandFlags Flags{ 0 };
		RHIFence* fence;
		RHISemaphore** waits;
		uint32_t waitcount {0};
		RHISemaphore** signals;
		uint32_t signalcount {0};
		// CPU-side signal: RHI thread sets to true after vkQueueSubmit;
		// main thread busy-waits before calling vkQueuePresentKHR.
		std::atomic<bool>* submitReady {nullptr};
	};


	struct ReserveInput_CmdInfo {
		RHICommandFlags Flags{ 0 };
	};

	constexpr size_t MaxCMDPoolSize = 16 * 1024 * 1024;
	constexpr size_t MaxCMDPageSize = 64 * 1024;
	constexpr size_t CmdPageMask = MaxCMDPageSize - 1;
	constexpr size_t PageReverseSize = sizeof(RHICommandT) + sizeof(ReserveInput_CmdInfo);

	struct RingCommandPool {


		RingCommandPool() {
			Buffer = (uint8_t*)malloc(MaxCMDPoolSize);
			for (size_t i = 0; i < 256; i++) {
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
				if (currentSize - sizeofCMD < PageReverseSize || (currentSize - sizeofCMD == PageReverseSize && commandT != RHICommandT::End)) {
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
			size_t index = 256;
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
		std::array<Page, 256> Pages;
		LockFreeQue<size_t> FreePages;
	};


	template<>
	inline bool QueTest<RingCommandPool::Page::BatchInfo>(const RingCommandPool::Page::BatchInfo& val) {
		// 检查 page 指针是否有效
		if (val.page == nullptr) {
			printf("BatchInfo: page is nullptr");
			return false;
		}
		if ((uintptr_t)val.page == 0xDDDDDDDDDDDDDDDDULL) {
			printf("BatchInfo: page is 0xDDDDDDDDDDDDDDDD (freed memory)");
			return false;
		}
		if ((uintptr_t)val.page < 0x1000) {
			printf("BatchInfo: page is too low: 0x%p", val.page);
			return false;
		}
		return true;
	}



    inline const char* getTextureLayoutName(TextureLayout layout) {
        switch (layout) {
        case TextureLayout::Undefined:                  return "Undefined";
        case TextureLayout::ColorAttachmentOptimal:     return "ColorAttachmentOptimal";
        case TextureLayout::DepthStencilAttachmentOptimal: return "DepthStencilAttachmentOptimal";
        case TextureLayout::ShaderReadOnlyOptimal:      return "ShaderReadOnlyOptimal";
        case TextureLayout::TransferSrcOptimal:         return "TransferSrcOptimal";
        case TextureLayout::TransferDstOptimal:         return "TransferDstOptimal";
        case TextureLayout::Storage:                    return "Storage";
        case TextureLayout::Present:                    return "Present";
        default:                                        return "Unknown";
        }
    }


} 