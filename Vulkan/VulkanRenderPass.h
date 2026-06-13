#pragma once

#include "../RHIRenderPass.h"
#include <unordered_map>
struct VkAttachmentDescription;

namespace FISIR {
	class VulkanDevice;
	
	struct __VKRenderPassData;

	class VulkanRenderPass : public RHIRenderPass {
		void InputAttachment(const RHIRenderPassInfo& info);
	
	public:
		virtual RenderPass_t getRenderPassHandle() override;

		virtual ~VulkanRenderPass();

		VulkanRenderPass(VulkanDevice* device, const RHIRenderPassInfo& renderPassinfo);
		

		std::vector<VkAttachmentDescription> attachmentDescriptions;
		__VKRenderPassData* mData;
		VulkanDevice* mDevice;
	};

}
