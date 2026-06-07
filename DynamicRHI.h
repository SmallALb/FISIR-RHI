#pragma once
#include <vector>
#include <cstdint>
#include "../Log/Logger.h"
#include "RHIResource.h"
#include "RHITexture.h"
#include "RHIBuffer.h"
#include "RHIViewport.h"
#include "RHIContext.h"
#include "RHIRenderPass.h"
#include"RHIPipeline.h"
#include "RHICommandList.h"
#include "RHIShader.h"
#include "RHIResourcePack.h"

enum class RHIAPI {
	OpenGL,
	Vulkan
};

namespace FISIR {	
	

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

		virtual RHIResourcePack* RHICreateResourcePack(Type restyp, std::initializer_list<RHIResource*> resources) = 0;

		virtual RHIResourcePack* RHICreateResourcePack(Type restyp, const std::vector<RHIResource*>& resources) = 0;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) = 0;
	};

	void setRHIAPI(RHIAPI api);
	RHIAPI getCurrentRHIAPI();
	void closeRHIAPI();

	DynamicRHI* RHIGet();
	void RHIDestroy();
}