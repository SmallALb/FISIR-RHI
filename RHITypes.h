#pragma once
#include <cstdint>
#include <vector>
#include <initializer_list>

namespace FISIR {

    enum class CmdType { None = 0, Render, Compute, Transfer };

    enum ShaderTYP {
        __VERTEXSHADER__,
        __FRAGMENTSHADER__,
        __TESSSHADER__,
        __GEOMETRY__,
        __COMPUTESHADER__,
        ShaderTYPCOUNT
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
    
    enum class RHIDescriptorTyp : uint8_t { Sampler, Image, UniformBuffer };
    
    enum RHIUsingStage : uint16_t {
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
    
    enum TextureUseFor {
        TextureUseForNone = 0,
        TextureUseForColorAttachment = 1,
        TextureUseForDepthStencilAttachment = 1 << 1,
        TextureUseForShaderReadOnly = 1 << 2,
        TextureUseForTransferSrc = 1 << 3,
        TextureUseForTransferDst = 1 << 4,
        TextureUseForStorage = 1 << 5,
        TextureUseForDefault = TextureUseForColorAttachment | TextureUseForDepthStencilAttachment | TextureUseForShaderReadOnly,
        TextureUseForAll = TextureUseForColorAttachment | TextureUseForDepthStencilAttachment | TextureUseForShaderReadOnly | TextureUseForTransferSrc | TextureUseForTransferDst | TextureUseForStorage,
    };
    
    using TextureUseForFlags = uint32_t;


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


    struct BufferInfo {
        void* data_CPU = nullptr;
        uint64_t size = 0, stride = 0;
        BufferLayoutFlags bufferlayout = UndefinedBuffer;
        MemType memoryType = MemType::MemTypNone;
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

    inline const char* getTextureLayoutName(TextureLayout layout) {
        switch (layout) {
        case TextureLayout::Undefined:                  return "Undefined";
        case TextureLayout::ColorAttachmentOptimal:     return "ColorAttachmentOptimal";
        case TextureLayout::DepthStencilAttachmentOptimal: return "DepthStencilAttachmentOptimal";
        case TextureLayout::ShaderReadOnlyOptimal:      return "ShaderReadOnlyOptimal";
        case TextureLayout::TransferSrcOptimal:         return "TransferSrcOptimal";
        case TextureLayout::TransferDstOptimal:         return "TransferDstOptimal";
        case TextureLayout::Storage:                    return "Storage";
        default:                                        return "Unknown";
        }
    }


} 