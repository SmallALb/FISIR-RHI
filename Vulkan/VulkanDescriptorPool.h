#pragma once
#include "VulkanPipeline.h"
#include "../RHIResource.h"


struct VkDescriptorPool_T;
struct VkDescriptorLayout_T;
struct VkDescriptorSetLayout_T;
struct VkCommandBuffer_T;
struct VkPipelineLayout_T;
namespace FISIR {
	class VulkanDevice;
	class RHIResourcePack;

	struct __VKDescriptorPoolData;
	
	class VulkanDescriptorPool {
	public:
		VulkanDescriptorPool(VulkanDevice* device);
		~VulkanDescriptorPool();

		VkDescriptorPool_T* getPool() const { return mPool; }
	
		VkDescriptorSetLayout_T* createDescriptorSetLayout(const RHIPipelineDescribeInfo& info, const RHIPushConstantRange& pcRange, VkPipelineLayout_T*& Pipelinelayout, const std::vector<uint32_t>& bindingMap);


		RHIResourcePackResult createResourcePack(const std::vector<RHIResource*>& resources);
		
		void destroyResourcePack(RHIResourcePack* pack);

		VulkanDevice* mDevice;
		VkDescriptorPool_T* mPool;
		__VKDescriptorPoolData* mData;
	};

	void CmdBindResourcePack(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack, uint32_t bindPoint);

	// 推 push constant。两条路径：
	//   · 描述符堆路径（VK_EXT_descriptor_heap）：用 vkCmdPushDataEXT —— 该扩展下
	//     vkCmdPushConstants 不算数，着色器静态使用 push constant 时会报
	//     "uses push-constant statically ... while there was no call to vkCmdPushDataEXT"；
	//   · 普通路径：vkCmdPushConstants，需要管线的布局（用 pipeline 取）。
	void CmdPushConstant(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIPipeline* pipeline,
	                     uint32_t offset, uint32_t size, const void* data, RHIUsingStageFlags stage);

}
