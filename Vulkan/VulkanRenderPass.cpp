#include "VulkanDevice.h"
#include "VulkanRenderPass.h"
#include "ChangeImageFlagsToVulkanFlags.h"
#include <unordered_map>
#include "../../Log/Logger.h"
namespace FISIR{
	


	struct SubpassAttachmentRefs {
		uint64_t ColorMask;
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
		uint32_t ColorEnrtiesRenderAttachmentIndex[64] { 0x3f3f };
		uint32_t ColorEnrtiesResloveAttachmentIndex[64] { 0x3f3f };

		uint32_t DepthStencilAttachmentIndex = -1;
		uint32_t DepthStencilResolveAttachmentIndex = -1;
	};


	void VulkanRenderPass::InputAttachment(const RHIRenderPassInfo& info) {
		for (int i=0; i<64; i++) if (info.ColorEntries[i].EntryPros.exeit) {
			//Color
			auto& pros = info.ColorEntries[i].EntryPros;
			VkAttachmentDescription attachmentDesc{
			  .format = getVulkanFormat(pros.colorType),
			  .samples = getVulkanSampleCount(pros.sampleCount),
			  .loadOp = getVulkanLoadOp(pros.loadOp),
			  .storeOp = getVulkanStoreOp(pros.storeOp),
			  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			  .initialLayout = getVulkanImageLayout(pros.initLayout),
			  .finalLayout = getVulkanImageLayout(pros.dstLayout)
			};
			mData->ColorEnrtiesRenderAttachmentIndex[i] = attachmentDescriptions.size();
			attachmentDescriptions.push_back(attachmentDesc);

			if (pros.hasResolveTarget) {
				VkAttachmentDescription attachmentDesc{
					.format = getVulkanFormat(pros.resolveColorType),
					.samples = getVulkanSampleCount(pros.resolveSampleCount),
					.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
					.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				};
				mData->ColorEnrtiesResloveAttachmentIndex[i] = attachmentDescriptions.size();
				attachmentDescriptions.push_back(attachmentDesc);
			}
		}


		//DepthStencil
		auto& target = info.DepthStencilEntry;
		if (target.exeit) {
			VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

			if (target.depthAction.isUsingDepth()) {
				if (target.depthAction.isDepthRead()) {
					depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				}
				else if (target.depthAction.isDepthWrite()) {
					depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				}
				if (target.depthAction.isDepthWrite()) {
					depthStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
				}
			}


			VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			VkAttachmentStoreOp stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

			if (target.depthAction.isUsingStencil()) {
				if (target.depthAction.isStencilRead()) {
					stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				}
				else if (target.depthAction.isStencilWrite()) {
					stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				}
				if (target.depthAction.isStencilWrite()) {
					stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
				}
			}

			VkAttachmentDescription attachmentDesc{
				.format = getVulkanFormat(target.colorType),
				.samples = getVulkanSampleCount(target.sampleCount),
				.loadOp = depthLoadOp,
				.storeOp = depthStoreOp,
				.stencilLoadOp = stencilLoadOp,
				.stencilStoreOp = stencilStoreOp,
				.initialLayout = getVulkanImageLayout(target.initLayout),
				.finalLayout = getVulkanImageLayout(target.dstLayout)
			};
			mData->DepthStencilAttachmentIndex = attachmentDescriptions.size();
			attachmentDescriptions.push_back(attachmentDesc);

			if (target.hasResolveTarget) {
				VkAttachmentDescription attachmentDesc{
					.format = getVulkanFormat(target.resolveColorType),
					.samples = getVulkanSampleCount(target.sampleCount),
					.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				};
				mData->DepthStencilResolveAttachmentIndex = attachmentDescriptions.size();
				attachmentDescriptions.push_back(attachmentDesc);
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

		InputAttachment(renderPassinfo);

		std::vector<VkSubpassDescription> subpasses;
		std::vector<SubpassAttachmentRefs> subpassRefs;

		std::vector<std::vector<VkAttachmentReference>> CollorRefs;
		std::vector<std::vector<VkAttachmentReference>> ResloveRefs;
		std::vector<VkAttachmentReference> DepthStencilRefs;
		std::vector<std::vector<VkAttachmentReference>> InputRefs;

		//subpass
		for (auto& subpass : renderPassinfo.SubPasses) {
			std::vector<VkAttachmentReference> colorRefs;
			std::vector<VkAttachmentReference> resloveRefs;
			std::vector<VkAttachmentReference> inputRefs;

			VkAttachmentReference depthref;
			if (renderPassinfo.DepthStencilEntry.exeit && subpass.UseDepthStencil) {
				depthref.attachment = mData->DepthStencilAttachmentIndex;
				depthref.layout = subpass.ReadDepthAsInput ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
					: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;;
				if (subpass.ReadDepthAsInput) {
					inputRefs.push_back(depthref);
				}
				else {
					depthref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				}
				DepthStencilRefs.push_back(depthref);
			}
			uint32_t mask = subpass.ColorEntryMask;
			for (uint32_t i = 0; mask; i++) if (mask & (1u << i) && renderPassinfo.ColorEntries[i].EntryPros.exeit) {
				mask &= ~(1u << i);
				auto& target = renderPassinfo.ColorEntries[i];
				auto& pros = renderPassinfo.ColorEntries[i].EntryPros;
				VkAttachmentReference ref {
					.attachment = mData->ColorEnrtiesRenderAttachmentIndex[i],
					.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				};
				colorRefs.push_back(ref);
				
				if (pros.hasResolveTarget) {
					VkAttachmentReference ref {
						.attachment = mData->ColorEnrtiesResloveAttachmentIndex[i],
						.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
					};
					resloveRefs.push_back(ref);
				}

			}
			CollorRefs.push_back(std::move(colorRefs));
			InputRefs.push_back(std::move(inputRefs));
			ResloveRefs.push_back(std::move(resloveRefs));

			VkSubpassDescription subpassDesc{
				.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.inputAttachmentCount = (uint32_t)InputRefs.back().size(),
				.pInputAttachments = inputRefs.empty() ? nullptr : InputRefs.back().data(),
				.colorAttachmentCount = (uint32_t)CollorRefs.back().size(),
				.pColorAttachments = CollorRefs.back().data(),
				.pResolveAttachments = ResloveRefs.back().data(),
				.pDepthStencilAttachment = renderPassinfo.DepthStencilEntry.exeit ? &DepthStencilRefs.back() : nullptr,
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
		}
	}
}
