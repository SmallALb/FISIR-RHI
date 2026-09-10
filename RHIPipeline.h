#pragma once

#include <functional>

#include "HashCreate.h"
#include "RHIRenderPass.h"
#include "RHIShader.h"
#include "RHITypes.h"
#include "RHIViewport.h"
namespace FISIR {
	using Pipeline_t = void*;

	
	enum RHIBaseDataTYPE : uint8_t {
		_FLoat = 1, _Fvec2 = 2, _Fvec3 = 3, _Fvec4 = 4,
		_Int = 5, _Ivec2 = 6, _Ivec3 = 7, _Ivec4 = 8,
		_Image2D = 9, _Sampler2D = 10, _SamplerCube = 11
	};//need 4bits


	enum ColorBit {
		_R_PASS_ = 0x01,
		_G_PASS_ = 0x02,
		_B_PASS_ = 0x04,
		_A_PASS = 0x08
	};

	enum class ColorBlendOP {
		None = 0,
		And,
		Copy,
		Xor,
		Or,
		Invert,
	};

	enum SamplerBIT {
		_1xBIT = 0x01,
		_2xBIT = 0x02,
		_2x2BII = 0x04,
		_2x4BIT = 0x08,
		_4x4BIT = 0x10,
		_4x8BIT = 0x20,
		_8x8BIT =0x40,
	};
	

	struct RHIVertexInputInfo {
		RHIVertexInputInfo(const std::initializer_list<RHIBaseDataTYPE>& lis) {
			if (lis.size() >= 64) return;
			for (auto& BaseDataType : BaseDataTypes) BaseDataType = 0;
			for (auto& typ : lis) {
				BaseDataTypes[Count / 16] |= typ << (4 * (Count % 16));
				Count++;
			}
		}

		RHIBaseDataTYPE get(uint8_t index) const {
			return RHIBaseDataTYPE((BaseDataTypes[index / 16]) >> (4 * (index % 16)) & 15);
		}

		bool operator==(const RHIVertexInputInfo& info) const {
			return info.Count == Count && 
				BaseDataTypes[0] == info.BaseDataTypes[0] && 
				BaseDataTypes[1] == info.BaseDataTypes[1] && 
				BaseDataTypes[2] == info.BaseDataTypes[2] &&
				BaseDataTypes[3] == info.BaseDataTypes[3];
		}

		bool operator!=(const RHIVertexInputInfo& info) const {
			return !(*this == info);
		}

		RHIBaseDataTYPE operator[](uint8_t idx) const {
			return get(idx);
		}

		uint64_t BaseDataTypes[4];
		uint8_t Count = 0;
	};

	struct RHIPipelineDescribeBinding {
		RHIPipelineDescribeBinding() {}

		RHIPipelineDescribeBinding(uint8_t binding, uint16_t count, RHIDescriptorTyp descriptorT = RHIDescriptorTyp::UniformBuffer, RHIUsingStageFlags Stage = NoneStage):
			count(count),
			binding(binding),
			descriptorTyp(descriptorT),
			usingStage(Stage)
		{}

		uint16_t count;
		uint8_t binding;
		RHIDescriptorTyp descriptorTyp;
		RHIUsingStageFlags usingStage;

		bool operator == (const RHIPipelineDescribeBinding& other) const{
			return descriptorTyp == other.descriptorTyp && binding == other.binding && count == other.count && usingStage == other.usingStage;
		}

		bool operator != (const RHIPipelineDescribeBinding& other) const {
			return !(*this == other);
		}
	};

	struct RHIPipelineDescribeInfo {
		RHIPipelineDescribeInfo(std::initializer_list<RHIPipelineDescribeBinding> data) : Bindings(data) {
			HashVal = Bindings.size();
			for (const auto& v : Bindings) {
				uint32_t typeStage = (static_cast<uint32_t>(v.descriptorTyp) << 16 | static_cast<uint32_t>(v.usingStage));
				HashVal = HashCombine(HashVal, v.binding);
				HashVal = HashCombine(HashVal, v.count);
				HashVal = HashCombine(HashVal, typeStage);

			}
		}

		std::vector<RHIPipelineDescribeBinding> Bindings;
		size_t HashVal;

		bool operator == (const RHIPipelineDescribeInfo& other) const {
			return HashVal == other.HashVal && Bindings == other.Bindings;
		}
	};


	struct RasterizationState {
		bool DepthClipEnable;
		bool DepthOffsetEnable;
		bool RasterizerDiscardEnable;
		PolygonMode Polygon;
		FrontFace Front;
		CullMode Cull;
	};


	struct RHIMultiSampleState {
		uint32_t SamplerBit{1};
		bool ShadingEnable {0};
		float MinSamplerShading{1.0f};
		uint32_t SamplerMaks{0};
		bool alpthaToCoverageEnable{0};
		bool alpthaToOneEnable{0};
	};

	struct DepthStencilState {
		bool DepthTestEnable {0};
		bool DepthWriteEnable {0};
		bool DepthBoundsTestEnable {0};
		
		float DepthMinBounds {0.0f};
		float DepthMaxBounds {1.0f};
		APIOperation DepthCmpOp { _Equal_Greate_ };
		bool StencilTestEnable {0};
	};

	struct ColorBlendState {
		bool ColorBlenEnable {0};
		ColorBit UsingColorBit;
	};

	struct PieplineLayoutHash {
		PieplineLayoutHash(const RHIPipelineDescribeInfo& dinfo) :
			desinfo(dinfo)
		{
		}
		const RHIPipelineDescribeInfo& desinfo;

		bool operator == (const PieplineLayoutHash& other) const {
			return desinfo == other.desinfo;
		}

		bool operator != (const PieplineLayoutHash& other) const {
			return !(*this == other);
		}
	};

	struct RHIPipelineState {
		RHIPipelineDescribeInfo describeInfo;
		RHIVertexInputInfo vertexInfo;
		TopologyType topologyType;
		RasterizationState rasterizationState;
		RHIMultiSampleState multiSampleState;
		DepthStencilState depthStencilState;
		ColorBlendState colorblendState;
		RHIShader* Shaders[ShaderTYPCOUNT] {nullptr};
		RHIRenderPass* renderpass;
		bool isComputePipeline{0};

		size_t getHash() const {
			uint32_t h = 0;
			// combine describe and constant range
			h = HashCombine(h, (uint32_t)describeInfo.HashVal);
			// vertex info
			h = HashCombine(h, (uint32_t)vertexInfo.Count);
			for (int i = 0; i < 4; ++i) h = HashCombine(h, (uint32_t)vertexInfo.BaseDataTypes[i]);
			// topology
			h = HashCombine(h, (uint32_t)topologyType);
			// rasterization
			h = HashCombine(h, (uint32_t)rasterizationState.DepthClipEnable);
			h = HashCombine(h, (uint32_t)rasterizationState.DepthOffsetEnable);
			h = HashCombine(h, (uint32_t)rasterizationState.RasterizerDiscardEnable);
			h = HashCombine(h, (uint32_t)rasterizationState.Polygon);
			h = HashCombine(h, (uint32_t)rasterizationState.Front);
			h = HashCombine(h, (uint32_t)rasterizationState.Cull);
			// multisample
			h = HashCombine(h, multiSampleState.SamplerBit);
			h = HashCombine(h, (uint32_t)multiSampleState.ShadingEnable);
			h = HashCombine(h, std::hash<float>{}(multiSampleState.MinSamplerShading));
			h = HashCombine(h, multiSampleState.SamplerMaks);
			h = HashCombine(h, (uint32_t)multiSampleState.alpthaToCoverageEnable);
			h = HashCombine(h, (uint32_t)multiSampleState.alpthaToOneEnable);
			// depth stencil
			h = HashCombine(h, (uint32_t)depthStencilState.DepthTestEnable);
			h = HashCombine(h, (uint32_t)depthStencilState.DepthWriteEnable);
			h = HashCombine(h, (uint32_t)depthStencilState.DepthBoundsTestEnable);
			h = HashCombine(h, std::hash<float>{}(depthStencilState.DepthMinBounds));
			h = HashCombine(h, std::hash<float>{}(depthStencilState.DepthMaxBounds));
			h = HashCombine(h, (uint32_t)depthStencilState.DepthCmpOp);
			h = HashCombine(h, (uint32_t)depthStencilState.StencilTestEnable);
			// color blend
			h = HashCombine(h, (uint32_t)colorblendState.ColorBlenEnable);
			h = HashCombine(h, (uint32_t)colorblendState.UsingColorBit);
			// shaders and renderpass
			for (int i = 0; i < ShaderTYPCOUNT; ++i) h = HashCombine(h, HashPointer(Shaders[i]));
			h = HashCombine(h, HashPointer(renderpass));
			h = HashCombine(h, (uint32_t)isComputePipeline);
			return (size_t)h;
		}

		bool operator==(const RHIPipelineState& other) const {
			if (!(describeInfo == other.describeInfo)) return false;
			if (!(vertexInfo == other.vertexInfo)) return false;
			if (topologyType != other.topologyType) return false;
			if (rasterizationState.DepthClipEnable != other.rasterizationState.DepthClipEnable) return false;
			if (rasterizationState.DepthOffsetEnable != other.rasterizationState.DepthOffsetEnable) return false;
			if (rasterizationState.RasterizerDiscardEnable != other.rasterizationState.RasterizerDiscardEnable) return false;
			if (rasterizationState.Polygon != other.rasterizationState.Polygon) return false;
			if (rasterizationState.Front != other.rasterizationState.Front) return false;
			if (rasterizationState.Cull != other.rasterizationState.Cull) return false;
			if (multiSampleState.SamplerBit != other.multiSampleState.SamplerBit) return false;
			if (multiSampleState.ShadingEnable != other.multiSampleState.ShadingEnable) return false;
			if (multiSampleState.MinSamplerShading != other.multiSampleState.MinSamplerShading) return false;
			if (multiSampleState.SamplerMaks != other.multiSampleState.SamplerMaks) return false;
			if (multiSampleState.alpthaToCoverageEnable != other.multiSampleState.alpthaToCoverageEnable) return false;
			if (multiSampleState.alpthaToOneEnable != other.multiSampleState.alpthaToOneEnable) return false;
			if (depthStencilState.DepthTestEnable != other.depthStencilState.DepthTestEnable) return false;
			if (depthStencilState.DepthWriteEnable != other.depthStencilState.DepthWriteEnable) return false;
			if (depthStencilState.DepthBoundsTestEnable != other.depthStencilState.DepthBoundsTestEnable) return false;
			if (depthStencilState.DepthMinBounds != other.depthStencilState.DepthMinBounds) return false;
			if (depthStencilState.DepthMaxBounds != other.depthStencilState.DepthMaxBounds) return false;
			if (depthStencilState.DepthCmpOp != other.depthStencilState.DepthCmpOp) return false;
			if (depthStencilState.StencilTestEnable != other.depthStencilState.StencilTestEnable) return false;
			if (colorblendState.ColorBlenEnable != other.colorblendState.ColorBlenEnable) return false;
			if (colorblendState.UsingColorBit != other.colorblendState.UsingColorBit) return false;
			for (int i = 0; i < ShaderTYPCOUNT; ++i) if (Shaders[i] != other.Shaders[i]) return false;
			if (renderpass != other.renderpass) return false;
			if (isComputePipeline != other.isComputePipeline) return false;
			return true;
		}


	};
	

	class RHIPipeline {
	public:
		virtual ~RHIPipeline() {}

		virtual Pipeline_t getPipelineHandle() = 0;

		virtual bool isComputePipeline() const = 0;

	};
 
}

namespace std {
	template<>
	struct hash<FISIR::RHIPipelineDescribeInfo> {

		size_t operator() (const FISIR::RHIPipelineDescribeInfo& info) const {
			return info.HashVal;
		}
	};
	

	template<> 
	struct hash<FISIR::PieplineLayoutHash> {
		size_t operator() (const FISIR::PieplineLayoutHash& v) const {
			size_t h = 0;
			h = FISIR::HashCombine(h, v.desinfo.HashVal);
			return h;
		}
	};

	template<>
	struct hash<FISIR::RHIPipelineState> {
		size_t operator() (const FISIR::RHIPipelineState& state) const {
			return state.getHash();
		}

	};

}