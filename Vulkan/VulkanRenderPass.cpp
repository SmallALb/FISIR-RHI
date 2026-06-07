#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "VulkanRenderPass.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include "../../Log/Logger.h"
namespace FISIR{
	

   #include "ChangeImageFlagsToVulkanFlags.h"

	struct SubpassAttachmentRefs {
		uint32_t ColorMask;
		bool UseDepthStencil;
		bool ReadDepthAsInput;

	};

	struct SubpassDependencyInfo {
		uint32_t SrcSubpass;
		uint32_t DstSubpass;
		VkPipelineStageFlags SrcStageMask;
		VkPipelineStageFlags DstStageMask;
		VkAccessFlags SrcAccessMask;
		VkAccessFlags DstAccessMask;
		VkDependencyFlags DependencyFlags;
	};

	static std::vector<SubpassDependencyInfo> ComputeSubpassDependencies(const std::vector<SubpassAttachmentRefs>& subpassAttachmentRefs) {
		std::vector<SubpassDependencyInfo> dependencies;
		
		uint32_t numSubpasses = (uint32_t)subpassAttachmentRefs.size();

		if (numSubpasses < 2) return dependencies;

		for (uint32_t i=1; i<numSubpasses; i++) {
			const auto& prev = subpassAttachmentRefs[i - 1];
			const auto& curr = subpassAttachmentRefs[i];

			VkPipelineStageFlags srcStage = 0;
			VkPipelineStageFlags dstStage = 0;
			VkAccessFlags srcAccess = 0;
			VkAccessFlags dstAccess = 0;
			bool hasDependency = false;

			uint32_t overlappingColors = prev.ColorMask & curr.ColorMask;
			if (overlappingColors != 0) {
				srcStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dstStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				srcAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				dstAccess |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
				hasDependency = true;
			}

			if (prev.UseDepthStencil && curr.UseDepthStencil) {
				if (!prev.ReadDepthAsInput && curr.ReadDepthAsInput) {
					srcStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
					dstStage |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					srcAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
					dstAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
					hasDependency = true;
				}
				
				if (!prev.ReadDepthAsInput && !curr.ReadDepthAsInput) {
					srcStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
					dstStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
					srcAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
					dstAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
					hasDependency = true;
				}

				if (prev.ReadDepthAsInput && !curr.ReadDepthAsInput) {
					srcStage |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					dstStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
					srcAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
					dstAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
					hasDependency = true;
				}
			}

			if (hasDependency) {
				SubpassDependencyInfo dependency{
					.SrcSubpass = i - 1,
					.DstSubpass = i,
					.SrcStageMask = srcStage,
					.DstStageMask = dstStage,
					.SrcAccessMask = srcAccess,
					.DstAccessMask = dstAccess,
					.DependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
				};
				dependencies.push_back(dependency);	
			}

		}

		return dependencies;
	}


	struct __VKRenderPassData {
		VkRenderPass renderpass;
	};

	void VulkanRenderPass::InputAttachment(std::vector<VkAttachmentDescription>& Attachments, std::unordered_map<RHITexture*, uint32_t>& TextureToAttachmentIndex, const RHIRenderPassInfo& info) {
		auto mask = info.ColorEntriesMask;
		for (uint32_t i = 0; mask != 0 && i <= TextureUseForAll; i++) if ((mask & (1u << i))) {
			mask &= ~(1u << i);
			//Color
			auto& Colorentry = info.mColorEntries[i];
			if (Colorentry.RenderTarget && !TextureToAttachmentIndex.contains(Colorentry.RenderTarget)) {
				VkAttachmentDescription attachmentDesc{
				  .format = getVulkanFormat(Colorentry.RenderTarget->getColorType()),
				  .samples = getVulkanSampleCount(Colorentry.RenderTarget->getSampleCount()),
				  .loadOp = getVulkanLoadOp(Colorentry.loadOp),
				  .storeOp = getVulkanStoreOp(Colorentry.storeOp),
				  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				  .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				};
				TextureToAttachmentIndex[Colorentry.RenderTarget] = Attachments.size();
				Attachments.push_back(attachmentDesc);
			}
			if (Colorentry.ResolveTarget && !TextureToAttachmentIndex.contains(Colorentry.ResolveTarget)) {
				VkAttachmentDescription attachmentDesc{
					.format = getVulkanFormat(Colorentry.ResolveTarget->getColorType()),
					.samples = getVulkanSampleCount(Colorentry.ResolveTarget->getSampleCount()),
					.loadOp = getVulkanLoadOp(Colorentry.loadOp),
					.storeOp = getVulkanStoreOp(Colorentry.storeOp),
					.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				};
				TextureToAttachmentIndex[Colorentry.ResolveTarget] = Attachments.size();
				Attachments.push_back(attachmentDesc);
			}

			//DepthStencil
			auto& DepthStencilentry = info.mDepthStencilEntry;
			if (DepthStencilentry.RenderTarget && !TextureToAttachmentIndex.contains(DepthStencilentry.RenderTarget)) {
				VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

				if (DepthStencilentry.DepthAction.isUsingDepth()) {
					if (DepthStencilentry.DepthAction.isDepthRead()) {
						depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
					}
					else if (DepthStencilentry.DepthAction.isDepthWrite()) {
						depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
					}
					if (DepthStencilentry.DepthAction.isDepthWrite()) {
						depthStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
					}
				}


				VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				VkAttachmentStoreOp stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

				if (DepthStencilentry.DepthAction.isUsingStencil()) {
					if (DepthStencilentry.DepthAction.isStencilRead()) {
						stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
					}
					else if (DepthStencilentry.DepthAction.isStencilWrite()) {
						stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
					}
					if (DepthStencilentry.DepthAction.isStencilWrite()) {
						stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
					}
				}

				VkAttachmentDescription attachmentDesc{
					.format = getVulkanFormat(DepthStencilentry.RenderTarget->getColorType()),
					.samples = getVulkanSampleCount(DepthStencilentry.RenderTarget->getSampleCount()),
					.loadOp = depthLoadOp,
					.storeOp = depthStoreOp,
					.stencilLoadOp = stencilLoadOp,
					.stencilStoreOp = stencilStoreOp,
					.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				};
				TextureToAttachmentIndex[DepthStencilentry.RenderTarget] = Attachments.size();
				Attachments.push_back(attachmentDesc);
			}

			if (DepthStencilentry.RenderTarget && DepthStencilentry.ResolveTarget && !TextureToAttachmentIndex.contains(DepthStencilentry.ResolveTarget)) {
				VkAttachmentDescription attachmentDesc{
					.format = getVulkanFormat(DepthStencilentry.ResolveTarget->getColorType()),
					.samples = getVulkanSampleCount(DepthStencilentry.ResolveTarget->getSampleCount()),
					.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				};
				TextureToAttachmentIndex[DepthStencilentry.ResolveTarget] = Attachments.size();
				Attachments.push_back(attachmentDesc);
			}
		}
	}

	RenderPass_t VulkanRenderPass::getRenderPassHandle() {
		return mData->renderpass;
	}

	VulkanRenderPass::~VulkanRenderPass() {
		vkDestroyRenderPass(mDevice->getLogicalDevice(), mData->renderpass, nullptr);
		delete mData;
	}



	VulkanRenderPass::VulkanRenderPass(VulkanDevice* device, const RHIRenderPassInfo& renderPassinfo) {
		mData = new __VKRenderPassData();
		mDevice = device;

		std::vector<VkAttachmentDescription> attachmentDescriptions;
		std::unordered_map<RHITexture*, uint32_t> textureToAttachmentIndex;
		InputAttachment(attachmentDescriptions, textureToAttachmentIndex, renderPassinfo);

		std::vector<VkSubpassDescription> subpasses;
		std::vector<SubpassAttachmentRefs> subpassRefs;

		std::vector<std::vector<VkAttachmentReference>> CollorRefs;
		std::vector<VkAttachmentReference> DepthStencilRefs;
		std::vector<std::vector<VkAttachmentReference>> InputRefs;

		//subpass
		for (auto& subpass : renderPassinfo.mSubPasses) {
			std::vector<VkAttachmentReference> colorRefs;
			std::vector<VkAttachmentReference> inputRefs;
			uint32_t mask = subpass.ColorEntryMask;
			for (uint32_t i = 0; mask != 0 && i <= TextureUseForAll; i++) if ((mask & (1u << i))) {
				mask &= ~(1u << i);
				auto& Colorentry = renderPassinfo.mColorEntries[i];
				if (Colorentry.RenderTarget) {
					VkAttachmentReference ref {
						.attachment = textureToAttachmentIndex[Colorentry.RenderTarget],
						.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
					};
					colorRefs.push_back(ref);
				}
				
				if (Colorentry.ResolveTarget) {
					VkAttachmentReference ref {
						.attachment = textureToAttachmentIndex[Colorentry.ResolveTarget],
						.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
					};
					colorRefs.push_back(ref);
				}
				VkAttachmentReference depthref;
				if (renderPassinfo.mDepthStencilEntry.RenderTarget) {
					depthref.attachment = textureToAttachmentIndex[renderPassinfo.mDepthStencilEntry.RenderTarget];
					if (subpass.ReadDepthAsInput){
						depthref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
						inputRefs.push_back(depthref);
					}
					else {
						depthref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					}
				}
				
				CollorRefs.push_back(std::move(colorRefs));
				DepthStencilRefs.push_back(depthref);
				InputRefs.push_back(std::move(inputRefs));

			}
			VkSubpassDescription subpassDesc{
				.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.inputAttachmentCount = (uint32_t)InputRefs.back().size(),
				.pInputAttachments = inputRefs.empty() ? nullptr : InputRefs.back().data(),
				.colorAttachmentCount = (uint32_t)CollorRefs.back().size(),
				.pColorAttachments = CollorRefs.back().data(),
				.pDepthStencilAttachment = renderPassinfo.mDepthStencilEntry.RenderTarget ? &DepthStencilRefs.back() : nullptr,
			};
			subpasses.push_back(subpassDesc);

			SubpassAttachmentRefs subpassRef{
				.ColorMask = subpass.ColorEntryMask,
				.UseDepthStencil = subpass.UseDepthStencil,
				.ReadDepthAsInput = subpass.ReadDepthAsInput
			};
			subpassRefs.push_back(subpassRef);
		}

		//dependency
		std::vector<VkSubpassDependency> dependencies;

		auto deps = ComputeSubpassDependencies(subpassRefs);

		if (!subpasses.empty()) {
			VkSubpassDependency dependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL,
				.dstSubpass = 0,
				.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				.srcAccessMask = 0,
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			};
			dependencies.push_back(dependency);
		}
		
		for (auto& d : deps) {
			VkSubpassDependency dependency{
				.srcSubpass = d.SrcSubpass,
				.dstSubpass = d.DstSubpass,
				.srcStageMask = d.SrcStageMask,
				.dstStageMask = d.DstStageMask,
				.srcAccessMask = d.SrcAccessMask,
				.dstAccessMask = d.DstAccessMask,
				.dependencyFlags = d.DependencyFlags
			};
			dependencies.push_back(dependency);
		}

		VkRenderPassCreateInfo renderPassCreateInfo{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = (uint32_t)attachmentDescriptions.size(),
			.pAttachments = attachmentDescriptions.data(),
			.subpassCount = (uint32_t)subpasses.size(),
			.pSubpasses = subpasses.data(),
			.dependencyCount = (uint32_t)dependencies.size(),
			.pDependencies = dependencies.data()
		};

		if (vkCreateRenderPass(mDevice->getLogicalDevice(), &renderPassCreateInfo, nullptr, &mData->renderpass) != VK_SUCCESS) {
			Error("Failed to create Vulkan Render Pass!");
		}
	}
}
