#pragma once

#include "../RHIRenderPass.h"
#include <unordered_map>

namespace FISIR {
	class VulkanDevice;
	
	struct __VKRenderPassData;

	class VulkanRenderPass : public RHIRenderPass {
		void InputAttachment(const RHIRenderPassInfo& info);
	
	public:
		virtual RenderPass_t getRenderPassHandle() override;

		virtual ~VulkanRenderPass();

		VulkanRenderPass(VulkanDevice* device, const RHIRenderPassInfo& renderPassinfo);
		
		
		virtual TextureLayout getAttachmentFinalLayout(uint32_t index) const override;

		virtual uint32_t getAttachmentCount() const override;

		__VKRenderPassData* mData;
		VulkanDevice* mDevice;
	};

}
