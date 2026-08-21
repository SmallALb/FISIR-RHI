#include "VulkanDescriptorPool.h"

#include <unordered_map>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "../RHIResourcePack.h"
#include "../SparseMap.h"
#include "ChangeImageFlagsToVulkanFlags.h"
#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanImageView.h"
#include "VulkanMemory.h"
#include "VulkanSampler.h"
#include "VulkanTexture.h"
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

	/*

		DescriptorSet

	*/
	class DescriptorSet : public RHIResourcePack {
		void InputInSet(const std::vector<RHIResource*>& resources) {
			const uint32_t count = (uint32_t)resources.size();

			// 1. 根据 resources 推断布局绑定与 Pool 尺寸：索引 = binding，类型取自资源
			std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
			layoutBindings.reserve(count);
			std::unordered_map<VkDescriptorType, uint32_t> poolSizeMap;
			for (uint32_t i = 0; i < count; ++i) {
				VkDescriptorType type = (VkDescriptorType)resources[i]->as<VulkanResource>()->getVkDescriptorType();
				layoutBindings.push_back({
					.binding = i,
					.descriptorType = type,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_ALL,
				});
				poolSizeMap[type]++;
			}

			// 2. 根据 resources 创建 Pool
			std::vector<VkDescriptorPoolSize> poolSizes;
			poolSizes.reserve(poolSizeMap.size());
			for (auto& [type, cnt] : poolSizeMap) poolSizes.push_back({ type, cnt });

			VkDescriptorPoolCreateInfo poolInfo{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 1,
				.poolSizeCount = (uint32_t)poolSizes.size(),
				.pPoolSizes = poolSizes.data(),
			};
			if (vkCreateDescriptorPool(mDevice->getLogicalDevice(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
				Error("Failed to create descriptor pool!");
				return;
			}

			// 3. 创建 Set Layout
			VkDescriptorSetLayoutCreateInfo layoutInfo{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = (uint32_t)layoutBindings.size(),
				.pBindings = layoutBindings.data(),
			};
			if (vkCreateDescriptorSetLayout(mDevice->getLogicalDevice(), &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
				Error("Failed to create descriptor set layout!");
				return;
			}

			// 5. 根据 Pool 创建 Set
			VkDescriptorSetAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &setLayout,
			};
			if (vkAllocateDescriptorSets(mDevice->getLogicalDevice(), &allocInfo, &set) != VK_SUCCESS) {
				Error("Failed to allocate descriptor set!");
				return;
			}

			// 6. 写入描述符（reserve 保证 &back() 在后续 push 时不被重分配）
			std::vector<VkWriteDescriptorSet> writes;
			std::vector<VkDescriptorBufferInfo> bufferInfos;
			std::vector<VkDescriptorImageInfo> imageInfos;
			writes.reserve(count);
			bufferInfos.reserve(count);
			imageInfos.reserve(count);

			for (uint32_t i = 0; i < count; ++i) {
				RHIResource* res = resources[i];
				VkDescriptorType type = (VkDescriptorType)res->as<VulkanResource>()->getVkDescriptorType();

				VkWriteDescriptorSet write{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = set,
					.dstBinding = i,
					.dstArrayElement = 0,
					.descriptorCount = 1,
					.descriptorType = type,
				};

				if (res->getResourceType() == Type::Buffer) {
					bufferInfos.push_back({
						.buffer = (VkBuffer)res->getResourceAPIHandle(),
						.offset = 0,
						.range = res->getSize(),
					});
					write.pBufferInfo = &bufferInfos.back();
				}
				else if (res->getResourceType() == Type::Texture) {
					auto* tex = static_cast<VulkanTexture*>(res);
					mViews.push_back(new VulkanImageView(mDevice, tex));
					imageInfos.push_back({
						.sampler = VK_NULL_HANDLE,
						.imageView = mViews.back()->getImageViewHandle(),
						.imageLayout = (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
							? VK_IMAGE_LAYOUT_GENERAL
							: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					});
					write.pImageInfo = &imageInfos.back();
				}
				else if (res->getResourceType() == Type::Sampler) {
					imageInfos.push_back({
						.sampler = (VkSampler)res->getResourceAPIHandle(),
						.imageView = VK_NULL_HANDLE,
						.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					});
					write.pImageInfo = &imageInfos.back();
				}

				writes.push_back(write);
			}

			vkUpdateDescriptorSets(mDevice->getLogicalDevice(), (uint32_t)writes.size(), writes.data(), 0, nullptr);
		}

	public:
		DescriptorSet(VulkanDevice* device, const std::vector<RHIResource*>& resources) {
			mDevice = device;
			resourceType = Type::Buffer;
			InputInSet(resources);
		}

		~DescriptorSet() {
			if (pipelineLayout) vkDestroyPipelineLayout(mDevice->getLogicalDevice(), pipelineLayout, nullptr);
			if (setLayout)       vkDestroyDescriptorSetLayout(mDevice->getLogicalDevice(), setLayout, nullptr);
			if (pool)            vkDestroyDescriptorPool(mDevice->getLogicalDevice(), pool, nullptr);
			for (auto* view : mViews) delete view;
		}

		virtual Type getResourceType() const override { return resourceType; };

		VulkanDevice* mDevice;
		VkDescriptorSet set{};
		VkDescriptorSetLayout setLayout{};
		VkPipelineLayout pipelineLayout{};
		VkDescriptorPool pool{};
		std::vector<VulkanImageView*> mViews;
		Type resourceType;
	};

	/*
		
		VulkanDescriptorPool
	
	*/
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
				if (!res) {
					Error("createResourcePack: null resource in list, skipping");
					continue;
				}
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

		// 降级路径：无 heap 支持时，用单个 DescriptorSet 容纳全部资源（含采样器）。
		// binding = 资源在列表中的索引，与管线 describeInfo 的 0..N-1 绑定一一对应。
		for (auto res : resources) {
			if (!res) {
				Error("createResourcePack: null resource in list");
				return {};
			}
		}
		if (resources.empty()) return {};

		RHIResourcePackResult res;
		res.ResourcePack = new DescriptorSet(mDevice, resources);
		return res;
	}

	void VulkanDescriptorPool::destroyResourcePack(RHIResourcePack* pack) {
		delete pack;
	}

		
	
	void CmdBindResourcePack(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack, uint32_t bindPoint) {
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
			else {
				// 降级路径：绑定单个 DescriptorSet
				auto* setPack = static_cast<DescriptorSet*>(Resourcepack);
				vkCmdBindDescriptorSets(cmd, (VkPipelineBindPoint)bindPoint, setPack->pipelineLayout,
					0, 1, &setPack->set, 0, nullptr);
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
			// 降级路径下 Samplerpack 恒为空（单 set 已包含采样器），无需额外处理
		}

	}

}
