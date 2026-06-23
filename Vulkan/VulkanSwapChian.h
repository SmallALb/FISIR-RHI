#pragma once
#include <atomic>
#include "../RHISwapChain.h"
namespace FISIR {
	class VulkanDevice;
	class VulkanViewport;
	class VulkanFrameBuffer;
	class VulkanTexture;
	class VulkanRenderPass;
	class VulkanPipeline;
	class VulkanQueue;
	class DynamicRHI;

	class __VkSwapChainData;


	class VulkanSwapChain : public RHISwapChain{
	public:	
		VulkanSwapChain(VulkanViewport* viewport, uint32_t QueFamilyIndex);

		bool init(VulkanDevice* device, DynamicRHI* rhi);

		~VulkanSwapChain();

		virtual uint32_t acquireGetImageInfoID() override;

		virtual void present(uint32_t infoid) override;

		virtual RHITexture* getSwapChainFrameTexture(uint32_t imageindex) const override;

		virtual uint32_t getImageCount() const override;

		virtual SwapChainGetImageInfo getSwapChainGetImageInfo(uint32_t id) override;

		virtual RHIFrameBuffer* getSwapChainFrameBuffer(uint32_t imageindex) override;

		virtual RHIPipeline* getSwapChainRenderPipeline() const override;

	private:
		bool recreateSwapChain();
		
		bool createSwapChian();

		bool createPipelineandRenderPass();
			
		SwapChainGetImageInfo SwapChainFrameInfos[MAX_SWAPCHAIN_FRAME];
		VulkanTexture* SwapChainTextures[MAX_SWAPCHAIN_FRAME];
		VulkanFrameBuffer* SwapChainFrameBuffers[MAX_SWAPCHAIN_FRAME];
		
		uint32_t MaxSwapChianFramCount{0};

		VulkanDevice* mDevice;
		
		VulkanRenderPass* VulkanSwapChainRednerPass;
		VulkanPipeline* VulkanViewportPipeline;

		VulkanViewport* Surfaceviewport;

		uint32_t mPresentQueFamilyIndex{0};

		VulkanQueue* PresentQueue {nullptr};

		DynamicRHI* usingRHI;

		__VkSwapChainData* mData;

		std::atomic_bool needReBuildSwapChain {0};
	};


}