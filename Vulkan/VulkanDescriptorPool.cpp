#include "VulkanDescriptorPool.h"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "../../Log/Logger.h"
#include "../../DataBase/SparseMap.h"
#include "../RHIResourcePack.h"
#include <unordered_map>
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanMemory.h"
namespace FISIR{


	struct DescriptorSizes {
		// 对齐值
		VkDeviceSize resourceHeapAlignment = 0;
		VkDeviceSize samplerHeapAlignment = 0;
		VkDeviceSize bufferAlignment = 0;
		VkDeviceSize imageAlignment = 0;
		VkDeviceSize samplerAlignment = 0;

		// 三类描述符的字节大小
		uint32_t bufferDescriptorSize = 0;   // UBO, SSBO, TexelBuffer, Dynamic UBO/SSBO
		uint32_t imageDescriptorSize = 0;   // SampledImage, StorageImage, InputAttachment
		uint32_t samplerDescriptorSize = 0;   // Sampler, CombinedImageSampler 中的采样器部分

		// 堆的限制
		VkDeviceSize maxResourceHeapSize = 0;
		VkDeviceSize maxSamplerHeapSize = 0;
		VkDeviceSize minResourceReserved = 0;
		VkDeviceSize minSamplerReserved = 0;
		uint32_t     maxEmbeddedSamplers = 0;
	} sizes;


	static void QueryDescriptorSizes(VulkanDevice* Device) {
		VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{};
		heapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;

		VkPhysicalDeviceProperties2 props2{};
		props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props2.pNext = &heapProps;
		vkGetPhysicalDeviceProperties2(Device->getPhysicalDevice(), &props2);

		sizes.resourceHeapAlignment = heapProps.resourceHeapAlignment;
		sizes.samplerHeapAlignment = heapProps.samplerHeapAlignment;
		sizes.bufferAlignment = heapProps.bufferDescriptorAlignment;
		sizes.imageAlignment = heapProps.imageDescriptorAlignment;
		sizes.samplerAlignment = heapProps.samplerDescriptorAlignment;

		sizes.bufferDescriptorSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		sizes.imageDescriptorSize = static_cast<uint32_t>(heapProps.imageDescriptorSize);
		sizes.samplerDescriptorSize = static_cast<uint32_t>(heapProps.samplerDescriptorSize);

		sizes.maxResourceHeapSize = heapProps.maxResourceHeapSize;
		sizes.maxSamplerHeapSize = heapProps.maxSamplerHeapSize;
		sizes.minResourceReserved = heapProps.minResourceHeapReservedRange;
		sizes.minSamplerReserved = heapProps.minSamplerHeapReservedRange;
		sizes.maxEmbeddedSamplers = heapProps.maxDescriptorHeapEmbeddedSamplers;
	}

	uint32_t GetDescriptorSize(VkDescriptorType type) {
		switch (type) {
			// --- Buffer 类：统一用 bufferDescriptorSize ---
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			return sizes.bufferDescriptorSize;

			// --- Image 类：统一用 imageDescriptorSize ---
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			return sizes.imageDescriptorSize;

			// --- Sampler 类 ---
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			return sizes.samplerDescriptorSize;

			// --- Combined Image Sampler = Image + Sampler 拼在一起 ---
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			return sizes.imageDescriptorSize + sizes.samplerDescriptorSize;

			// --- 加速结构：规范里和 Buffer 一样 ---
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			return sizes.bufferDescriptorSize;

		default:
			return 0;
		}
	}

	VkDeviceSize GetDescriptorAlignment(const DescriptorSizes& sizes, VkDescriptorType type) {
		switch (type) {
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			return sizes.bufferAlignment;

		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			return sizes.imageAlignment;

		case VK_DESCRIPTOR_TYPE_SAMPLER:
			return sizes.samplerAlignment;

		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			// Combined 需要同时满足 image 和 sampler 的对齐要求
			return (std::max)(sizes.imageAlignment, sizes.samplerAlignment);
		default:
			return 16; // 安全兜底
		}
	}

	PFN_vkWriteResourceDescriptorsEXT fpWriteResourceDescriptors{ nullptr };
	PFN_vkWriteSamplerDescriptorsEXT fpWriteSamplerDescriptors {nullptr};
	PFN_vkCmdBindResourceHeapEXT fpCmdBindResourceHeap = {nullptr};
	PFN_vkCmdBindSamplerHeapEXT fpCmdBindSamplerHeap = {nullptr};

	/*

	Descriptor Heap

	*/
	class DescriptorHeap : public RHIResourcePack {
		
		
		bool check(Type SrcResTyp, Type DstResTyp) {
			return SrcResTyp == DstResTyp || (SrcResTyp == Type::Buffer && DstResTyp == Type::Texture) || (SrcResTyp == Type::Texture && DstResTyp == Type::Buffer);
		}	

		uint32_t caculateAndCheck(Type ResTyp, const std::vector<RHIResource*>& resources) {
			uint32_t res = 0;
			for (auto& resource : resources) if (check(ResTyp, resource->getResourceType())) {
				res += GetDescriptorSize((VkDescriptorType)(resource->as<VulkanResource>()->getVkDescriptorType()));
			}
			else return 0;
			return res;
		}

		void InputInHeap(const std::vector<RHIResource*>& resources) {
			uint32_t offset = 0;
			for (auto& resource : resources) {
				auto VkHandle = resource->as<VulkanResource>();
				uint32_t currentSize = resource->getSize();
				uint32_t descriptorsize = GetDescriptorSize((VkDescriptorType)VkHandle->getVkDescriptorType());
				if (resource->getResourceType() == Type::Buffer) {
					VkBufferDeviceAddressInfo addrInfo{
					.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
					.buffer = (VkBuffer)resource->getResourceAPIHandle(),
					};

					VkDeviceAddress bufferAddr = vkGetBufferDeviceAddress(mDevice->getLogicalDevice(), &addrInfo);

					VkDeviceAddressRangeEXT range{
						.address = bufferAddr,
						.size = currentSize,
					};

					VkResourceDescriptorInfoEXT desInfo{
						.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
						.type = (VkDescriptorType)VkHandle->getVkDescriptorType(),
						.data = {
							.pAddressRange = &range
						}
					};

					VkHostAddressRangeEXT hostRange{
						.address = (void*)((uint64_t)mHeadBuffer->getHostVisablePtr() + offset),
						.size = descriptorsize
					};

					fpWriteResourceDescriptors(mDevice->getLogicalDevice(), 1, &desInfo, &hostRange);
				}
				else if (resource->getResourceType() == Type::Texture) {
					auto Image = static_cast<VulkanTexture*>(resource);
					VkImageViewCreateInfo viewInfo{
						.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
						.image = (VkImage)Image->getResourceAPIHandle(),
						.viewType = VK_IMAGE_VIEW_TYPE_2D,
						.format = (VkFormat)Image->getVkColorType(),
						.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
					};

					VkImageDescriptorInfoEXT ImageInfo{
						.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
						.pView = &viewInfo,
						.layout = (VkImageLayout)Image->getVkTextureLayout()
					};

					VkResourceDescriptorInfoEXT desInfo{
						.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
						.type = (VkDescriptorType)VkHandle->getVkDescriptorType(),
						.data = {
							.pImage = &ImageInfo,
						}
					};

					VkHostAddressRangeEXT hostRange{
						.address = (void*)((uint64_t)mHeadBuffer->getHostVisablePtr() + offset),
						.size = descriptorsize
					};
					fpWriteResourceDescriptors(mDevice->getLogicalDevice(), 1, &desInfo, &hostRange);

				}
				else if (resource->getResourceType() == Type::Sampler) {
					VkSamplerCreateInfo samplerInfo{
						.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
					};
					VkHostAddressRangeEXT hostRange{
						.address = (void*)((uint64_t)mHeadBuffer->getHostVisablePtr() + offset),
						.size = descriptorsize
					};

					fpWriteSamplerDescriptors(mDevice->getLogicalDevice(), 1, &samplerInfo, &hostRange);

				}

				offset += descriptorsize;
			}
		}


	public:

		DescriptorHeap(VulkanDevice* Device, Type typ, const std::vector<RHIResource*>& resources) {
			//Create Heap
			mDevice = Device;
			resourceType = typ;
			VkDeviceSize reservedSize = (typ == Type::Sampler) ? sizes.minSamplerReserved : sizes.minResourceReserved;

			//--caculate Size	
			UseDataSize = caculateAndCheck(typ, resources);
			uint32_t totalSize = UseDataSize + reservedSize;
			VkDeviceSize alignment = (typ == Type::Sampler) ? sizes.samplerHeapAlignment : sizes.resourceHeapAlignment;
			totalSize = (totalSize + alignment - 1) & ~(alignment - 1);

			//--create buffer
			BufferInfo info{
				.size = totalSize,
				.bufferlayout = BufferLayout::StorageBuffer,
				.memoryType = (MemType)(MemTypHostVisable | MemTypHostCoherent),
			};

			mHeadBuffer = new VulkanBuffer(mDevice, info, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

			resourceType = typ;

			//Input Buffer / Sampler Data
			InputInHeap(resources);
		}
		

		~DescriptorHeap() {
			delete mHeadBuffer;
		}

		VulkanBuffer* mHeadBuffer;
		VulkanDevice* mDevice;
		uint32_t UseDataSize;
		Type resourceType;
	};

	class DescriptorSet : public RHIResourcePack {
	
	
	};


	struct __VKDescriptorPoolData {
		std::unordered_map<RHIPipelineDescribeInfo, VkDescriptorSetLayout> DescriptorSetLayoutMap;

		bool HeapEnable{0};
	};

	VulkanDescriptorPool::VulkanDescriptorPool(VulkanDevice* device) {
		mDevice = device;
		mData = new __VKDescriptorPoolData();
		QueryDescriptorSizes(mDevice);
		mData->HeapEnable = mDevice->isDescriptorHeapSupported();
		if (!mData->HeapEnable) {
			

		}
		else {
			auto logicDevice = mDevice->getLogicalDevice();
			fpWriteResourceDescriptors = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(
				vkGetDeviceProcAddr(logicDevice, "vkWriteResourceDescriptorsEXT"));

			fpWriteSamplerDescriptors = reinterpret_cast<PFN_vkWriteSamplerDescriptorsEXT>(
				vkGetDeviceProcAddr(logicDevice, "vkWriteSamplerDescriptorsEXT"));

			fpCmdBindResourceHeap = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(
				vkGetDeviceProcAddr(logicDevice, "vkCmdBindResourceHeapEXT"));

			fpCmdBindSamplerHeap = reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(
				vkGetDeviceProcAddr(logicDevice, "vkCmdBindSamplerHeapEXT"));
		}


	}
	
	VulkanDescriptorPool::~VulkanDescriptorPool() {
		for (auto& [Info, layout] : mData->DescriptorSetLayoutMap) {
			vkDestroyDescriptorSetLayout(mDevice->getLogicalDevice(), layout, nullptr);
		}
		delete mData;
	}

	VkDescriptorSetLayout_T* VulkanDescriptorPool::createDescriptorSetLayout(const RHIPipelineDescribeInfo& info) {
		if (mData->DescriptorSetLayoutMap.contains(info))
			return mData->DescriptorSetLayoutMap[info];
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		for (const auto& v : info.Bindings) {
			VkDescriptorSetLayoutBinding layoutBinding{
				.binding = v.binding,
				.descriptorType = static_cast<VkDescriptorType>(v.descriptorTyp),
				.descriptorCount = v.count,
				.stageFlags = v.usingStage
			};
			bindings.push_back(layoutBinding);
		}
		VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = static_cast<uint32_t>(bindings.size()),
			.pBindings = bindings.data()
		};
		VkDescriptorSetLayout layout;
		if (vkCreateDescriptorSetLayout(mDevice->getLogicalDevice(), &layoutCreateInfo, nullptr, &layout) != VK_SUCCESS) {
			Error("Failed to create descriptor set layout!");
			return nullptr;
		}
		mData->DescriptorSetLayoutMap[info] = layout;
		return mData->DescriptorSetLayoutMap[info];
	}

	RHIResourcePack* VulkanDescriptorPool::createResourcePack(Type restyp, const std::vector<RHIResource*>& resources) {
		return mData->HeapEnable ? new DescriptorHeap(mDevice, restyp, resources) : nullptr;
	}

	void VulkanDescriptorPool::destroyResourcePack(RHIResourcePack* pack) {
		delete pack;
	}

		
	
	void CmdBindResourcePack(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIResourcePack* pack) {
		if (device->isDescriptorHeapSupported()) {
			auto PackHandle = static_cast<DescriptorHeap*>(pack);
			VkDeviceSize reservedSize = (PackHandle->resourceType == Type::Sampler) ? sizes.minSamplerReserved : sizes.minResourceReserved;
			VkBindHeapInfoEXT info {
				.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
				.heapRange = {
					.address = PackHandle->mHeadBuffer->getDeviceAddress(),
					.size = PackHandle->mHeadBuffer->getSize(),
					
				},
				.reservedRangeOffset = PackHandle->UseDataSize,
				.reservedRangeSize = reservedSize
			};
			PackHandle->resourceType == Type::Sampler ? fpCmdBindSamplerHeap(cmd, &info) : fpCmdBindResourceHeap(cmd, &info);
			
		}
	}

}
