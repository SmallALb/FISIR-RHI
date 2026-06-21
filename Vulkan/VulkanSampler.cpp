#include "VulkanSampler.h"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "ChangeImageFlagsToVulkanFlags.h"
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
			.maxLod = info.minLop,
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
