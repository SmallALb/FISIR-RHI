#pragma once
#include "../RHITypes.h"
#include <vulkan/vulkan.h>

namespace FISIR{
	inline VkFormat getVulkanFormat(TextureCOLORType type) {
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


	inline VkSampleCountFlagBits getVulkanSampleCount(uint32_t sampleCount) {
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


	inline VkImageLayout getVulkanImageLayout(TextureLayout layout) {
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
		case TextureLayout::Present:
			return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		}
		return VK_IMAGE_LAYOUT_UNDEFINED;
	}

	inline VkAttachmentLoadOp getVulkanLoadOp(RenderTargetLoadAction loadAction) {
		switch (loadAction) {
		case RenderTargetLoadAction::Load:
			return VK_ATTACHMENT_LOAD_OP_LOAD;
		case RenderTargetLoadAction::Clear:
			return VK_ATTACHMENT_LOAD_OP_CLEAR;
		default:
			return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		}
	}

	inline VkAttachmentStoreOp getVulkanStoreOp(RenderTargetStoreAction storeAction) {
		switch (storeAction) {
		case RenderTargetStoreAction::Store:
			return VK_ATTACHMENT_STORE_OP_STORE;
		case RenderTargetStoreAction::MultisampleResolve:
			return VK_ATTACHMENT_STORE_OP_DONT_CARE; // Vulkan does not have a direct equivalent for multisample resolve store operation
		default:
			return VK_ATTACHMENT_STORE_OP_DONT_CARE;
		}
	}



	inline VkCompareOp getVkOperation(APIOperation op) {
		switch (op) {
		case _NONE_OP_:
			return VK_COMPARE_OP_NEVER;
		case _NOT_Equal_:
			return VK_COMPARE_OP_NOT_EQUAL;
		case _Equal_:
			return VK_COMPARE_OP_EQUAL;
		case _Equal_Less_:
			return VK_COMPARE_OP_LESS_OR_EQUAL;
		case _Equal_Greate_:
			return VK_COMPARE_OP_GREATER_OR_EQUAL;
		case _Less_:
			return VK_COMPARE_OP_LESS;
		case _Greate_:
			return VK_COMPARE_OP_GREATER;
		case _Always_:
			return VK_COMPARE_OP_ALWAYS;
		}
		return VK_COMPARE_OP_NEVER;
	}

	inline VkFilter getVkFilter(SamplerFilter filter) {
		switch (filter) {
		case SamplerFilter::NEAREST:
			return VK_FILTER_NEAREST;
		case SamplerFilter::LINEAR:
			return VK_FILTER_LINEAR;
		case SamplerFilter::CUBIC:
			return VK_FILTER_CUBIC_EXT;
		}
		return VK_FILTER_NEAREST;
	}


	inline VkSamplerMipmapMode getVkMipMapMode(SamplerFilter filter) {
		switch (filter) {
		case SamplerFilter::NEAREST:
			return VK_SAMPLER_MIPMAP_MODE_NEAREST;
		case SamplerFilter::LINEAR:
			return VK_SAMPLER_MIPMAP_MODE_LINEAR;
		}
		return VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}

	inline VkSamplerAddressMode getVkSamplerAddressMode(SamplerOverFoundMode mode) {
		switch (mode) {

		case SamplerOverFoundMode::REPEAT:
			return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		case SamplerOverFoundMode::MIRRORED_REPEAT:
			return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		case SamplerOverFoundMode::CLAMP_TO_EDGE:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case SamplerOverFoundMode::MIRROR_CLAMP_TO_EDGE:
			return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		case SamplerOverFoundMode::CLAMP_TO_BORDER:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		}
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}

}