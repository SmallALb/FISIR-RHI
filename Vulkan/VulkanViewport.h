#pragma once

#include "../RHIViewport.h"

struct VkSurfaceKHR_T;

namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;
	class VulkanFrameBuffer;
	class VulkanRenderPass;
	class VulkanImageView;

	struct __VKViewportData;
	
	class VullkanViewport : public RHIViewport {
	public:
		
		VullkanViewport(VulkanDevice* device, uint32_t iniWidth, uint32_t initHeight, void* WindowHandle);

		~VullkanViewport();

		virtual void* getNativeSwapChain() const override;

		virtual void* getNativeBackBufferTexture() const override;

		virtual void* getNativeWindow(void** handle) const override;

		virtual uint32_t getViewportWidth() const override;

		virtual uint32_t getViewportHeight() const override;

		virtual void tick(float deltatime) override;

		virtual void waitForFrameEventCompletion() override;

		virtual void IssueFrameEvent() override;

		VkSurfaceKHR_T* getVkSurface() const;

		VulkanDevice* mDevice{nullptr};
		__VKViewportData* mData {nullptr};
		std::vector<VulkanTexture*> VulkanSwapChainTextures;
		VulkanRenderPass* VulkanSwapChainRednerPass;
		std::vector<VulkanFrameBuffer*> VulkanSwapChainFrameBuffers;
	};

}