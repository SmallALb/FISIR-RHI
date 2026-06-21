#include "VulkanDevice.h"
#include "VulkanImageView.h"
#include "VulkanTexture.h"
#include <vulkan/vulkan.h>
#include "../LockFreeQue.h"
#include "../../Log/Logger.h"
namespace FISIR {
	static VkImageViewType getVulkanViewTypeFromTextureType(TextureType typ) {
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

	static VkImageAspectFlags getVulkanAspectFlagsForUsing(TextureUseForFlags usefor) {
		return usefor & TextureUseForDepthStencilAttachment ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
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
		.viewType = getVulkanViewTypeFromTextureType(texture->getTextureType()),
		.format = (VkFormat)(static_cast<VulkanTexture*>(texture))->getVkColorType(),
		.subresourceRange = {
			.aspectMask = getVulkanAspectFlagsForUsing(texture->getTextureUseFor()),
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
	

	struct __VkImageViewManagerData {
		std::unordered_map<VulkanTexture*, size_t> VulkanTextureToIndex;
		LockFreeQue<size_t> FreeImageViewIndex;
		std::mutex ImageViewsLock;
		std::mutex TextureMapLock;
	};

	
	VulkanImageViewManager::VulkanImageViewManager(VulkanDevice* device):mDevice(device) {
		mData = new __VkImageViewManagerData;
		ImageViews.resize(10, new VulkanImageView(mDevice));
		for (size_t i=0; i<10; i++) mData->FreeImageViewIndex.push(i);
	}

	VulkanImageViewManager::~VulkanImageViewManager() {
		std::lock_guard<std::mutex> lockViews(mData->ImageViewsLock);
		for (auto& view : ImageViews) delete view;
		
	}
	
	VulkanImageView* VulkanImageViewManager::getViewToTexture(VulkanTexture* texture) {
		size_t index = -1;
		{
			std::lock_guard<std::mutex> lockMap(mData->TextureMapLock);
			auto it = mData->VulkanTextureToIndex.find(texture);
			if (it != mData->VulkanTextureToIndex.end()) {
				std::lock_guard<std::mutex> lockViews(mData->ImageViewsLock);
				return ImageViews[it->second];
			}
		}

		if (mData->FreeImageViewIndex.empty() || !mData->FreeImageViewIndex.pop(index)) {
			std::lock_guard<std::mutex> lockViews(mData->ImageViewsLock);
			ImageViews.push_back(new VulkanImageView(mDevice, texture));
			{
				std::lock_guard<std::mutex> lockMap(mData->TextureMapLock);
				mData->VulkanTextureToIndex[texture] = ImageViews.size() - 1;
			}
			return ImageViews.back();
		}

		{
			std::lock_guard<std::mutex> lockMap(mData->TextureMapLock);
			mData->VulkanTextureToIndex[texture] = index;
		}
		VulkanImageView* imageView = nullptr;
		{
			std::lock_guard<std::mutex> lockViews(mData->ImageViewsLock);
			imageView = ImageViews[index];
		}
		imageView->build(texture);
		return imageView;
	}
	
	void VulkanImageViewManager::freeViewToTexture(VulkanTexture* texture) {
		size_t index = -1;
		VulkanImageView* imageView = nullptr;
		{
			std::lock_guard<std::mutex> lockMap(mData->TextureMapLock);
			auto it = mData->VulkanTextureToIndex.find(texture);
			if (it != mData->VulkanTextureToIndex.end())  {
				index = it->second;
				mData->VulkanTextureToIndex.erase(texture);
			}
		}
		if (index == -1) return;
		{
			std::lock_guard<std::mutex> lockViews(mData->ImageViewsLock);
			imageView = ImageViews[index];
		}
		imageView->destroyCurrent();
		mData->FreeImageViewIndex.push(index);

	}
}
