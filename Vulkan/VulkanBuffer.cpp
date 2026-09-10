#include "VulkanBuffer.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDebugNameSet.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"

namespace FISIR {	


	static VkDescriptorType getDescriptorType(BufferLayoutFlags type) {
		if (type & UniformBuffer) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		if (type & (RBuffer | RWBuffer)) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		
		return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}
	
	struct __VKBufferData {
		VkBuffer buffer;
		GpuBlock* mBlock;
		uint64_t size;
		uint64_t stride;
		BufferLayoutFlags bufferLayout;
		MemType memoryType;
		VkDeviceAddress Gpuaddress;
	};

	static VkBufferUsageFlags getBufferUsage(BufferLayoutFlags layout) {
		VkBufferUsageFlags flags = 0;

		if (layout & VertexBuffer) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (layout & IndexBuffer) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (layout & UniformBuffer) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if (layout & RBuffer) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (layout & RWBuffer) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (layout & TransferDstBuffer) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (layout & TransferSrcBuffer) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (layout & IndirectBuffer)  flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		return flags;
	}
	
	VulkanBuffer::VulkanBuffer(VulkanDevice* device, const BufferInfo& info, uint32_t Usage, const char* DebugName) : mDevice(device) {
		mData = new __VKBufferData();
		auto allocator = mDevice->getAllocator();
		mData->size = info.size;
		mData->stride = info.stride;
		mData->bufferLayout = info.bufferlayout;
		mData->memoryType = info.memoryType;

		// 跨队列族共享缓冲：列出全部（去重后的）队列族索引，允许 compute/graphics/transfer
		// 直接读写而无需显式所有权转移（release/acquire）。仅当用户显式开启时才使用 CONCURRENT，
		// 默认仍为 EXCLUSIVE 以保留既有行为。
		uint32_t concurrentFamilies[3] = { 0, 0, 0 };
		uint32_t concurrentFamilyCount = 0;
		if (info.concurrentSharing) {
			const uint32_t families[3] = {
				mDevice->getGraphicQueue()->getFamilyIndex(),
				mDevice->getComputeQueue()->getFamilyIndex(),
				mDevice->getTransferQueue()->getFamilyIndex()
			};
			for (uint32_t i = 0; i < 3; ++i) {
				bool dup = false;
				for (uint32_t j = 0; j < concurrentFamilyCount; ++j) {
					if (concurrentFamilies[j] == families[i]) { dup = true; break; }
				}
				if (!dup) concurrentFamilies[concurrentFamilyCount++] = families[i];
			}
		}

		VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = info.size,
			.usage = (Usage ? (VkBufferUsageFlags)Usage : getBufferUsage(info.bufferlayout)) |
					(mDevice->isDescriptorHeapSupported() ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) : 0),
			.sharingMode = info.concurrentSharing ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = info.concurrentSharing ? concurrentFamilyCount : 0,
			.pQueueFamilyIndices = info.concurrentSharing ? concurrentFamilies : nullptr
		};
		vkCreateBuffer(mDevice->getLogicalDevice(), &bufferInfo, nullptr, &mData->buffer);

		VkMemoryRequirements memReqs;
		vkGetBufferMemoryRequirements(mDevice->getLogicalDevice(), mData->buffer, &memReqs);


		Data_GPU = nullptr;
		size_t allocAlignment = (size_t)memReqs.alignment;
		// 描述符堆缓冲：驱动的 memReqs.alignment 可能小于 resourceHeapAlignment / samplerHeapAlignment（本驱动返回 16），
		// 需强制按 heap 对齐分配，否则 vkCmdBindResourceHeapEXT 的 heapRange.address 校验失败。
		if ((Usage & VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT) && mDevice->isDescriptorHeapSupported()) {
			const auto& heap = mDevice->getHeapSizeInfo();
			if ((size_t)heap.resourceHeapAlignment > allocAlignment) allocAlignment = (size_t)heap.resourceHeapAlignment;
			if ((size_t)heap.samplerHeapAlignment > allocAlignment) allocAlignment = (size_t)heap.samplerHeapAlignment;
		}
		mData->mBlock = allocator->create(info.size, allocAlignment, info.memoryType, this, &Data_GPU);
		if (!mData->mBlock) {
			Error("Failed to allocate GPU memory for buffer '{}' ({} bytes)", DebugName ? DebugName : "VulkanBuffer", info.size);
			return;
		}
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mData->buffer, VK_OBJECT_TYPE_BUFFER, (DebugName ? DebugName : "VulkanBuffer"));
		VkBufferDeviceAddressInfo addrInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = mData->buffer
		};
		if (mDevice->isDescriptorHeapSupported()) mData->Gpuaddress = vkGetBufferDeviceAddress(mDevice->getLogicalDevice(), &addrInfo);
		if (info.data_CPU) {
			memcpy(Data_GPU, info.data_CPU, info.size);
		}
	}

	VulkanBuffer::~VulkanBuffer() {
		auto allocater = mDevice->getAllocator();
		vkDestroyBuffer(mDevice->getLogicalDevice(), mData->buffer, nullptr);
		if (mData->mBlock) allocater->free(mData->mBlock);
		delete mData;
	}

	size_t VulkanBuffer::getSize() const {
		return mData->size;
	}
	
	void VulkanBuffer::updateBufferData(void* Data, size_t size) {
		if ((mData->memoryType & MemTypHostVisable)) memcpy(Data_GPU, Data, size);
	}

	void* VulkanBuffer::getResourceAPIHandle() const {
		return mData->buffer;
	}

	MemType VulkanBuffer::getResourceMemType() const {
		return mData->memoryType;
	}

	uint32_t VulkanBuffer::getMemoryOffset() const {
		return GetGpuBlockInfo(mData->mBlock).offset;
	}

	uint32_t VulkanBuffer::getMemorySize() const {
		return GetGpuBlockInfo(mData->mBlock).Size;
	}

	uint32_t VulkanBuffer::getVkDescriptorType() const {
		return getDescriptorType(mData->bufferLayout);
	}

	uint64_t  VulkanBuffer::getDeviceAddress() const {
		return mData->Gpuaddress;
	}

}
