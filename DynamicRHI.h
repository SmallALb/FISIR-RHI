#pragma once
#include "RHITypes.h"
#include <vector>
#include <cstdint>

#include <atomic>

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
	class RHIFrameBuffer;
	class RHISemaphore;
	class RHIFence;
	class RHISwapChain;
	class RHISampler;
	struct RHIPipelineState;
	struct RHIRenderPassInfo;
	
	
	class DynamicRHI {
	public:
		virtual ~DynamicRHI() {}
		virtual bool Init() = 0;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) = 0;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) = 0;

		virtual RHIViewport* RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type, void* WindowHandle) = 0;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) = 0;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) = 0;

		virtual void RHISubmitCommandList(
			RHICommandListBase* cmdList, 
			RHIFence* fence = nullptr, 
			const std::vector<RHISemaphore*>& waitSemaphore = {},
			const std::vector<RHISemaphore*>& singalSemaphore = {},
			std::atomic_bool* submitTag = nullptr, std::atomic_bool* gpuDoneTag = nullptr) = 0;

		virtual RHIResourcePackResult RHICreateResourcePack(const std::vector<RHIResource*>& resources) = 0;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) = 0;
	
		virtual RHIFrameBuffer* RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) = 0;
		
		virtual RHISemaphore* RHICreateSemaphore(const char* name = "Unnamed Semaphore") = 0;
		
		virtual void RHIDestroySemaphore(RHISemaphore* semaphore) = 0;

		virtual RHISampler* RHICreateSampler(const SamplerInfo& info) = 0;

		virtual void RHIFlushAndWaitAfterCommand(CmdType cmdtype) = 0;

		virtual void RHICreateContext(RHICommandListBase* cmdlist) = 0;

		virtual RHIFence* RHICreateFence(bool signaled = false, const char* name = "Unnamed Fence") = 0;

		virtual RHISwapChain* RHIGetSwapChain(RHIViewport* viewport) = 0;
		
		virtual void RHIDestroyFence(RHIFence* fence) = 0;

	};

}