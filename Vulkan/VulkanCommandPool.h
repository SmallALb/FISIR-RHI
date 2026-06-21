#pragma once
#include "../RHIResource.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <atomic>
#include "../LockFreeQue.h"
#include <mutex>
struct VkCommandBuffer_T;

namespace FISIR{
	class VulkanDevice;
	class VulkanCommandPool;
	class VulkanSemaphore;
	class VulkanViewport;
	class VulkanFence;
	class VulkanSwapChain;
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
		_Presnet_,
		COMMAND_POOL_TYPE_COUNT
	};
	

	struct ResourceWillBeLayout_Access {
		ResourceAccess access;
		TextureLayout layout;
	};


	struct CBInfo {
		VkCommandBuffer_T* buffer{ nullptr };
		CommandBufferType type;
		CommandPoolType poolType;
		VulkanCommandPool* pool{ nullptr };
		std::unordered_map<RHIResource*, ResourceWillBeLayout_Access> QuoteResources;
	
		CBInfo() {}

		CBInfo(
			VkCommandBuffer_T* buffer_,
			CommandBufferType type_,
			CommandPoolType poolType_,
			VulkanCommandPool* pool_
		): buffer(buffer_), type(type_), poolType(poolType_), pool(pool_) {}

		CBInfo(const CBInfo& other) :
			buffer(other.buffer),
			type(other.type),
			poolType(other.poolType),
			pool(other.pool),
			QuoteResources(other.QuoteResources)  
		{}


		CBInfo(CBInfo&& other) noexcept :
			buffer(other.buffer), 
			pool(other.pool), 
			QuoteResources(std::move(other.QuoteResources)),
			type(other.type),
			poolType(other.poolType)
		{
			other.buffer = nullptr;
			other.pool = nullptr;
		}

		CBInfo& operator=(CBInfo&& other) noexcept = default;
		CBInfo& operator=(const CBInfo& other) = default;
	};


	class VulkanCommandPool {
	public:
		VulkanCommandPool(VulkanDevice* device, CommandPoolType mType, uint32_t FamilyIndex = UINT32_MAX);
		~VulkanCommandPool();

		CBInfo createCommandBuffer(CommandBufferType cbType);

		void releaseCommandBuffer(const CBInfo& cbInfo);

		bool isPoolUsed() const { return UsedInThread.load(std::memory_order_acquire); }

		__VKCommandPoolData* mData;
		VulkanDevice* mDevice;
		CommandPoolType mPoolType;
		std::atomic_bool UsedInThread{0};

	};
	

    class VulkanCommandPoolManager {
        std::vector<VulkanCommandPool*>& getPool(CommandPoolType type);

        LockFreeQue<VulkanCommandPool*>& getQue(CommandPoolType type);

        std::mutex& getMutex(CommandPoolType type);

    public:
        VulkanCommandPoolManager(VulkanDevice* device);

        ~VulkanCommandPoolManager();

        VulkanCommandPool* getCommandPool(CommandPoolType type);

		VulkanCommandPool* getCommandPool(uint32_t FamilyIndex);
        
		void reBackCommandPool(VulkanCommandPool* pool);

    private:
        std::vector<VulkanCommandPool*> RenderCommandPools, ComputeCommandPools, TransferCommandPools;
        std::unordered_map<uint32_t, VulkanCommandPool*> FamilyIndexToPool;
		std::mutex RenderCommandPoolsMTX, ComputeCommandPoolsMTX, TransferCommandPoolsMTX, MapMutex;
        LockFreeQue<VulkanCommandPool*> FreeRenderCommandPools, FreeComputeCommandPools, FreeTransferCommandPools;
        
		VulkanDevice* mDevice;
    };

    struct ThreadCommanPoolListener {
        ThreadCommanPoolListener(VulkanCommandPoolManager* manager, CommandPoolType type, uint32_t familyIndex = UINT32_MAX) {
            mManager = manager;
			if (familyIndex != UINT32_MAX) {
				mCommandPool = mManager->getCommandPool(familyIndex);
			}
			else {
				mCommandPool = mManager->getCommandPool(type);
			}
            if (mCommandPool) mCommandPool->UsedInThread.store(true, std::memory_order_release);
        }

        ~ThreadCommanPoolListener() {
            mCommandPool->UsedInThread.store(false, std::memory_order_release);
            mManager->reBackCommandPool(mCommandPool);
        }

        VulkanCommandPool* mCommandPool{ nullptr };
        VulkanCommandPoolManager* mManager{ nullptr };
    };

}


