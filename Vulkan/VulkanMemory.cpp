#include "VulkanMemory.h"

#include <algorithm>
#include <queue>
#include <set>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "../SparseMap.h"
#include "VulkanDevice.h"
#include "VulkanDeviceAllocationPool.h"
#include "VulkanTexture.h"
namespace FISIR{

	
	constexpr size_t PageSize = 16 * 1024 * 1024;


	struct __VkMemoryData {
		VkPhysicalDeviceMemoryProperties mDeviceMemoryProperties;
		std::vector<std::vector<AllocationPool*>> DevicePoolMap;
		std::unordered_map<AllocationPool*, void*> HostVisablePoolMap;
	};


	VulkanMemoryAllocator::VulkanMemoryAllocator() {
		mData = new __VkMemoryData();
	}

	VulkanMemoryAllocator::~VulkanMemoryAllocator() {
		for (auto& [hostPool, cpuPtr] : mData->HostVisablePoolMap) {
			vkUnmapMemory(mDevice->getLogicalDevice(), hostPool->Pool);
		}

		for (auto& pools : mData->DevicePoolMap) {
			for (auto& pool : pools) {
				if (pool) {
					delete pool;
				}
			}
			pools.clear();
		}
		mData->DevicePoolMap.clear();
		delete mData;
	}

	void VulkanMemoryAllocator::init(VulkanDevice* device) {
		mDevice = device;
		vkGetPhysicalDeviceMemoryProperties(mDevice->getPhysicalDevice(), &mData->mDeviceMemoryProperties);
		mData->DevicePoolMap.resize(mData->mDeviceMemoryProperties.memoryTypeCount);
	}

	GpuBlock* VulkanMemoryAllocator::create(size_t Size, size_t align, MemType visable, RHIResource* resource, void** CpuSetPtr) {
		return make_sure_type_exits_and_new(Size, align, visable, resource, CpuSetPtr);
	}


	void VulkanMemoryAllocator::free(GpuBlock* Block) {
		mData->DevicePoolMap[Block->Info.MemoryType][Block->PoolID]->FreeBlock(Block);
	}


	GpuBlock* VulkanMemoryAllocator::make_sure_type_exits_and_new(size_t Size, size_t align, MemType require, RHIResource* resource, void** CpuSetPtr) {
		VkMemoryRequirements req;
		resource->getResourceType() == Type::Texture
			? vkGetImageMemoryRequirements(mDevice->getLogicalDevice(), static_cast<VkImage>(resource->getResourceAPIHandle()), &req)
			: vkGetBufferMemoryRequirements(mDevice->getLogicalDevice(), static_cast<VkBuffer>(resource->getResourceAPIHandle()), &req);
		
		uint32_t MemTyp = get_suitable_type(req.memoryTypeBits, require);
		if (MemTyp == UINT32_MAX) {
			Error("No suitable memory type found!");
			return nullptr;
		}
		auto& pools = mData->DevicePoolMap[MemTyp];
		GpuBlock* block = nullptr;
		size_t poolID = 0;


		for (; poolID < pools.size(); poolID++) {
			if (pools[poolID]->totalSize > 0) {
				block = pools[poolID]->NewBlock(Size, align);
				if (block) {
					// 分配成功，记录池ID
					block->PoolID = poolID;
					break;
				}
			}
		}

		if (!block) {
			poolID = pools.size();
			size_t poolSize = std::max(PageSize, align_up(Size, align));
			pools.emplace_back(new AllocationPool(poolID, poolSize, MemTyp,
				mDevice->getLogicalDevice(),
				mDevice->isDescriptorHeapSupported()));
			if (pools[poolID]->Pool == VK_NULL_HANDLE) {
				Error("vkAllocateMemory failed for new pool: MemType={}, Size={}", MemTyp, poolSize);
				return nullptr;
			}

			if ((require & MemTypHostVisable) && !mData->HostVisablePoolMap.contains(pools[poolID])) {
				void* mappedPtr = nullptr;
				VkResult result = vkMapMemory(mDevice->getLogicalDevice(), pools[poolID]->Pool,
					0, PageSize, 0, &mappedPtr);
				if (result != VK_SUCCESS) {
					Error("Failed to map new pool: {}", (size_t)result);
					return nullptr;
				}
				mData->HostVisablePoolMap[pools[poolID]] = mappedPtr;
				Debug("Created and mapped new pool: MemType={}, PoolID={}, Ptr=0x{:x}",
					MemTyp, poolID, (size_t)mappedPtr);
			}

			// 在新池中分配
			block = pools[poolID]->NewBlock(Size, align);
			if (!block) {
				Error("Failed to allocate from new pool!");
				return nullptr;
			}
			block->PoolID = poolID;
		}

		if (resource && block) {
			bindMemoryFor(block, align, resource, CpuSetPtr);
		}


		return block;
	}

	uint32_t VulkanMemoryAllocator::get_suitable_type(uint32_t MemBits, MemType require) {
		for (uint32_t i = 0; i < mData->mDeviceMemoryProperties.memoryTypeCount; i++) if (MemBits & (1 << i) && ((mData->mDeviceMemoryProperties.memoryTypes[i].propertyFlags & require) == require))
			return i;
		return UINT32_MAX;
	}


	void VulkanMemoryAllocator::bindMemoryFor(GpuBlock* block, size_t align, RHIResource* resource, void** CpuSetPtr) {
		block->bindingResource = resource;

		if (block->Info.offset % align != 0) {
			Error("Memory offset {} is not aligned to required alignment {}!", block->Info.offset, align);
			return;
		}

		resource->getResourceType() == Type::Texture
			? vkBindImageMemory(
				mDevice->getLogicalDevice(),
				static_cast<VkImage>(resource->getResourceAPIHandle()),
				block->Info.GpuMemory, block->Info.offset)
			: vkBindBufferMemory(
				mDevice->getLogicalDevice(),
				static_cast<VkBuffer>(resource->getResourceAPIHandle()),
				block->Info.GpuMemory, block->Info.offset)
			;

		if ((resource->getResourceMemType() & MemTypHostVisable) && CpuSetPtr != nullptr) {
			*CpuSetPtr = (void*)((char*)mData->HostVisablePoolMap[mData->DevicePoolMap[block->Info.MemoryType][block->PoolID]] + block->Info.offset);
		}
	}


	BlockInfo GetGpuBlockInfo(GpuBlock* block) {
		return block->Info;
	}

}
