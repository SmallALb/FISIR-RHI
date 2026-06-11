#pragma once
#include "../RHIResource.h";
#include <vector>
#include <unordered_map>
#include <atomic>
struct VkCommandBuffer_T;

namespace FISIR{
	class VulkanDevice;
	class VulkanCommandPool;

	struct __VKCommandPoolData;

	
	enum CommandBufferType {
		_Primary_,
		_Secondary_,
		COMMAND_BUFFER_TYPE_COUNT
	};
	
	enum CommandPoolType {
		_Graphics_,
		_Compute_,
		_Transfer_,
		COMMAND_POOL_TYPE_COUNT
	};
	

	struct ResourceWillBeLayout_Access {
		ResourceAccess access;
		TextureLayout layout;
	};


	struct CBInfo {
		VkCommandBuffer_T* buffer;
		CommandBufferType type;
		CommandPoolType poolType;
		VulkanCommandPool* pool;
		std::unordered_map<RHIResource*, ResourceWillBeLayout_Access> QuoteResources;
	};


	class VulkanCommandPool {
	public:
		VulkanCommandPool(VulkanDevice* device, CommandPoolType mType);
		~VulkanCommandPool();

		CBInfo createCommandBuffer(CommandBufferType cbType);

		void releaseCommandBuffer(const CBInfo& cbInfo);

		bool isPoolUsed() const { return UsedInThread.load(std::memory_order_acquire); }

		__VKCommandPoolData* mData;
		VulkanDevice* mDevice;
		CommandPoolType mPoolType;
		std::atomic_bool UsedInThread{0};

	};
	



}


