#pragma once
#include "VulkanPipeline.h"
#include "../RHIResource.h"


struct VkDescriptorPool_T;
struct VkDescriptorLayout_T;
struct VkDescriptorSetLayout_T;
struct VkCommandBuffer_T;
namespace FISIR {
	class VulkanDevice;
	class RHIResourcePack;

	//如果能够使用Descrtptor_heap
	//那么这个Pool的管理对象就是Heap
	//如果不能使用Descriptor_heap
	//那么回到DescriptorPool
	struct __VKDescriptorPoolData;
	
	class VulkanDescriptorPool {
	public:
		VulkanDescriptorPool(VulkanDevice* device);
		~VulkanDescriptorPool();

		VkDescriptorPool_T* getPool() const { return mPool; }
	
		VkDescriptorSetLayout_T* createDescriptorSetLayout(const RHIPipelineDescribeInfo& info);


		RHIResourcePackResult createResourcePack(const std::vector<RHIResource*>& resources);
		
		void destroyResourcePack(RHIResourcePack* pack);

		VulkanDevice* mDevice;
		VkDescriptorPool_T* mPool;
		__VKDescriptorPoolData* mData;
	};

	void CmdBindResourcePack(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack);

}
