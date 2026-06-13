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

#include <thread>
namespace FISIR {  
  class VulkanPipeline;
  class VulkanDevice;
  class VulkanCommandPool;


  class VulkanRHI : public DynamicRHI {
	public:
		~VulkanRHI();

		virtual bool Init() override;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) override;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) override;

		virtual RHIViewport* RHICreateViewport() override;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) override;

		virtual RHIContext* RHIGetContext(CmdType type) override;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) override;

		virtual void RHISubmitCommandList(RHICommandListBase* cmdList) override;

		virtual RHIResourcePack* RHICreateResourcePack(Type restyp, const std::vector<RHIResource*>& resources) override;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) override;

		virtual RHIFrameBuffer* RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) override;



	private:
		void VulkanRHILoop();

		void VulkanResourceLoop();
	private:
		std::unordered_map<std::string, VulkanPipeline*> PipelineMap;
		VulkanDevice* mDevice;
		VulkanFencePool* mFencePool;
		VulkanSemaphorePool* mSemaphorePool;
		VulkanDescriptorPool* mDescriptorPool;
		std::atomic<int> CurrentFrame{ 0 };
		std::atomic_bool stopTag {0};
  };


}

extern "C" {
	EXPORTDLL FISIR::DynamicRHI* RHICreate();
	EXPORTDLL void RHIDestroy(FISIR::DynamicRHI* rhi);
}
