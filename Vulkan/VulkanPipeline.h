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

		// 管线布局句柄：vkCmdPushConstants 需要它（前端只当不透明指针传递）
		virtual void* getPipelineLayoutHandle() override;


		virtual bool isComputePipeline() const override;

	private:
		bool mIsComputePipeline {false};
		__VKPipelineData* mData;
		VulkanDevice* mDevice;
	};



}
