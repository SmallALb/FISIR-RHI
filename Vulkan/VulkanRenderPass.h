#pragma once

#include "../RHIRenderPass.h"
#include "../RHITexture.h"
#include <unordered_map>
struct VkAttachmentDescription;

namespace FISIR {
	class VulkanDevice;
	
	struct __VKRenderPassData;

	class VulkanRenderPass : public RHIRenderPass {
		void InputAttachment(std::vector<VkAttachmentDescription>& Attachments, std::unordered_map<RHITexture*, uint32_t>& TextureToAttachmentIndex, const RHIRenderPassInfo& info);
	
	public:
		virtual RenderPass_t getRenderPassHandle() override;

		virtual ~VulkanRenderPass();

		VulkanRenderPass(VulkanDevice* device, const RHIRenderPassInfo& renderPassinfo);
	
		__VKRenderPassData* mData;
		VulkanDevice* mDevice;
	};

}
