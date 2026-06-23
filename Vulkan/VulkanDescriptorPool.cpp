#include "VulkanDescriptorPool.h"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "../../Log/Logger.h"
#include "../SparseMap.h"
#include "../RHIResourcePack.h"
#include <unordered_map>
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanMemory.h"
#include "VulkanSampler.h"
#include "ChangeImageFlagsToVulkanFlags.h"
namespace FISIR{


	static VkShaderStageFlags ChoiceDescriptorStage(RHIUsingStageFlags stage) {
		Debug("ChoiceDescriptorStage called with stage = {}", (uint32_t)stage);
		VkShaderStageFlags res = 0;
		if ((stage & VertexShaderStage)) res |= VK_SHADER_STAGE_VERTEX_BIT;
		if ((stage & FragmentShaderStage)) res |= VK_SHADER_STAGE_FRAGMENT_BIT;
		if ((stage & ComputeShaderStage)) res |= VK_SHADER_STAGE_COMPUTE_BIT;
		return res;
	}


	static uint32_t GetDescriptorSize(VulkanDevice* device, VkDescriptorType type) {
		const auto& sizes = device->getHeapSizeInfo();
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

	VkDeviceSize GetDescriptorAlignment(VulkanDevice* device, VkDescriptorType type) {
		const auto& sizes = device->getHeapSizeInfo();
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

		uint32_t caculateAndCheck(Type ResTyp, const std::vector<RHIResource*>& resources) {
			uint32_t res = 0;
			for (auto& resource : resources) {
				res += GetDescriptorSize(mDevice, (VkDescriptorType)(resource->as<VulkanResource>()->getVkDescriptorType()));
			}
			
			return res;
		}

		void InputInHeap(const std::vector<RHIResource*>& resources) {
			uint32_t offset = 0;
			for (auto& resource : resources) {
				auto VkHandle = resource->as<VulkanResource>();
				uint32_t currentSize = resource->getSize();
				uint32_t descriptorsize = GetDescriptorSize(mDevice, (VkDescriptorType)VkHandle->getVkDescriptorType());
				uint32_t alignment = GetDescriptorAlignment(mDevice, (VkDescriptorType)VkHandle->getVkDescriptorType());

				if (alignment > 0) {
					offset = (offset + alignment - 1) & ~(alignment - 1);
				}

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
					auto sampler = static_cast<VulkanSampler*>(resource);
					auto info = sampler->getSamplerInfo();
					VkSamplerCreateInfo samplerInfo{
						.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
						.magFilter = getVkFilter(info.enlagerFilter),
						.minFilter = getVkFilter(info.minFilter),
						.mipmapMode = getVkMipMapMode(info.mipMapMode),
						.addressModeU = getVkSamplerAddressMode(info.u),
						.addressModeV = getVkSamplerAddressMode(info.v),
						.addressModeW = getVkSamplerAddressMode(info.w),
						.mipLodBias = info.mipLodBias,
						.anisotropyEnable = info.anisotropyEnable,
						.maxAnisotropy = info.maxAnisotropy,
						.compareEnable = info.compareEnable,
						.compareOp = getVkOperation(info.compareOP),
						.minLod = info.minLop,
						.maxLod = info.minLop,
						.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
						.unnormalizedCoordinates = info.unNormalized,
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
			const auto& sizes = Device->getHeapSizeInfo();
			//Create Heap
			mDevice = Device;
			resourceType = typ;
			reservedSize = (typ == Type::Sampler) ? sizes.minSamplerReserved : sizes.minResourceReserved;

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


		virtual Type getResourceType() const {return resourceType;};


		~DescriptorHeap() {
			delete mHeadBuffer;
		}
		VkDeviceSize reservedSize;
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
				.stageFlags = ChoiceDescriptorStage(v.usingStage)
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

	RHIResourcePackResult VulkanDescriptorPool::createResourcePack(const std::vector<RHIResource*>& resources) {
		if (mData->HeapEnable) {
			std::vector<RHIResource*> resourceList;   // Buffer 和 Texture
			std::vector<RHIResource*> samplerList;    // Sampler

			for (auto res : resources) {
				if (res->getResourceType() == Type::Sampler) {
					samplerList.push_back(res);
				}
				else {
					resourceList.push_back(res);
				}
			}
			
			if (resourceList.empty() && samplerList.empty()) return {};
			
			RHIResourcePackResult res;

			if (!resourceList.empty()) {
				res.ResourcePack = new DescriptorHeap(mDevice, Type::Buffer, resourceList);
			}
			if (!samplerList.empty()) {
				res.SamplerPack = new DescriptorHeap(mDevice, Type::Sampler, samplerList);
			}

			return res;
		}

		return {};
	}

	void VulkanDescriptorPool::destroyResourcePack(RHIResourcePack* pack) {
		delete pack;
	}

		
	
	void CmdBindResourcePack(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack) {
		const auto& sizes = device->getHeapSizeInfo();
		if (Resourcepack) {
			if (device->isDescriptorHeapSupported()) {
				auto PackHandle = static_cast<DescriptorHeap*>(Resourcepack);
				Debug("PackHandle->resourceType = {}", (int)PackHandle->resourceType);
				VkDeviceSize reservedSize = (PackHandle->resourceType == Type::Sampler) ? sizes.minSamplerReserved : sizes.minResourceReserved;
				VkDeviceSize alignment = (PackHandle->resourceType == Type::Sampler) ? sizes.samplerHeapAlignment : sizes.resourceHeapAlignment;
				VkDeviceSize alignedOffset = (PackHandle->UseDataSize + alignment - 1) & ~(alignment - 1);
				VkBindHeapInfoEXT info{
					.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
					.heapRange = {
						.address = PackHandle->mHeadBuffer->getDeviceAddress(),
						.size = PackHandle->mHeadBuffer->getSize(),

					},
					.reservedRangeOffset = alignedOffset,
					.reservedRangeSize = reservedSize
				};
				fpCmdBindResourceHeap(cmd, &info);
				Debug("Bind ResourcePack : RangeSize : {}", reservedSize);
			}
		}

		if (Samplerpack) {
			if (device->isDescriptorHeapSupported()) {
				auto PackHandle = static_cast<DescriptorHeap*>(Samplerpack);
				VkDeviceSize reservedSize = (PackHandle->resourceType == Type::Sampler) ? sizes.minSamplerReserved : sizes.minResourceReserved;
				VkBindHeapInfoEXT info{
					.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
					.heapRange = {
						.address = PackHandle->mHeadBuffer->getDeviceAddress(),
						.size = PackHandle->mHeadBuffer->getSize(),
					},
					.reservedRangeOffset = PackHandle->UseDataSize,
					.reservedRangeSize = reservedSize
				};
				fpCmdBindSamplerHeap(cmd, &info);


			}
		
		}

	}

}
