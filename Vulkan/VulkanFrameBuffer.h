#pragma once

#include "../RHIFrameBuffer.h"

namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;
	class VulkanRenderPass;

	struct __VkFrameBufferData;

	class VulkanFrameBuffer: public RHIFrameBuffer {
	public:
		VulkanFrameBuffer(VulkanDevice* device, const std::vector<RHITexture*>& textures, uint32_t width, uint32_t height, RHIRenderPass* renderpass);
		
		~VulkanFrameBuffer();

		virtual RHIRenderPass* getFrameRenderPass() const override;

		virtual uint32_t getFrameWidth() const override;

		virtual uint32_t getFrameHeight() const override;

		virtual void* getResourceAPIHandle() const override;

	private:
		VulkanDevice* mDevice;
		VulkanTexture* mDepthStencilEntry;
		RHIRenderPass* mRenderPass;
		uint32_t mHeight{0};
		uint32_t mWidth{0};
		__VkFrameBufferData* mData;
	};

}