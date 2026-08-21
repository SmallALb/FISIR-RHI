#include "VulkanTexture.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDebugNameSet.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"

namespace FISIR {
	struct __VkTextureData {
		TextureSize size;
		uint16_t mipLevels;
		uint16_t arrayLayers;
		VkImage image;
		GpuBlock* mBlock;
		TextureCOLORType colorType;
		TextureType type;
		std::atomic<VkImageLayout> currentLayout;
		TextureUseForFlags useFor;
		uint32_t sampleCount;
		bool NoNeedRelease{0};
	};

	static VkImageLayout getVulkanImageLayout(TextureLayout layout) {
		switch (layout) {
		case TextureLayout::Undefined:
			return VK_IMAGE_LAYOUT_UNDEFINED;
		case TextureLayout::ColorAttachmentOptimal:
			return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case TextureLayout::DepthStencilAttachmentOptimal:
			return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		case TextureLayout::ShaderReadOnlyOptimal:
			return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case TextureLayout::TransferSrcOptimal:
			return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		case TextureLayout::TransferDstOptimal:
			return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		case TextureLayout::Storage:
			return VK_IMAGE_LAYOUT_GENERAL;
		default:
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	static VkImageUsageFlags getVulkanImageUsage(TextureUseForFlags	flags) {
		VkImageUsageFlags usage = 0;
		if (flags & TextureUseFor::TextureUseForColorAttachment) {
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
		if (flags & TextureUseFor::TextureUseForDepthStencilAttachment) {
			usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		if (flags & TextureUseFor::TextureUseForShaderReadOnly) {
			usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if (flags & TextureUseFor::TextureUseForTransferSrc) {
			usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if (flags & TextureUseFor::TextureUseForTransferDst) {
			usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		if (flags & TextureUseFor::TextureUseForStorage) {
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if (flags & TextureUseFor::TextureUseForInputAttachment) {
			usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
		}
		return usage;
	}
	
	static TextureLayout getFormVulkanImageLayout(VkImageLayout layout) {
		switch (layout) {
		case VK_IMAGE_LAYOUT_UNDEFINED:
			return TextureLayout::Undefined;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return TextureLayout::ColorAttachmentOptimal;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return TextureLayout::DepthStencilAttachmentOptimal;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return TextureLayout::ShaderReadOnlyOptimal;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			return TextureLayout::TransferSrcOptimal;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			return TextureLayout::TransferDstOptimal;
		case VK_IMAGE_LAYOUT_GENERAL:
			return TextureLayout::Storage;
		default:
			return TextureLayout::Undefined;
		}
	}


	static VkFormat getVulkanFormat(TextureCOLORType type) {
		switch (type) {
		case TextureCOLORType::RGB_8:
			return VK_FORMAT_R8G8B8_UNORM;
		case TextureCOLORType::RGB_16:
			return VK_FORMAT_R16G16B16_UNORM;
		case TextureCOLORType::RGB_32:
			return VK_FORMAT_R32G32B32_SFLOAT;
		case TextureCOLORType::RGBA_8:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case TextureCOLORType::RGBA_16:
			return VK_FORMAT_R16G16B16A16_UNORM;
		case TextureCOLORType::RGBA_32:
			return VK_FORMAT_R32G32B32A32_SFLOAT;
		case TextureCOLORType::R_8:
			return VK_FORMAT_R8_UNORM;
		case TextureCOLORType::Depth24_Stencil8:
			return VK_FORMAT_D24_UNORM_S8_UINT;
		default:
			return VK_FORMAT_UNDEFINED;
		}
	}

	static TextureCOLORType getTextureColorTypeFromVkFormat(VkFormat format) {
		switch (format) {
		case VK_FORMAT_R8G8B8A8_UNORM: return TextureCOLORType::RGBA_8;
		case VK_FORMAT_R16G16B16A16_UNORM: return TextureCOLORType::RGBA_16;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return TextureCOLORType::RGBA_32;
		case VK_FORMAT_R8G8B8_UNORM: return TextureCOLORType::RGB_8;
		case VK_FORMAT_D24_UNORM_S8_UINT: return TextureCOLORType::Depth24_Stencil8;
		default: return TextureCOLORType::RGBA_8;
		}
	}

	static VkSampleCountFlagBits getVulkanSampleCount(uint32_t sampleCount) {
		switch (sampleCount) {
		case 0:
		case 1:  return VK_SAMPLE_COUNT_1_BIT;
		case 2:  return VK_SAMPLE_COUNT_2_BIT;
		case 4:  return VK_SAMPLE_COUNT_4_BIT;
		case 8:  return VK_SAMPLE_COUNT_8_BIT;
		case 16: return VK_SAMPLE_COUNT_16_BIT;
		case 32: return VK_SAMPLE_COUNT_32_BIT;
		case 64: return VK_SAMPLE_COUNT_64_BIT;
		default:
			return VK_SAMPLE_COUNT_1_BIT;
		}
	}

	VulkanTexture::VulkanTexture(VulkanDevice* inDevice, const TextureInfo& info, uint32_t usage, const char* name):
		mDevice(inDevice)
	{
		mData = new __VkTextureData();
		mData->size = info.size;
		mData->colorType = info.colorType;
		mData->type = info.type;
		mData->mipLevels = info.mipLevels;
		mData->arrayLayers = info.arrayLayers;
		mData->sampleCount = info.sampleCount;
		mData->useFor = info.useFor;
		auto Allocator = mDevice->getAllocator();
		size_t imageSize = info.size.width * info.size.height * info.size.depth * getTextureColorTypeSize(info.colorType);
		VkImageCreateInfo imageInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.flags = info.type == TextureType::TEXTUREARRAY ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : (VkImageCreateFlags)0,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = getVulkanFormat(info.colorType),
			.extent = {info.size.width, info.size.height, info.size.depth},
			.mipLevels = mData->mipLevels,
			.arrayLayers = mData->arrayLayers,
			.samples = getVulkanSampleCount(mData->sampleCount),
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = usage != 0  
						? usage 
						: getVulkanImageUsage(info.useFor),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		 };
		 vkCreateImage(mDevice->getLogicalDevice(), &imageInfo, nullptr, &mData->image);
		
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(mDevice->getLogicalDevice(), mData->image, &memReqs);

		 mData->mBlock = Allocator->create(imageSize, memReqs.alignment, MemType::MemTypeDeviceLocal, this);
	
		 setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mData->image, VK_OBJECT_TYPE_IMAGE, name ? name : "VulkanTexture");
	}

	VulkanTexture::VulkanTexture(VulkanDevice* inDevice, VkImage_T* imagehandle, size_t format, const TextureSize& size, bool NoNeedRelease, const char* name) {
		mData = new __VkTextureData();
		mData->size = size;
		mData->image = imagehandle;
		mData->mBlock = nullptr;
		mData->mipLevels = 1;
		mData->arrayLayers = 1;
		mData->sampleCount = 0;
		mData->colorType = getTextureColorTypeFromVkFormat((VkFormat)format);
		mData->type = TextureType::TEXTURE2D;
		mData->currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		mData->useFor = TextureUseForColorAttachment | TextureUseForShaderReadOnly;
		mData->NoNeedRelease = NoNeedRelease;
		if (name) {
			setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mData->image, VK_OBJECT_TYPE_IMAGE, name);
		}
		
	
	}

	VulkanTexture::~VulkanTexture() {
		if (!mData->NoNeedRelease){
			vkDestroyImage(mDevice->getLogicalDevice(), mData->image, nullptr);
			auto Allocator = mDevice->getAllocator();
			Allocator->free(mData->mBlock);
		}
		delete mData;
	}

	void* VulkanTexture::getResourceAPIHandle() const {
		return mData->image;
	}

	TextureSize VulkanTexture::getTextureSize() const {
		return mData->size;
	}

	TextureLayout VulkanTexture::getCurrentLayout() const {
		return getFormVulkanImageLayout(mData->currentLayout);
	}

	uint16_t VulkanTexture::getLayerCount() const {
		return mData->arrayLayers;
	}

	uint16_t VulkanTexture::getMipLevelCount() const {
		return mData->mipLevels;
	}

	void VulkanTexture::transitionLayout(TextureLayout newLayout) {
		mData->currentLayout = getVulkanImageLayout(newLayout);
		Warn("The Image Ox{:x} Layout Become : {}", (size_t)(this), getTextureLayoutName(newLayout));

	}

	TextureCOLORType VulkanTexture::getColorType() const {
		return mData->colorType;
	}

	uint32_t VulkanTexture::getVkDescriptorType() const {
		// 描述符类型必须由创建时的用途决定，而不能随 currentLayout 变化：
		// 资源包在纹理完成布局转换前就可能被创建（例如交换链的 offscreen 纹理），
		// 若按 currentLayout 推断会得到与管线布局不一致的类型，导致 set 与管线不兼容。
		if (mData->useFor & TextureUseFor::TextureUseForStorage)         return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		if (mData->useFor & TextureUseFor::TextureUseForShaderReadOnly)  return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		if (mData->useFor & TextureUseFor::TextureUseForInputAttachment) return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	}

	TextureUseForFlags VulkanTexture::getTextureUseFor() const {
		return mData->useFor;
	}

	TextureType VulkanTexture::getTextureType() const {
		return mData->type;
	}

	uint32_t VulkanTexture::getVkColorType() const {
		return getVulkanFormat(mData->colorType);
	}

	uint32_t VulkanTexture::getVkTextureLayout() const {
		return mData->currentLayout;
	}

	uint32_t VulkanTexture::getSampleCount() const {
		return mData->sampleCount;
	}

}
 