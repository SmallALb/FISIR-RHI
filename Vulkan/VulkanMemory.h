#pragma once
#include <cstdint>
#include <utility>
#include "../RHIResource.h"

struct VkDevice_T;
struct VkDeviceMemory_T;
struct VkBuffer_T;
struct VkImage_T;


namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;
	struct GpuBlock;
	
	//Encapsulates Vulkan details;
	struct __VkMemoryData;

	//the Info of GpuBlock
	struct BlockInfo {
		size_t Size{0};
		size_t offset{0};
		uint32_t MemoryType{0};
		uint32_t MemUsingType{ 0 };
		VkDeviceMemory_T* GpuMemory{nullptr};
	};

	BlockInfo GetGpuBlockInfo(GpuBlock* block);

	class VulkanMemoryAllocator {
	public:
		VulkanMemoryAllocator();
		
		~VulkanMemoryAllocator();

		void init(VulkanDevice* device);

		GpuBlock* create(size_t Size, MemType visable = MemTypeDeviceLocal, RHIResource* resource = nullptr, void** CpuSetPtr = nullptr);
		
		void free(GpuBlock* Block);

		void bindMemoryFor(GpuBlock* block, RHIResource* resource, void** CpuSetPtr = nullptr);



	private:
		std::pair<uint32_t, uint32_t> make_sure_type_exits(MemType require, RHIResource* resource);

		uint32_t get_suitable_type(uint32_t MemBits, MemType require);

	private:
		VulkanDevice* mDevice;	
		__VkMemoryData* mData;
	};
	
}