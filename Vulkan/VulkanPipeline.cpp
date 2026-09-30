#include "VulkanPipeline.h"

#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "VulkanDevice.h"
#include "VulkanRHI.h"
#include "VulkanShader.h"



namespace FISIR{
	extern std::unordered_map<PieplineLayoutHash, VkPipelineLayout_T*>& getPipelineLayoutMap();



	static uint32_t GetDescriptorSize(VulkanDevice* device, VkDescriptorType type) {
		const auto& sizes = device->getHeapSizeInfo();
		switch (type) {
			// --- Buffer 类：统一用 bufferDescriptorSize ---
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			return sizes.bufferDescriptorSize;

			// --- Image 类：统一用 imageDescriptorSize ---
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			return sizes.imageDescriptorSize;

			// --- Sampler 类 ---
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			return sizes.samplerDescriptorSize;

			// --- Combined Image Sampler = Image + Sampler 拼在一起 ---
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			return sizes.imageDescriptorSize + sizes.samplerDescriptorSize;

			// --- 加速结构：规范里和 Buffer 一样 ---
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			return sizes.bufferDescriptorSize;

		default:
			return 0;
		}
	}

	static VkDescriptorType choiceDescriptorType(RHIDescriptorTyp typ) {
		switch (typ) {
		case RHIDescriptorTyp::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
		case RHIDescriptorTyp::Image: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		case RHIDescriptorTyp::RWImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		case RHIDescriptorTyp::SamplerImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		case RHIDescriptorTyp::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case RHIDescriptorTyp::RBuffer:
		case RHIDescriptorTyp::RWBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		}
		return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	}

	static VkShaderStageFlags  ChoiceDescriptorStage(RHIUsingStageFlags stage) {
		Debug("ChoiceDescriptorStage called with stage = {}", (uint32_t)stage);
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

		case _UByte4Norm:
			return 4;
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

			case _UByte4Norm:
				return VK_FORMAT_R8G8B8A8_UNORM;

			default:
				return VK_FORMAT_R32_SFLOAT;
		}
	}

	static VkBlendFactor getBlendFactor(BlendFactor factor) {
		switch (factor) {
		case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
		case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
		case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
		case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
		case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
		case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
		case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		default:                            return VK_BLEND_FACTOR_ONE;
		}
	}

	static VkBlendOp getBlendOp(BlendOp op) {
		switch (op) {
		case BlendOp::Add:             return VK_BLEND_OP_ADD;
		case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
		case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
		case BlendOp::Min:             return VK_BLEND_OP_MIN;
		case BlendOp::Max:             return VK_BLEND_OP_MAX;
		default:                       return VK_BLEND_OP_ADD;
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
		std::vector<uint32_t> bindingRemap;

	};

	static std::vector<VkDescriptorSetAndBindingMappingEXT> BuildDescriptorMappings(VulkanDevice* device, const RHIPipelineDescribeInfo& describeInfo, std::vector<uint32_t>& outBindingRemap) {
		std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
		outBindingRemap.resize(describeInfo.Bindings.size());

		uint32_t uniqueLayoutBinding = 0;


		uint32_t CurrentResourceoffset = 0;
		uint32_t CurrentSampleroffset = 0;
		const auto& sizes = device->getHeapSizeInfo();
		uint32_t index = 0;
		for (const auto& binding : describeInfo.Bindings) {
			VkDescriptorType vkType = choiceDescriptorType(binding.descriptorTyp);
			uint32_t descSize = GetDescriptorSize(device, vkType);

			uint32_t originalBinding = binding.binding;

			VkSpirvResourceTypeFlagsEXT resourceMask = 0;

			// Get alignment requirement for this descriptor type
			VkDeviceSize alignment = 0;
			switch (binding.descriptorTyp) {
				case RHIDescriptorTyp::Sampler: {
					alignment = sizes.samplerAlignment;
					resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
					break;
				}
				case RHIDescriptorTyp::SamplerImage: {
					alignment = sizes.imageAlignment;
					resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
					break;
				}
				case RHIDescriptorTyp::Image:
				case RHIDescriptorTyp::RWImage: {
					alignment = sizes.imageAlignment;
					resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT;
					break;
				}
				case RHIDescriptorTyp::UniformBuffer: {
					alignment = sizes.bufferAlignment;
					resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
					break;
				}
				case RHIDescriptorTyp::RBuffer: {
					alignment = sizes.bufferAlignment;
					resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT;
					break;
				}
				case RHIDescriptorTyp::RWBuffer: {
					alignment = sizes.bufferAlignment;
					resourceMask = VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT;
					break;

				}
				default: {
					resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
					alignment = 16;
					break;
				}
			}

			uint32_t& currentOffset = (binding.descriptorTyp == RHIDescriptorTyp::Sampler)
				? CurrentSampleroffset
				: CurrentResourceoffset;

			if (alignment > 0) {
				currentOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
			}

			uint32_t arrayStride = (binding.descriptorTyp == RHIDescriptorTyp::SamplerImage ||
				binding.descriptorTyp == RHIDescriptorTyp::Image ||
				binding.descriptorTyp == RHIDescriptorTyp::RWImage)
				? alignment
				: descSize;


			VkDescriptorSetAndBindingMappingEXT mapping = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
				.descriptorSet = 0,  // 当前统一使用 set 0
				.firstBinding = binding.binding,
				.bindingCount = binding.count,
				// 根据描述符类型自动选择 resourceMask
				.resourceMask = resourceMask,
				.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
				.sourceData = {
					.constantOffset = {
						.heapOffset = currentOffset,
						.heapArrayStride = arrayStride, 
					}
				}
			};

			mappings.push_back(mapping);
			currentOffset += descSize * binding.count;
			index++;
		}
#ifdef  _DEBUG

		Debug("=== Generated Mappings ===");
		for (auto i = 0u; i<mappings.size(); i++) {
			Debug("mapping[{}]: firstBinding={}, bindingCount={}, resourceMask=0x{:x}",
				i, mappings[i].firstBinding, mappings[i].bindingCount, (uint64_t)mappings[i].resourceMask);
		}
		Debug("=== End Mappings ===");

#endif //  _DEBUG
		return mappings;
	}

	
	
	VulkanPipeline::VulkanPipeline(VulkanDevice* inDevice, VulkanDescriptorPool* DescriptorPool, const RHIPipelineState& State) :
		mDevice(inDevice)
	{
		mData = new __VKPipelineData();



		//MappingInfo
		std::vector<VkDescriptorSetAndBindingMappingEXT> mappingInfo;
		mappingInfo = std::move(BuildDescriptorMappings(mDevice, State.describeInfo, mData->bindingRemap));
		
		//DesLayout and PipelineLayout
		mData->Descriptorlayout = DescriptorPool->createDescriptorSetLayout(State.describeInfo, State.pushConstantRange, mData->PipelineLayout, mData->bindingRemap);

		VkShaderDescriptorSetAndBindingMappingInfoEXT shaderMappingInfo = {
			.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
			.mappingCount = (uint32_t)mappingInfo.size(),
			.pMappings = mappingInfo.data(),  // 指向数组数据
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
					.pNext = mDevice->isDescriptorHeapSupported() ? &shaderMappingInfo : nullptr,
					.stage = usingStage,
					.module = (VkShaderModule)State.Shaders[i]->getResourceAPIHandle(),
					.pName = State.Shaders[i]->getEntryPoint()
				};
				shaderInfos.push_back(info);
			}
		}
		else {
			VkPipelineShaderStageCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.pNext = mDevice->isDescriptorHeapSupported() ? &shaderMappingInfo : nullptr,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = (VkShaderModule)State.Shaders[__COMPUTESHADER__]->getResourceAPIHandle(),
				.pName = State.Shaders[__COMPUTESHADER__]->getEntryPoint()
			};
			shaderInfos.push_back(info);
		}

		//Create Pipeline
			VkPipelineCreateFlags2CreateInfo flags2info {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
				.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT
			};
			
		if (!State.isComputePipeline) {

			//Vertext Input
			std::vector<VkVertexInputAttributeDescription> attributes;
			uint32_t stride = 0;
			for (uint32_t i = 0; i < State.vertexInfo.Count; i++) {
				VkVertexInputAttributeDescription attribute{
					.location = i,
					.binding = 0,
					.format = getDataTypeFormat(State.vertexInfo[i]),
					.offset = stride
				};
				stride += getDataTypeSize(State.vertexInfo[i]);
				attributes.push_back(attribute);
			}

			VkVertexInputBindingDescription BindingDescriptioninfo{
				.binding = 0,
				.stride = stride,
				.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
			};

			VkDeviceSize offsetSize[] = { 0 };

			VkPipelineVertexInputStateCreateInfo ISinfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
				.vertexBindingDescriptionCount = 1,
				.pVertexBindingDescriptions = &BindingDescriptioninfo,
				.vertexAttributeDescriptionCount = (uint32_t)attributes.size(),
				.pVertexAttributeDescriptions = attributes.data()
			};

			//Dynamic
			VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
				.dynamicStateCount = 3,
				.pDynamicStates = DynamicState
			};

			//Viewport
			VkPipelineViewportStateCreateInfo viewportStateCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
				.viewportCount = 1,
				.pViewports = nullptr,
				.scissorCount = 1,
				.pScissors = nullptr
			};

			//InputAssembly
			VkPipelineInputAssemblyStateCreateInfo pipelineIAStateCreateInfo{
			  .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			  .topology = getPrimitiveTopology(State.topologyType),
			};

			//Rasterization
			VkPipelineRasterizationStateCreateInfo pipelineRasterizationState{
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
			VkPipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo{
			  .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			  .rasterizationSamples = (VkSampleCountFlagBits)State.multiSampleState.SamplerBit,
			  .sampleShadingEnable = State.multiSampleState.ShadingEnable,
			  .minSampleShading = 1.0f,
			  .pSampleMask = nullptr,
			  .alphaToCoverageEnable = State.multiSampleState.alpthaToCoverageEnable,
			  .alphaToOneEnable = State.multiSampleState.alpthaToOneEnable
			};

			//DepthSetncil
			VkPipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo{
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
			  .srcColorBlendFactor = getBlendFactor(State.colorblendState.SrcColorBlend),
			  .dstColorBlendFactor = getBlendFactor(State.colorblendState.DstColorBlend),
			  .colorBlendOp = getBlendOp(State.colorblendState.ColorBlendOp),
			  .srcAlphaBlendFactor = getBlendFactor(State.colorblendState.SrcAlphaBlend),
			  .dstAlphaBlendFactor = getBlendFactor(State.colorblendState.DstAlphaBlend),
			  .alphaBlendOp = getBlendOp(State.colorblendState.AlphaBlendOp),
			  .colorWriteMask = (VkColorComponentFlags)State.colorblendState.UsingColorBit,
			};

			VkPipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo = {
			  .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			  .logicOpEnable = VK_FALSE,
			  .attachmentCount = 1,
			  .pAttachments = &pipelineColorBlendAttachmentState,
			  .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
			};

			VkGraphicsPipelineCreateInfo info = {
				.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
				.pNext = mDevice->isDescriptorHeapSupported() ? &flags2info : nullptr,
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
				.layout = mDevice->isDescriptorHeapSupported() ? VK_NULL_HANDLE : mData->PipelineLayout,
				.renderPass = (VkRenderPass)State.renderpass->getRenderPassHandle(),
				.basePipelineIndex = -1,
			};

			vkCreateGraphicsPipelines(mDevice->getLogicalDevice(), nullptr, 1, &info, nullptr, &mData->mPipeline);
			

		}
		else {
			mIsComputePipeline = true;
			VkComputePipelineCreateInfo info = {
			  .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			  .pNext = mDevice->isDescriptorHeapSupported() ? &flags2info : nullptr,
			  .stage = shaderInfos[0],
			  .layout = mDevice->isDescriptorHeapSupported() ? VK_NULL_HANDLE : mData->PipelineLayout,
			};
			vkCreateComputePipelines(mDevice->getLogicalDevice(), nullptr, 1, &info, nullptr, &mData->mPipeline);

			if (mDevice->isDescriptorHeapSupported()) info.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
		}
	}

	VulkanPipeline::~VulkanPipeline() {
		vkDestroyPipeline(mDevice->getLogicalDevice(), mData->mPipeline, nullptr);
		delete mData;
	}
	

	Pipeline_t VulkanPipeline::getPipelineHandle() {
		return mData->mPipeline;
	}

	void* VulkanPipeline::getPipelineLayoutHandle() {
		return mData->PipelineLayout;
	}

	bool VulkanPipeline::isComputePipeline() const {
		return mIsComputePipeline;
	}


}
