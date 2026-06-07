#pragma once

#include "../RHIPipeline.h"

namespace FISIR{
	class VulkanDescriptorPool;
	class VulkanDevice;

	struct __VKPipelineData;
	
	class VulkanPipeline : public RHIPipeline {
		friend class VulkanRHI;

	private:
		VulkanPipeline(VulkanDevice* device, VulkanDescriptorPool* DescriptorPool, const RHIPipelineState& State);
		
	public:

		virtual ~VulkanPipeline(); 
	


		virtual Pipeline_t getPipelineHandle() override;

	private:

		__VKPipelineData* mData;
		VulkanDevice* mDevice;
	};



}
