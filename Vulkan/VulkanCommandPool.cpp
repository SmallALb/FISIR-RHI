#include "VulkanCommandPool.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include "../../Log/Logger.h"


namespace FISIR{
	constexpr size_t INITIAL_PRIMARY_COMMAND_BUFFER_COUNT = 64;
	constexpr size_t INITIAL_SECONDARY_COMMAND_BUFFER_COUNT = 128;


	static uint32_t getQueFamilyIndex(VulkanDevice* device, CommandPoolType poolType) {
		VulkanQueue* queue = nullptr;

		switch (poolType) {
		case FISIR::_Graphics_:
			queue= device->getGraphicQueue(); break;
		case FISIR::_Transfer_:
			queue= device->getTransferQueue(); break;
		case FISIR::_Compute_:
			queue = device->getComputeQueue(); break;
		}
		if (!queue) return 0;
		return queue->getFamilyIndex();
	}

	struct __VKCommandPoolData {
		VkCommandPool mPool;
		std::vector<VkCommandBuffer> PrimaryCommandBufferPool;
		std::vector<VkCommandBuffer> SecondaryCommandBufferPool;
		std::unordered_map<VkCommandBuffer, size_t> PrimaryCommandBufferUsage; 
		std::unordered_map<VkCommandBuffer, size_t> SecondaryCommandBufferUsage;
		std::queue<size_t> FreePrimaryCommandBuffers;
		std::queue<size_t> FreeSecondaryCommandBuffers;
	};

	

	VulkanCommandPool::VulkanCommandPool(VulkanDevice* device, CommandPoolType mType, uint32_t FamilyIndex) {
		mDevice = device;
		mPoolType = mType;
		mData = new __VKCommandPoolData();
		mData->PrimaryCommandBufferPool.resize(INITIAL_PRIMARY_COMMAND_BUFFER_COUNT);
		mData->SecondaryCommandBufferPool.resize(INITIAL_SECONDARY_COMMAND_BUFFER_COUNT);
		
		VkCommandPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | (mType == _Transfer_ ? VK_COMMAND_POOL_CREATE_TRANSIENT_BIT : (VkCommandPoolCreateFlags)0),
			.queueFamilyIndex = FamilyIndex == UINT32_MAX ? getQueFamilyIndex(mDevice, mType) : FamilyIndex,
		};
		vkCreateCommandPool(mDevice->getLogicalDevice(), &poolInfo, nullptr, &mData->mPool);

		VkCommandBufferAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = mData->mPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = static_cast<uint32_t>(mData->PrimaryCommandBufferPool.size())
		};
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->PrimaryCommandBufferPool.data());

		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		allocInfo.commandBufferCount = static_cast<uint32_t>(mData->SecondaryCommandBufferPool.size());
		vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, mData->SecondaryCommandBufferPool.data());
	
		for (size_t i = 0; i < mData->PrimaryCommandBufferPool.size(); ++i) {
			mData->PrimaryCommandBufferUsage[mData->PrimaryCommandBufferPool[i]] = i;
			mData->FreePrimaryCommandBuffers.push(i);
		}

		for (size_t i = 0; i < mData->SecondaryCommandBufferPool.size(); ++i) {
			mData->SecondaryCommandBufferUsage[mData->SecondaryCommandBufferPool[i]] = i;
			mData->FreeSecondaryCommandBuffers.push(i);
		}
	}
	
	VulkanCommandPool::~VulkanCommandPool() {
		if (mData) {
			vkFreeCommandBuffers(mDevice->getLogicalDevice(), mData->mPool, static_cast<uint32_t>(mData->PrimaryCommandBufferPool.size()), mData->PrimaryCommandBufferPool.data());
			vkFreeCommandBuffers(mDevice->getLogicalDevice(), mData->mPool, static_cast<uint32_t>(mData->SecondaryCommandBufferPool.size()), mData->SecondaryCommandBufferPool.data());
			vkDestroyCommandPool(mDevice->getLogicalDevice(), mData->mPool, nullptr);
			delete mData;
			mData = nullptr;
		}
	}

	CBInfo VulkanCommandPool::createCommandBuffer(CommandBufferType cbType) {
		if ((cbType == _Primary_) ? mData->FreePrimaryCommandBuffers.empty() : mData->FreeSecondaryCommandBuffers.empty()) {
			VkCommandBufferAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = mData->mPool,
				.level = (cbType == _Primary_) ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY,
				.commandBufferCount = 1
			};
			VkCommandBuffer newCB;
			if (vkAllocateCommandBuffers(mDevice->getLogicalDevice(), &allocInfo, &newCB) != VK_SUCCESS) {
				Error("Failed to allocate command buffer!");
				return {};
			}
			if (cbType == _Primary_) {
				mData->PrimaryCommandBufferPool.push_back(newCB);
				mData->FreePrimaryCommandBuffers.push(mData->PrimaryCommandBufferPool.size() - 1);
			}
			else {
				mData->SecondaryCommandBufferPool.push_back(newCB);
				mData->FreeSecondaryCommandBuffers.push(mData->SecondaryCommandBufferPool.size() - 1);
			}
			(cbType == _Primary_ ? mData->PrimaryCommandBufferUsage[newCB] : mData->SecondaryCommandBufferUsage[newCB]) 
				= (cbType == _Primary_ ? mData->FreePrimaryCommandBuffers.size()-1 : mData->FreeSecondaryCommandBuffers.size()-1);
		}
		uint32_t index = (cbType == _Primary_) ? mData->FreePrimaryCommandBuffers.front() : mData->FreeSecondaryCommandBuffers.front();
		(cbType == _Primary_) ? mData->FreePrimaryCommandBuffers.pop() : mData->FreeSecondaryCommandBuffers.pop();
		
		CBInfo cbInfo((cbType == _Primary_) ? mData->PrimaryCommandBufferPool[index] : mData->SecondaryCommandBufferPool[index], cbType, mPoolType, this);

		if (cbInfo.buffer == VK_NULL_HANDLE) {
			Error("Failed to allocate command buffer!");
			return {};
		}
		return cbInfo;
	}

	void VulkanCommandPool::releaseCommandBuffer(const CBInfo& cbInfo) {
		if (cbInfo.pool != this) {
			Error("Attempting to release a command buffer that does not belong to this pool!");
			return;
		}
		vkResetCommandBuffer(cbInfo.buffer, 0);
		(cbInfo.type == _Primary_) ? mData->FreePrimaryCommandBuffers.push(mData->PrimaryCommandBufferUsage[cbInfo.buffer]) : mData->FreeSecondaryCommandBuffers.push(mData->SecondaryCommandBufferUsage[cbInfo.buffer]);

	}


	std::vector<VulkanCommandPool*>& VulkanCommandPoolManager::getPool(CommandPoolType type) {
		switch (type) {
		case _Graphics_:
			return RenderCommandPools;
		case _Compute_:
			return ComputeCommandPools;
		case _Transfer_:
			return TransferCommandPools;
		}
	}

	LockFreeQue<VulkanCommandPool*>& VulkanCommandPoolManager::getQue(CommandPoolType type) {
		switch (type) {
		case _Graphics_:
			return FreeRenderCommandPools;
		case _Compute_:
			return FreeComputeCommandPools;
		case _Transfer_:
			return FreeTransferCommandPools;
		}
	}

	std::mutex& VulkanCommandPoolManager::getMutex(CommandPoolType type) {
		switch (type) {
		case _Graphics_:
			return RenderCommandPoolsMTX;
		case _Compute_:
			return ComputeCommandPoolsMTX;
		case _Transfer_:
			return TransferCommandPoolsMTX;
		}
	}

	VulkanCommandPoolManager::VulkanCommandPoolManager(VulkanDevice* device) : mDevice(device) {
		RenderCommandPools.resize(10, nullptr);
		ComputeCommandPools.resize(10, nullptr);
		TransferCommandPools.resize(10, nullptr);

		if (!ThreadCommanPoolListener::commandPoolRunning.load(std::memory_order_acquire)) ThreadCommanPoolListener::commandPoolRunning.store(1, std::memory_order_release);

		for (auto& pool : RenderCommandPools) {
			pool = new VulkanCommandPool(mDevice, CommandPoolType::_Graphics_);
			FreeRenderCommandPools.push(pool);
		}

		for (auto& pool : ComputeCommandPools) {
			pool = new VulkanCommandPool(mDevice, CommandPoolType::_Compute_);
			FreeComputeCommandPools.push(pool);
		}

		for (auto& pool : TransferCommandPools) {
			pool = new VulkanCommandPool(mDevice, CommandPoolType::_Transfer_);
			FreeTransferCommandPools.push(pool);
		}
	}

	VulkanCommandPoolManager::~VulkanCommandPoolManager() {
		ThreadCommanPoolListener::commandPoolRunning.store(0, std::memory_order_release);
		
		for (auto& pool : RenderCommandPools) {
			delete pool;
		}

		for (auto& pool : ComputeCommandPools) {
			delete pool;
		}

		for (auto& pool : TransferCommandPools) {
			delete pool;
		}

		std::lock_guard<std::mutex> lock(MapMutex);
		for (auto& [familyindex, pool] : FamilyIndexToPool) delete pool;	

	}

	VulkanCommandPool* VulkanCommandPoolManager::getCommandPool(CommandPoolType type) {
		auto& Pool = getPool(type);
		auto& Que = getQue(type);
		auto& Mtx = getMutex(type);

		VulkanCommandPool* res = nullptr;
		uint32_t familyindex = getQueFamilyIndex(mDevice, type);

		if (Que.pop(res)) {

			return res;
		}

		std::lock_guard<std::mutex> lock(Mtx);

		if (Que.pop(res)) {
			return res;
		}

		res = new VulkanCommandPool(mDevice, type);
		Pool.push_back(res);

		return res;
	}

	VulkanCommandPool* VulkanCommandPoolManager::getCommandPool(uint32_t FamilyIndex) {
		std::lock_guard<std::mutex> lock(MapMutex);
		if (FamilyIndexToPool.contains(FamilyIndex)) return FamilyIndexToPool[FamilyIndex];
		FamilyIndexToPool[FamilyIndex] = new VulkanCommandPool(mDevice, CommandPoolType::_Presnet_, FamilyIndex);
		return FamilyIndexToPool[FamilyIndex];
	}

	void VulkanCommandPoolManager::reBackCommandPool(VulkanCommandPool* pool) {
		if (!pool) return;
		auto& Que = getQue(pool->mPoolType);
		Que.push(pool);
	}


	std::atomic_bool ThreadCommanPoolListener::commandPoolRunning {0};


}
