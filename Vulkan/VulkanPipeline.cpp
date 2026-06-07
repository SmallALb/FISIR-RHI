#include "VulkanPipeline.h"
#include "VulkanDevice.h"
#include "VulkanShader.h"
#include "VulkanRHI.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <string>



namespace FISIR{
	extern std::unordered_map<PieplineLayoutHash, VkPipelineLayout_T*>& getPipelineLayoutMap();


	static VkDescriptorType choiceDescriptorType(RHIDescriptorTyp typ) {
		switch (typ) {
		case RHIDescriptorTyp::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
		case RHIDescriptorTyp::Image: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		case RHIDescriptorTyp::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		}
		return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	}

	static VkShaderStageFlags  ChoiceDescriptorStage(RHIUsingStage stage) {
		VkShaderStageFlags res = 0;
		if ((stage & VertexShaderStage)) res |= VK_SHADER_STAGE_VERTEX_BIT;
		if ((stage & FragmentShaderStage)) res |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if ((stage & ComputeShaderStage)) res |= VK_SHADER_STAGE_COMPUTE_BIT;
		return res;
	}


	static uint32_t getDataTypeSize(RHIBaseDataTYPE typ) {
		switch (typ) {
		case _FLoat:
		case _Int:
			return 4;

		case _Fvec2:
		case _Ivec2:
			return 2 * 4;

		case _Fvec3:
		case _Ivec3:
			return 3 * 4;

		case _Fvec4:
		case _Ivec4:
			return 4 * 4;
		default:
			return 0;
		}
	}



	static VkFormat getDataTypeFormat(RHIBaseDataTYPE typ) {
		switch(typ) {
			case _FLoat:
				return VK_FORMAT_R32_SFLOAT;
			case _Fvec2:
				return VK_FORMAT_R32G32_SFLOAT;
			case _Fvec3:
				return VK_FORMAT_R32G32B32_SFLOAT;
			case _Fvec4:
				return VK_FORMAT_R32G32B32A32_SFLOAT;

			case _Int:
				return VK_FORMAT_R32_SINT;
			case _Ivec2:
				return VK_FORMAT_R32G32_SINT;	
			case _Ivec3:
				return VK_FORMAT_R32G32B32_SINT;
			case _Ivec4:
				return VK_FORMAT_R32G32B32A32_SINT;

			default:
				return VK_FORMAT_R32_SFLOAT;
		}
	}

	static VkPrimitiveTopology getPrimitiveTopology(TopologyType typ) {
		switch (typ) {
		case TopologyType::Line:
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		case TopologyType::LineStrip:
			return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		case TopologyType::Point:
			return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		case TopologyType::Triangle:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		case TopologyType::TriangleStrip:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		case TopologyType::TriangleFan:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		}
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}

	static VkPolygonMode getPolygonMode(PolygonMode mode) {
		switch(mode) {
			case PolygonMode::Fill:
				return VK_POLYGON_MODE_FILL;
			case PolygonMode::Line:
				return VK_POLYGON_MODE_LINE;
			case PolygonMode::Point:
				return VK_POLYGON_MODE_POINT;
		}
		return VK_POLYGON_MODE_FILL;
	}
	
	static VkCompareOp getCmpOP(APIOperation op) {
		switch(op) {
			case _NOT_Equal_: return VK_COMPARE_OP_NOT_EQUAL;
			case _Equal_: return VK_COMPARE_OP_EQUAL;
			case _Equal_Greate_: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case _Equal_Less_: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case _Greate_: return VK_COMPARE_OP_GREATER;
			case _Less_: return VK_COMPARE_OP_LESS;
			case _Always_: return VK_COMPARE_OP_MAX_ENUM;
		
		}
		return VK_COMPARE_OP_MAX_ENUM;
	}


	VkDynamicState DynamicState[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_DEPTH_BIAS,
	};

	struct __VKPipelineData {
		VkDescriptorSetLayout Descriptorlayout;
		VkPipelineLayout PipelineLayout;
		VkPipeline mPipeline;
	};
	
	
	VulkanPipeline::VulkanPipeline(VulkanDevice* inDevice, VulkanDescriptorPool* DescriptorPool, const RHIPipelineState& State) :
		mDevice(inDevice)
	{
		auto& PipelineLayoutMap = getPipelineLayoutMap();
		mData = new __VKPipelineData();
		//DesLayout
		mData->Descriptorlayout = DescriptorPool->createDescriptorSetLayout(State.describeInfo);

		//PushConstantRange
		VkPushConstantRange PushConstantRange = {
		  .stageFlags = ChoiceDescriptorStage(State.constantRange.Stage),
		  .offset = 0,
		  .size = State.constantRange.bufferSize,
		};

		//PipelineLayout
		PieplineLayoutHash HashVal(State.describeInfo, State.constantRange);

		if (PipelineLayoutMap.contains(HashVal)) mData->PipelineLayout = PipelineLayoutMap[HashVal];
		else {
			VkPipelineLayoutCreateInfo PipelineLayoutInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &mData->Descriptorlayout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &PushConstantRange,
			};
			vkCreatePipelineLayout(mDevice->getLogicalDevice(), &PipelineLayoutInfo, nullptr, &mData->PipelineLayout);
			PipelineLayoutMap[HashVal] = mData->PipelineLayout;
		}


		//Vertext Input
		std::vector<VkVertexInputAttributeDescription> attributes;
		uint32_t stride = 0;
		for (uint32_t i = 0; i<State.vertexInfo.Count; i++) {
			VkVertexInputAttributeDescription attribute {
				.location = i,
				.binding =0,
				.format = getDataTypeFormat(State.vertexInfo[i]),
				.offset = stride
			};
			stride += getDataTypeSize(State.vertexInfo[i]);
			attributes.push_back(attribute);
		}

		VkVertexInputBindingDescription BindingDescriptioninfo {
			.binding = 0,
			.stride = stride,
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		VkDeviceSize offsetSize[] = {0};

		VkPipelineVertexInputStateCreateInfo ISinfo {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &BindingDescriptioninfo,
			.vertexAttributeDescriptionCount = (uint32_t)attributes.size(),
			.pVertexAttributeDescriptions = attributes.data()
		};

		//Dynamic
		VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 3,
			.pDynamicStates = DynamicState
		};

		//Viewport
		VkPipelineViewportStateCreateInfo viewportStateCreateInfo {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.pViewports = nullptr,
			.scissorCount = 1,
			.pScissors = nullptr
		};

		//InputAssembly
		VkPipelineInputAssemblyStateCreateInfo pipelineIAStateCreateInfo {
		  .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		  .topology = getPrimitiveTopology(State.topologyType),
		};

		//Rasterization
		VkPipelineRasterizationStateCreateInfo pipelineRasterizationState {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.depthClampEnable = State.rasterizationState.DepthClipEnable,
			.rasterizerDiscardEnable = State.rasterizationState.RasterizerDiscardEnable,
			.polygonMode = getPolygonMode(State.rasterizationState.Polygon),
			.cullMode = (VkCullModeFlags)State.rasterizationState.Cull,
			.frontFace = (VkFrontFace)State.rasterizationState.Front,
			.depthBiasEnable = State.rasterizationState.DepthOffsetEnable,
			.lineWidth = 1.0f,
		};

		//Multisample
		VkPipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo {
		  .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		  .rasterizationSamples = (VkSampleCountFlagBits)State.multiSampleState.SamplerBit,
		  .sampleShadingEnable = State.multiSampleState.ShadingEnable,
		  .minSampleShading = 1.0f,
		  .pSampleMask = nullptr,
		  .alphaToCoverageEnable = State.multiSampleState.alpthaToCoverageEnable,
		  .alphaToOneEnable = State.multiSampleState.alpthaToOneEnable
		};

		//DepthSetncil
		VkPipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo {
		  .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		  .depthTestEnable = State.depthStencilState.DepthTestEnable,
		  .depthWriteEnable = State.depthStencilState.DepthWriteEnable,
		  .depthCompareOp = getCmpOP(State.depthStencilState.DepthCmpOp),
		  .depthBoundsTestEnable = State.depthStencilState.DepthBoundsTestEnable,
		  .stencilTestEnable = State.depthStencilState.StencilTestEnable,
		  .minDepthBounds = 0.0f,
		  .maxDepthBounds = 1.0f,
		};

		//PipelineColorBlend
		VkPipelineColorBlendAttachmentState pipelineColorBlendAttachmentState = {
		  .blendEnable = State.colorblendState.ColorBlenEnable,
		  .colorWriteMask = (VkColorComponentFlags)State.colorblendState.UsingColorBit,
		};

		VkPipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo = {
		  .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		  .logicOpEnable = VK_FALSE,
		  .attachmentCount = 1,
		  .pAttachments = &pipelineColorBlendAttachmentState,
		  .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
		};

		//Shader
		std::vector<VkPipelineShaderStageCreateInfo> shaderInfos;
		if (!State.isComputePipeline) {
			for (uint32_t i = 0; i < ShaderTYPCOUNT; i++) if (State.Shaders[i]) {
				VkShaderStageFlagBits usingStage;
				switch (i) {
					case __VERTEXSHADER__: usingStage = VK_SHADER_STAGE_VERTEX_BIT; break;
					case __FRAGMENTSHADER__: usingStage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
					case __GEOMETRY__: usingStage = VK_SHADER_STAGE_GEOMETRY_BIT; break;
				}

				VkPipelineShaderStageCreateInfo info {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.stage = usingStage,
					.module = (VkShaderModule)State.Shaders[i]->getResourceAPIHandle(),
					.pName = "main"
				};
				shaderInfos.push_back(info);
			}
		}
		else {
			VkPipelineShaderStageCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = (VkShaderModule)State.Shaders[__COMPUTESHADER__]->getResourceAPIHandle(),
				.pName = "main"
			};
			shaderInfos.push_back(info);
		}

		//Create Pipeline
		if (!State.isComputePipeline) {
			VkGraphicsPipelineCreateInfo info = {
				.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
				.stageCount = (uint32_t)shaderInfos.size(),
				.pStages = shaderInfos.data(),
				.pVertexInputState = &ISinfo,
				.pInputAssemblyState = &pipelineIAStateCreateInfo,
				.pViewportState = &viewportStateCreateInfo,
				.pRasterizationState = &pipelineRasterizationState,
				.pMultisampleState = &pipelineMultisampleStateCreateInfo,
				.pDepthStencilState = &pipelineDepthStencilStateCreateInfo,
				.pColorBlendState = &pipelineColorBlendStateCreateInfo,
				.pDynamicState = &pipelineDynamicStateCreateInfo,
				.layout = mData->PipelineLayout,
				.renderPass = (VkRenderPass)State.renderpass->getRenderPassHandle(),
				.basePipelineIndex = -1,
			};
			vkCreateGraphicsPipelines(mDevice->getLogicalDevice(), nullptr, 1, &info, nullptr, &mData->mPipeline);
			
			if (mDevice->isDescriptorHeapSupported()) info.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

		}
		else {
			VkComputePipelineCreateInfo info = {
			  .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			  .stage = shaderInfos[0],
			  .layout = mData->PipelineLayout,
			};
			vkCreateComputePipelines(mDevice->getLogicalDevice(), nullptr, 1, &info, nullptr, &mData->mPipeline);

			if (mDevice->isDescriptorHeapSupported()) info.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
		}
	}

	VulkanPipeline::~VulkanPipeline() {
		delete mData;
	}
	

	Pipeline_t VulkanPipeline::getPipelineHandle() {
		return mData->mPipeline;
	}

}
