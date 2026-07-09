#pragma once
#include "../RHIResource.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <atomic>
#include "../LockFreeQue.h"
#include <mutex>
#include "../RHICommandList.h"
struct VkCommandBuffer_T;

namespace FISIR{
	class VulkanDevice;
	class VulkanCommandPool;
	class VulkanSemaphore;
	class VulkanViewport;
	class VulkanFence;
	class VulkanSwapChain;
	class VulkanRenderPass;
	struct __VKCommandPoolData;

	
	enum CommandBufferType {
		_Primary_,
		_Secondary_,
		COMMAND_BUFFER_TYPE_COUNT
	};
	

	struct ResourceWillBeLayout_Access {
		ResourceAccess access;
		TextureLayout layout;
	};


	struct CBInfo {
		VkCommandBuffer_T* buffer{ nullptr };
		CommandBufferType type;
		CmdType poolType;
		VulkanCommandPool* pool{ nullptr };
		std::unordered_map<RHIResource*, ResourceWillBeLayout_Access> QuoteResources;
	
		CBInfo() {}

		CBInfo(
			VkCommandBuffer_T* buffer_,
			CommandBufferType type_,
			CmdType poolType_,
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
		VulkanCommandPool(VulkanDevice* device, CmdType mType, uint32_t FamilyIndex = UINT32_MAX);
		~VulkanCommandPool();

		CBInfo createCommandBuffer(CommandBufferType cbType);

		void releaseCommandBuffer(const CBInfo& cbInfo);

		bool isPoolUsed() const { return UsedInThread.load(std::memory_order_acquire); }

		__VKCommandPoolData* mData;
		VulkanDevice* mDevice;
		CmdType mPoolType;
		std::atomic_bool UsedInThread{0};

	};
	

    class VulkanCommandPoolManager {
        std::vector<VulkanCommandPool*>& getPool(CmdType type);

        LockFreeQue<VulkanCommandPool*>& getQue(CmdType type);

        std::mutex& getMutex(CmdType type);

    public:
        VulkanCommandPoolManager(VulkanDevice* device);

        ~VulkanCommandPoolManager();

        VulkanCommandPool* getCommandPool(CmdType type);

		VulkanCommandPool* getCommandPool(uint32_t FamilyIndex);
        
		void reBackCommandPool(VulkanCommandPool* pool);

    private:
        std::vector<VulkanCommandPool*> RenderCommandPools, ComputeCommandPools, TransferCommandPools;
        std::unordered_map<uint32_t, VulkanCommandPool*> FamilyIndexToPool;
		std::mutex RenderCommandPoolsMTX, ComputeCommandPoolsMTX, TransferCommandPoolsMTX, MapMutex;
        LockFreeQue<VulkanCommandPool*> FreeRenderCommandPools, FreeComputeCommandPools, FreeTransferCommandPools;
        
		VulkanDevice* mDevice;
    };

	class CommandExecuteThreadPool {
		struct ThreadData {
			VulkanRenderPass* renderPass{ nullptr };
			CBInfo ExecutedCB {};
		};
	
		struct ExecutedPageTask {
			RingCommandPool::Page::BatchInfo Batch;
			std::atomic_int* ThreadID;
		};

	public:
		CommandExecuteThreadPool(VulkanDevice* device, VulkanCommandPoolManager* VkCmdPoolManager);

		~CommandExecuteThreadPool();

		void pushCommandBatch(RingCommandPool::Page* page, std::atomic_int* threadId);

		CBInfo getExecutedCB(uint32_t ThreadId, VulkanRenderPass** renderPass = nullptr);
		
	private:
		void ThreadLoop(int ThreadID);

		LockFreeQue<ExecutedPageTask> NeedExecutePages;
		

		std::vector<std::thread> Threads;
		std::vector<std::atomic_bool> ThreadFlags;
		std::vector<ThreadData> ThreadExecutedCBs;
		std::atomic_bool StopTag {0};
		
		VulkanCommandPoolManager* usingManager;
		VulkanDevice* mDevice;
 	};


}


