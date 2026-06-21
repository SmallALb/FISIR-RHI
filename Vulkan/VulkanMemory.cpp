#include "VulkanMemory.h"
#include "VulkanDevice.h"
#include <vulkan/vulkan.h>
#include "../../Log/Logger.h"
#include <vector>
#include <queue>
#include <set>
#include "../SparseMap.h"
#include "VulkanTexture.h"
#include "VulkanDeviceAllocationPool.h"
#include <unordered_map>
namespace FISIR{

	
	constexpr size_t PageSize = 128 * 1024 * 1024;


	struct __VkMemoryData {
		VkPhysicalDeviceMemoryProperties mDeviceMemoryProperties;
		sparse_map<uint32_t, std::vector<AllocationPool>> DevicePoolMap;
		std::unordered_map<AllocationPool*, void*> HostVisablePoolMap;
	};


	VulkanMemoryAllocator::VulkanMemoryAllocator() {
		mData = new __VkMemoryData();
	}

	VulkanMemoryAllocator::~VulkanMemoryAllocator() {
		for (auto& [hostPool, cpuPtr] : mData->HostVisablePoolMap) {
			vkUnmapMemory(mDevice->getLogicalDevice(), hostPool->Pool);
		}
		delete mData;
	}

	void VulkanMemoryAllocator::init(VulkanDevice* device) {
		mDevice = device;
		vkGetPhysicalDeviceMemoryProperties(mDevice->getPhysicalDevice(), &mData->mDeviceMemoryProperties);
	}

	GpuBlock* VulkanMemoryAllocator::create(size_t Size, size_t align, MemType visable, RHIResource* resource, void** CpuSetPtr) {
		Debug("Try Create Gpu Block");
		auto T = make_sure_type_exits(visable, resource);
		GpuBlock* block = mData->DevicePoolMap[T.first][T.second].NewBlock(Size, align);
		if (!block) {
			auto& pools = mData->DevicePoolMap[T.first];
			uint32_t poolID = T.second;
			for (; poolID <pools.size(); poolID++) if (pools[poolID].totalSize > 0) {
				block = pools[poolID].NewBlock(Size, align);
				if (block) return block;
			}
			if (poolID == pools.size()) {
				pools.emplace_back(poolID, PageSize, T.first, mDevice->getLogicalDevice(), mDevice->isDescriptorHeapSupported());
			}
			block = pools.back().NewBlock(Size, align);
		}
		if (resource) bindMemoryFor(block, align, resource, CpuSetPtr);
		return block;
	}


	void VulkanMemoryAllocator::free(GpuBlock* Block) {
		mData->DevicePoolMap[Block->Info.MemoryType][Block->PoolID].FreeBlock(Block);
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
				block->Info.GpuMemory, block->Info.offset )
			: vkBindBufferMemory(
				mDevice->getLogicalDevice(),
				static_cast<VkBuffer>(resource->getResourceAPIHandle()),
				block->Info.GpuMemory, block->Info.offset)
			;

		Debug("Map Memory: {}", resource->getResourceType() == Type::Texture ? "Texture" : "Buffer");
		if ((resource->getResourceMemType() & MemTypHostVisable) && CpuSetPtr != nullptr) {
			*CpuSetPtr = (void*)((char*)mData->HostVisablePoolMap[&mData->DevicePoolMap[block->Info.MemoryType][block->PoolID]] + block->Info.offset);
			Debug("Mapped Memory: {}", block->Info.Size);
		}
	}


	std::pair<uint32_t, uint32_t> VulkanMemoryAllocator::make_sure_type_exits(MemType require, RHIResource* resource) {
		uint32_t MemTyp = UINT32_MAX;
		uint32_t poolID = 0;
		VkMemoryRequirements req;
		resource->getResourceType() == Type::Texture
			? vkGetImageMemoryRequirements(mDevice->getLogicalDevice(), static_cast<VkImage>(resource->getResourceAPIHandle()), &req)
			: vkGetBufferMemoryRequirements(mDevice->getLogicalDevice(), static_cast<VkBuffer>(resource->getResourceAPIHandle()), &req);
		MemTyp = get_suitable_type(req.memoryTypeBits, require);

		auto& pools = mData->DevicePoolMap[MemTyp];

		for (;poolID < pools.size(); poolID++) if (pools[poolID].totalSize > 0) break;
		
		if (poolID == pools.size()) {
			pools.emplace_back(poolID, PageSize, MemTyp, mDevice->getLogicalDevice(), mDevice->isDescriptorHeapSupported());
		}
		if ((require & MemTypHostVisable) && !mData->HostVisablePoolMap.contains(&pools[poolID])) {
			mData->HostVisablePoolMap[&pools[poolID]] = nullptr;
			vkMapMemory(mDevice->getLogicalDevice(), pools[poolID].Pool, 0, PageSize, 0, &mData->HostVisablePoolMap[&pools[poolID]]);
			Debug("Create New Pool for Memory Type: {}, Pool ID: {}", MemTyp, poolID);
			Debug("Mapped Memory: {}", PageSize);
		}
		return { MemTyp, poolID };
	}

	uint32_t VulkanMemoryAllocator::get_suitable_type(uint32_t MemBits, MemType require) {
		for (uint32_t i = 0; i < mData->mDeviceMemoryProperties.memoryTypeCount; i++) if (MemBits & (1 << i) && ((mData->mDeviceMemoryProperties.memoryTypes[i].propertyFlags & require) == require))
			return i;
		return UINT32_MAX;
	}


	BlockInfo GetGpuBlockInfo(GpuBlock* block) {
		return block->Info;
	}

}
