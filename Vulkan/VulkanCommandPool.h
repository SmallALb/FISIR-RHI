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
	class VulkanFrameBuffer;
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
		size_t index {SIZE_MAX};
	
		CBInfo() {}

		CBInfo(
			VkCommandBuffer_T* buffer_,
			CommandBufferType type_,
			CmdType poolType_,
			VulkanCommandPool* pool_,
			uint32_t idx
		): buffer(buffer_), type(type_), poolType(poolType_), pool(pool_), index(idx) {}

		CBInfo(const CBInfo& other) :
			buffer(other.buffer),
			type(other.type),
			poolType(other.poolType),
			pool(other.pool),
			QuoteResources(other.QuoteResources),
			index(other.index)
		{}


		CBInfo(CBInfo&& other) noexcept :
			buffer(other.buffer), 
			pool(other.pool), 
			QuoteResources(std::move(other.QuoteResources)),
			type(other.type),
			poolType(other.poolType),
			index(other.index)
		{
			other.buffer = nullptr;
			other.pool = nullptr;
		}

		CBInfo& operator=(CBInfo&& other) noexcept {
			buffer = other.buffer;
			type = other.type;
			poolType = other.poolType;
			pool = other.pool;
			QuoteResources = std::move(other.QuoteResources);
			index = other.index;  
			other.buffer = nullptr;
			other.pool = nullptr;
			return *this;
		}

		CBInfo& operator=(const CBInfo& other) {
			buffer = other.buffer;
			type = other.type;
			poolType = other.poolType;
			pool = other.pool;
			QuoteResources = other.QuoteResources;
			index = other.index;  
			return *this;
		}
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



	struct ExecuteResultData {
		VulkanFrameBuffer* frameBuffer{ nullptr };
		ClearValue clearValue{};
		bool renderPassEndTag{ false };
		bool commandsEndTag {false};
		uint32_t subpassIndex{ 0 };
		CBInfo ExecutedCB {};
		std::atomic<VulkanFence*> fence { nullptr };
		std::vector<RHISemaphore*> waits;
		std::vector<RHISemaphore*> signals;

		ExecuteResultData& operator=(ExecuteResultData&& other) noexcept {
			frameBuffer = other.frameBuffer;
			clearValue = other.clearValue;
			renderPassEndTag = other.renderPassEndTag;
			subpassIndex = other.subpassIndex;
			ExecutedCB = std::move(other.ExecutedCB);
			waits = std::move(other.waits);
			signals = std::move(other.signals);
			fence.store(other.fence, std::memory_order_release);
			return *this;
		}
	};
	
	class CommandExecuteThreadPool {	
		struct ExecutedPageTask {
			RingCommandPool::Page::BatchInfo Batch;
			ExecuteResultData* result;
			std::atomic_uint32_t* finishCount;
		};
	public:
		CommandExecuteThreadPool(VulkanDevice* device, VulkanCommandPoolManager* VkCmdPoolManager);

		~CommandExecuteThreadPool();

		void pushCommandBatch(RingCommandPool::Page::BatchInfo batch, ExecuteResultData* result, std::atomic_uint32_t* finishCount);
	private:
		void ThreadLoop(uint32_t ThreadID);

		LockFreeQue<ExecutedPageTask> NeedExecutePages;
		

		std::vector<std::thread> Threads;
		std::atomic_bool StopTag {0};
		
		VulkanCommandPoolManager* usingManager;
		VulkanDevice* mDevice;
 	};


}


