#include "VulkanSampler.h"

#include <vulkan/vulkan.h>

#include "ChangeImageFlagsToVulkanFlags.h"
#include "VulkanDevice.h"
#include "VulkanTexture.h"
namespace FISIR{
	

	struct __VkSamplerData {
		VkSampler Sampler;
	};


	VulkanSampler::VulkanSampler(VulkanDevice* device, const SamplerInfo& info) {
		mDevice = device;
		mData = new __VkSamplerData();
		
		mSamplerInfo = info;

		VkSamplerCreateInfo createinfo {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = getVkFilter(info.enlagerFilter),
			.minFilter = getVkFilter(info.minFilter),
			.mipmapMode = getVkMipMapMode(info.mipMapMode),
			.addressModeU = getVkSamplerAddressMode(info.u),
			.addressModeV = getVkSamplerAddressMode(info.v),
			.addressModeW = getVkSamplerAddressMode(info.w),
			.mipLodBias = info.mipLodBias,
			.anisotropyEnable = info.anisotropyEnable,
			.maxAnisotropy = info.maxAnisotropy,
			.compareEnable = info.compareEnable,
			.compareOp = getVkOperation(info.compareOP),
			.minLod = info.minLop,
			// 曾被误写成 info.minLop：默认 SamplerInfo 的 minLop == maxLop == 1.0 时
			// 看不出差别，但 mipLevels > 1 的纹理（如预滤波环境图）会被钳在 1.0 以内。
			.maxLod = info.maxLop,
			.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
			.unnormalizedCoordinates = info.unNormalized,
		};

		vkCreateSampler(mDevice->getLogicalDevice(), &createinfo, nullptr, &mData->Sampler);
	}

	VulkanSampler::~VulkanSampler() {
		vkDestroySampler(mDevice->getLogicalDevice(), mData->Sampler, nullptr);	
	}

	void* VulkanSampler::getResourceAPIHandle() const {
		return mData->Sampler;
	}

	uint32_t VulkanSampler::getVkDescriptorType() const {
		return VK_DESCRIPTOR_TYPE_SAMPLER;
	}

	SamplerInfo VulkanSampler::getSamplerInfo() const {
		return mSamplerInfo;
	}

	void* VulkanSampler::changeOtherHandle(const std::type_info& typ) {
		if (typ == typeid(VulkanResource))
			return static_cast<VulkanResource*>(this);
	}


}
