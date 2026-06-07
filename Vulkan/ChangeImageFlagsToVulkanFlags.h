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


static VkSampleCountFlagBits getVulkanSampleCount(uint32_t sampleCount) {
	switch (sampleCount) {
	case 1:
		return VK_SAMPLE_COUNT_1_BIT;
	case 2:
		return VK_SAMPLE_COUNT_2_BIT;
	case 4:
		return VK_SAMPLE_COUNT_4_BIT;
	case 8:
		return VK_SAMPLE_COUNT_8_BIT;
	case 16:
		return VK_SAMPLE_COUNT_16_BIT;
	case 32:
		return VK_SAMPLE_COUNT_32_BIT;
	case 64:
		return VK_SAMPLE_COUNT_64_BIT;
	default:
		return VK_SAMPLE_COUNT_1_BIT;
	}
}


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

static VkAttachmentLoadOp getVulkanLoadOp(RenderTargetLoadAction loadAction) {
	switch (loadAction) {
	case RenderTargetLoadAction::Load:
		return VK_ATTACHMENT_LOAD_OP_LOAD;
	case RenderTargetLoadAction::Clear:
		return VK_ATTACHMENT_LOAD_OP_CLEAR;
	default:
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}
}

static VkAttachmentStoreOp getVulkanStoreOp(RenderTargetStoreAction storeAction) {
	switch (storeAction) {
	case RenderTargetStoreAction::Store:
		return VK_ATTACHMENT_STORE_OP_STORE;
	case RenderTargetStoreAction::MultisampleResolve:
		return VK_ATTACHMENT_STORE_OP_DONT_CARE; // Vulkan does not have a direct equivalent for multisample resolve store operation
	default:
		return VK_ATTACHMENT_STORE_OP_DONT_CARE;
	}
}