#include "VulkanImageView.h"

#include <vulkan/vulkan.h>

#include "../LockFreeQue.h"
#include "../Log/Logger.h"
#include "VulkanDevice.h"
#include "VulkanTexture.h"

namespace FISIR {
	uint32_t getVulkanViewTypeFromTextureType(TextureType typ) {
		switch (typ) {
		case TextureType::TEXTURE1D:
			return VK_IMAGE_VIEW_TYPE_1D;
		case TextureType::TEXTURE2D:
			return VK_IMAGE_VIEW_TYPE_2D;
		case TextureType::TEXTURE3D:
			return VK_IMAGE_VIEW_TYPE_3D;
		case TextureType::TEXTUREARRAY:
			return VK_IMAGE_VIEW_TYPE_CUBE;
		}
		return VK_IMAGE_VIEW_TYPE_1D;
	}

	static VkImageAspectFlags getVulkanAspectFlagsForUsingImpl(TextureUseForFlags usefor) {
		return usefor & TextureUseForDepthStencilAttachment ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	}

	uint32_t getVulkanAspectFlagsForUsing(TextureUseForFlags usefor) {
		return (uint32_t)getVulkanAspectFlagsForUsingImpl(usefor);
	}

	struct __VkImageViewData {
		VkImageView ImageView;
		bool isBuild {0};
	};

	VulkanImageView::VulkanImageView(VulkanDevice* device, VulkanTexture* texture): mDevice(device) {
		mData = new __VkImageViewData();
		if (texture) build(texture);
	}

	VulkanImageView::~VulkanImageView() {
		if (mData->isBuild) destroyCurrent();
	}

	void VulkanImageView::build(VulkanTexture* texture) {
		VkImageViewCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = static_cast<VkImage>(texture->getResourceAPIHandle()),
		.viewType = (VkImageViewType)getVulkanViewTypeFromTextureType(texture->getTextureType()),
		.format = (VkFormat)(static_cast<VulkanTexture*>(texture))->getVkColorType(),
		.subresourceRange = {
			.aspectMask = (VkImageAspectFlags)getVulkanAspectFlagsForUsing(texture->getTextureUseFor()),
			.baseMipLevel = 0,
			.levelCount = texture->getMipLevelCount(),
			.baseArrayLayer = 0,
			.layerCount = texture->getLayerCount(),
		},
		};
		if (vkCreateImageView(mDevice->getLogicalDevice(), &info, nullptr, &mData->ImageView)!= VK_SUCCESS) {
			Error("Build Vk Image View Failed");
		}
		mData->isBuild = 1;
	}

	void VulkanImageView::destroyCurrent() {
		vkDestroyImageView(mDevice->getLogicalDevice(), mData->ImageView, nullptr);
		mData->isBuild = 0;
	}

	VkImageView_T* VulkanImageView::getImageViewHandle() const {
		return mData->ImageView;
	}

}
