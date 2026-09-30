#pragma once

#include "../RHITypes.h"

struct VkImageView_T;

namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;

	struct __VkImageViewData;

	// 纹理类型 → VkImageViewType（返回值就是 VkImageViewType，用 uint32_t 透传，
	// 避免在本头文件里引入 vulkan.h）。TEXTUREARRAY 映射为 CUBE —— 本后端的
	// 「纹理数组」在 VkImage 侧带 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT，视图即立方体贴图。
	uint32_t getVulkanViewTypeFromTextureType(TextureType typ);

	// 用途位 → VkImageAspectFlags（同样是 uint32_t 透传）。
	uint32_t getVulkanAspectFlagsForUsing(TextureUseForFlags usefor);

	class VulkanImageView {
	public:
		VulkanImageView(VulkanDevice* device, VulkanTexture* texture = nullptr);

		~VulkanImageView();
		
		void build(VulkanTexture* texture);

		void destroyCurrent();

		VkImageView_T* getImageViewHandle() const;
	private:
		__VkImageViewData* mData;
		VulkanDevice* mDevice;
	};



}


