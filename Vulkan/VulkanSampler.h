#pragma once 

#include "../RHISampler.h"
#include "VulkanResourceInterface.h"

namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;
	struct __VkSamplerData;

	class VulkanSampler : public RHISampler, public VulkanResource {
	public:
		VulkanSampler(VulkanDevice* device, const SamplerInfo& info);
		
		~VulkanSampler();

		virtual void* getResourceAPIHandle() const override;

		virtual uint32_t getVkDescriptorType() const override;

		virtual SamplerInfo getSamplerInfo() const override;


	private:
		virtual void* changeOtherHandle(const std::type_info& typ) override;

		SamplerInfo mSamplerInfo;

		VulkanDevice* mDevice;

		__VkSamplerData* mData;
	};

}