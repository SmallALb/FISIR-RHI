#include "VulkanBuffer.h"

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "VulkanDebugNameSet.h"
#include "VulkanDevice.h"
#include "VulkanMemory.h"

namespace FISIR {	


	static VkDescriptorType getDescriptorType(BufferLayoutFlags type) {
		if (type & UniformBuffer) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		if (type & StorageBuffer) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		
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
		if (layout & StorageBuffer) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
		VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = info.size,
			.usage = (Usage ? (VkBufferUsageFlags)Usage : getBufferUsage(info.bufferlayout)) | 
					(mDevice->isDescriptorHeapSupported() ? (VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) : 0),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE
		};
		vkCreateBuffer(mDevice->getLogicalDevice(), &bufferInfo, nullptr, &mData->buffer);

		VkMemoryRequirements memReqs;
		vkGetBufferMemoryRequirements(mDevice->getLogicalDevice(), mData->buffer, &memReqs);


		mData->mBlock = allocator->create(info.size, memReqs.alignment, info.memoryType, this, &Data_GPU);
		Debug("Create Buffer: {}", info.size);
		setVkObjectName(mDevice->getLogicalDevice(), (uint64_t)mData->buffer, VK_OBJECT_TYPE_BUFFER, (DebugName ? DebugName : "VulkanBuffer"));
		VkBufferDeviceAddressInfo addrInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = mData->buffer
		};
		mData->Gpuaddress = vkGetBufferDeviceAddress(mDevice->getLogicalDevice(), &addrInfo);
		Debug("Buffer created with device address: 0x{:x}", mData->Gpuaddress);
		if (info.data_CPU) {
			memcpy(Data_GPU, info.data_CPU, info.size);
		}
	}

	VulkanBuffer::~VulkanBuffer() {
		auto allocater = mDevice->getAllocator();
		vkDestroyBuffer(mDevice->getLogicalDevice(), mData->buffer, nullptr);
		allocater->free(mData->mBlock);
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
