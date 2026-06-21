#pragma once

#include "../DynamicRHI.h"
#include "VulkanViewport.h"
#include "../../DataBase/AutoPtr.h"

#include "../../DLLheader.h"
#include <mutex>
#include "VulkanCommandPool.h"
#include "VulkanFencePool.h"
#include "VulkanSemaphorePool.h"
#include "VulkanDescriptorPool.h"
#include <unordered_set>
#include <thread>
namespace FISIR {  
  class VulkanPipeline;
  class VulkanDevice;
  class VulkanCommandPool;
  class VulkanViewport;
  class VulkanSwapChain;
  class VulkanShader;
  class VulkanSampler;
  class VulkanRHI : public DynamicRHI {
	public:
		VulkanRHI();

		~VulkanRHI();

		virtual bool Init() override;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) override;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) override;

		virtual RHIViewport* RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type, void* WindowHandle) override;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) override;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) override;

		virtual void RHISubmitCommandList(RHICommandListBase* cmdList, RHIFence* fence, const std::vector<RHISemaphore*>& waitSemaphore, const std::vector<RHISemaphore*>& singalSemaphore, std::atomic_bool* tag) override;

		virtual RHIResourcePackResult RHICreateResourcePack(const std::vector<RHIResource*>& resources) override;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) override;

		virtual RHIFrameBuffer* RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) override;

		virtual RHISemaphore* RHICreateSemaphore(const char* name) override;

		virtual RHISwapChain* RHIGetSwapChain(RHIViewport* viewport) override;

		virtual void RHIDestroySemaphore(RHISemaphore* semaphore) override;

		virtual RHISampler* RHICreateSampler(const SamplerInfo& info) override;

		virtual RHIFence* RHICreateFence(bool signaled, const char* name) override;

		virtual void RHICreateContext(RHICommandListBase* cmdlist) override;

		virtual void RHIDestroyFence(RHIFence* fence) override;

		ThreadCommanPoolListener* choiceCommandPool(CmdType type);

		ThreadCommanPoolListener* choiceCommandPool(uint32_t FamilyIndex);


	private:
		void VulkanRHILoop();

		void VulkanResourceLoop();
	private:
		VulkanDevice* mDevice;
		VulkanFencePool* mFencePool;
		VulkanSemaphorePool* mSemaphorePool;
		VulkanDescriptorPool* mDescriptorPool;
		VulkanCommandPoolManager* mCmdPoolManager;
		std::atomic_bool stopTag {0};
		
  };


}

extern "C" {
	EXPORTDLL FISIR::DynamicRHI* RHICreate();
	EXPORTDLL void RHIDestroy(FISIR::DynamicRHI* rhi);
}
