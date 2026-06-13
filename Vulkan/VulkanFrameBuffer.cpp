#include "VulkanFrameBuffer.h"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "../../Log/Logger.h"
#include "VulkanRenderPass.h"
#include "VulkanImageView.h"
namespace FISIR{
	
	
	

	struct __VkFrameBufferData {
		VkFramebuffer mBuffer;
		std::vector<VkImageView> views;

	};


	VulkanFrameBuffer::VulkanFrameBuffer(VulkanDevice* device, const std::vector<RHITexture*>& textures, uint32_t width, uint32_t height, RHIRenderPass* renderpass)
		: mDevice(device), 
		mDepthStencilEntry(nullptr),        
		mRenderPass(renderpass),        
		mHeight(height),
		mWidth(width), 
		mData(new __VkFrameBufferData())  
	{
		
		auto vulkanrenderPass = static_cast<VulkanRenderPass*>(renderpass);

		for (auto& texture : textures) {
			auto view = mDevice->getImageView(texture);
			mData->views.push_back(view->getImageViewHandle());
		}

		VkFramebufferCreateInfo famebufferCreateInfo {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = static_cast<VkRenderPass>(mRenderPass->getRenderPassHandle()),
			.attachmentCount = static_cast<uint32_t>(mData->views.size()),
			.pAttachments = mData->views.data(),
			.width = width,
			.height = height,
			.layers = 1,
		};

		if (vkCreateFramebuffer(mDevice->getLogicalDevice(), &famebufferCreateInfo, nullptr, &mData->mBuffer) != VK_SUCCESS) {
			Error("Create FrameBuffer Failed");
			return;
		}

	}
	
	VulkanFrameBuffer::~VulkanFrameBuffer() {
	
	}

	RHIRenderPass* VulkanFrameBuffer::getFrameRenderPass() const {
		return mRenderPass;
	}

	uint32_t VulkanFrameBuffer::getFrameWidth() const {
		return mWidth;
	}

	uint32_t VulkanFrameBuffer::getFrameHeight() const {
		return mHeight;
	}

	
	void* VulkanFrameBuffer::getResourceAPIHandle() const {
		return mData->mBuffer;
	}
}
