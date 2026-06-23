#pragma once
#include <cstdint>
#include <vector>
#include <initializer_list>

namespace FISIR {
    class RHIResourcePack;

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

    enum class Type { NLL, Buffer, Texture, Sampler, RenderPass, Pipeline, FrmeBuffer };
    
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
        TransferDstBuffer = 0x20
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
    
    enum class RHIDescriptorTyp : uint8_t { Sampler, Image, SamplerImage, UniformBuffer };
    
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

    struct BufferTransitionInfo {
        class RHIBuffer* buffer; 
        ResourceAccess waitForAccessDone;
        ResourceAccess beginAccessWhenDone;
    };

    struct TextureTransitionInfo {
        class RHITexture* texture;
        ResourceAccess waitForAccessDone {ResourceAccess::Undefined};
        ResourceAccess beginAccessWhenDone;
        TextureLayout oldLayout {TextureLayout::Undefined};
        TextureLayout newLayout;
    };

    struct RHIResourcePackResult {
        RHIResourcePack* ResourcePack{ nullptr };
        RHIResourcePack* SamplerPack{ nullptr };

    };


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