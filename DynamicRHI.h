#pragma once
#include "RHITypes.h"
#include <vector>
#include <cstdint>



namespace FISIR {	
	class RHITexture;
	class RHIBuffer;
	class RHIViewport;
	class RHIPipeline;
	class RHIContext;
	class RHIShader;
	class RHICommandListBase;
	class RHIResourcePack;
	class RHIRenderPass;
	class RHIResource;

	struct RHIPipelineState;
	struct RHIRenderPassInfo;


	class DynamicRHI {
	public:
		virtual ~DynamicRHI() {}
		virtual bool Init() = 0;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) = 0;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) = 0;

		virtual RHIViewport* RHICreateViewport() = 0;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) = 0;

		virtual RHIContext* RHIGetContext(CmdType type) = 0;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) = 0;

		virtual void RHISubmitCommandList(RHICommandListBase* cmdList) = 0;

		virtual RHIResourcePack* RHICreateResourcePack(Type restyp, const std::vector<RHIResource*>& resources) = 0;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) = 0;
	};

}