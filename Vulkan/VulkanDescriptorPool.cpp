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
	extern std::unordered_map<PieplineLayoutHash, VkPipelineLayout_T*>& getPipelineLayoutMap();


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

	static VkDescriptorType ChoiceDescriptorType(RHIDescriptorTyp typ) {
		switch (typ) {
		case RHIDescriptorTyp::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
		case RHIDescriptorTyp::Image: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		case RHIDescriptorTyp::RWImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		case RHIDescriptorTyp::SamplerImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		case RHIDescriptorTyp::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case RHIDescriptorTyp::RBuffer:
		case RHIDescriptorTyp::RWBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		}
		return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	}

	PFN_vkWriteResourceDescriptorsEXT fpWriteResourceDescriptors{ nullptr };
	PFN_vkWriteSamplerDescriptorsEXT fpWriteSamplerDescriptors {nullptr};
	PFN_vkCmdBindResourceHeapEXT fpCmdBindResourceHeap = {nullptr};
	PFN_vkCmdBindSamplerHeapEXT fpCmdBindSamplerHeap = {nullptr};
	// 描述符堆路径下 push constant 由 vkCmdPushDataEXT 承担（见 CmdPushConstant）
	PFN_vkCmdPushDataEXT fpCmdPushDataEXT = {nullptr};

	// 描述符堆里「实现预留区」的起点：**固定常量，与 UseDataSize 无关**。
	// 取固定值是为了满足 VUID-vkCmdBindResourceHeapEXT-pBindInfo-11236 —— 同一段内存不允许
	// 在别的命令缓冲里以**不同的**预留区出现（完全相同才允许）。而堆缓冲是走 suballocator 从
	// 同一个池子里切的，某个堆释放后地址会被下一个堆复用；如果预留区跟着 UseDataSize 走，
	// 复用同一地址的新堆就会和在飞命令缓冲里的旧预留区冲突（实测报过：地址同为 0xf788480，
	// 偏移 128 vs 160）。固定偏移 + 固定预留大小 ⇒ 同一地址永远得到完全相同的预留区。
	// 代价：每个堆多占一点（预留区本来就要 minResourceHeapReservedRange ≈ 94KB，可忽略）。
	constexpr VkDeviceSize kDescriptorHeapReservedOffset = 4096;

	// 预留区**大小**也必须与堆类型无关，取资源堆 / 采样器堆两者的较大值。
	// 理由与固定偏移相同：同一段内存被复用时预留区要**完全相同**。堆缓冲是 suballocator 从
	// 同一个池子里切的，采样器堆释放后地址可能被资源堆复用，而两者的
	// minResourceHeapReservedRange / minSamplerHeapReservedRange 通常不同 → 预留区就不同了，
	// 校验层会报 VUID-11236。统一取较大值对两者都不小于规范要求的最小值，合法。
	inline VkDeviceSize DescriptorHeapReservedBytes(const DescriptorSizes& sizes) {
		return (sizes.minResourceReserved > sizes.minSamplerReserved)
			? sizes.minResourceReserved : sizes.minSamplerReserved;
	}

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
					// 视图类型与子资源范围必须**从纹理本身推导**，不能写死 2D + 单 mip 单层：
					//   · TEXTUREARRAY（arrayLayers == 6，VkImage 带 CUBE_COMPATIBLE）需要的是
					//     VK_IMAGE_VIEW_TYPE_CUBE；写死 2D 会让着色器里的 TextureCube 只看到
					//     第 0 张 face，环境贴图采样退化成一张平面图。
					//   · mipLevels > 1 的纹理（IBL 的预滤波环境图）写死 levelCount = 1 会把
					//     LOD 钳在第 0 级，按粗糙度取 mip 的预滤波反射全部退化成镜面反射。
					// 取值方式与 VulkanImageView::build 完全一致（顺带修正深度/模板纹理的 aspect）。
					VkImageViewCreateInfo viewInfo{
						.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
						.image = (VkImage)Image->getResourceAPIHandle(),
						.viewType = (VkImageViewType)getVulkanViewTypeFromTextureType(Image->getTextureType()),
						.format = (VkFormat)Image->getVkColorType(),
						.subresourceRange = {
							.aspectMask = (VkImageAspectFlags)getVulkanAspectFlagsForUsing(Image->getTextureUseFor()),
							.baseMipLevel = 0,
							.levelCount = Image->getMipLevelCount(),
							.baseArrayLayer = 0,
							.layerCount = Image->getLayerCount(),
						}
					};

					// 布局按**描述符类型**固定，不再读创建瞬间的 currentLayout：
					// 纹理的 currentLayout 由资源线程在围栏置位后回填（VulkanRHI 的
					// QuoteResources → transitionLayout），而资源包完全可能在该回填之前创建
					//（发起转换命令后 fence->wait() 返回即建包），此时读到的是 Undefined，
					// 会被原样烧进描述符堆。与 DescriptorSet 降级路径（同样固定
					// SHADER_READ_ONLY_OPTIMAL）统一后两条路径行为一致，也不再依赖创建时序。
					const bool asStorageImage = ((VkDescriptorType)VkHandle->getVkDescriptorType() == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
					VkImageDescriptorInfoEXT ImageInfo{
						.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
						.pView = &viewInfo,
						.layout = asStorageImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
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
						// 曾被误写成 info.minLop：默认 SamplerInfo 的 minLop == maxLop == 1.0
						// 时看不出差别，但预滤波环境图（mipLevels > 1，按粗糙度取 LOD）会被
						// 钳在 1.0 以内，粗糙度反射全部退化成镜面。
						.maxLod = info.maxLop,
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
			reservedSize = DescriptorHeapReservedBytes(sizes);

			//--caculate Size
			UseDataSize = caculateAndCheck(typ, resources);
			VkDeviceSize alignment = (typ == Type::Sampler) ? sizes.samplerHeapAlignment : sizes.resourceHeapAlignment;
			// 宿主缓冲必须覆盖到「预留区起点 + 预留大小」：预留区固定在 kDescriptorHeapReservedOffset，
			// 不再是 align(UseDataSize)。长度按对齐向上取整，保证预留区完整落在本缓冲内 ——
			// 越界就会落到紧邻的分配里（描述符堆缓冲是走 suballocator 从同一个大池子里切的）。
			// 上一版按 align(UseDataSize) 起算，已经踩过一次 reserved range 冲突。
			uint32_t totalSize = (uint32_t)((kDescriptorHeapReservedOffset + reservedSize + alignment - 1) & ~(alignment - 1));

			//--create buffer
			BufferInfo info{
				.size = totalSize,
				.bufferlayout = BufferLayout::UndefinedBuffer, // 描述符堆宿主缓冲，非着色器直接访问
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

			// 4. 为 vkCmdBindDescriptorSets 准备 pipeline layout
			//    （自带 setLayout，与 pipeline 的 layout 兼容即可，无需是同一个 handle）
			VkPipelineLayoutCreateInfo pipelineLayoutInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &setLayout,
			};
			if (vkCreatePipelineLayout(mDevice->getLogicalDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
				Error("Failed to create pipeline layout!");
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

			fpCmdPushDataEXT = reinterpret_cast<PFN_vkCmdPushDataEXT>(
				vkGetDeviceProcAddr(logicDevice, "vkCmdPushDataEXT"));
		}


	}
	
	VulkanDescriptorPool::~VulkanDescriptorPool() {
		for (auto& [Info, layout] : mData->DescriptorSetLayoutMap) {
			vkDestroyDescriptorSetLayout(mDevice->getLogicalDevice(), layout, nullptr);
		}
		delete mData;
	}

	VkDescriptorSetLayout_T* VulkanDescriptorPool::createDescriptorSetLayout(const RHIPipelineDescribeInfo& info, const RHIPushConstantRange& pcRange, VkPipelineLayout_T*& Pipelinelayout, const std::vector<uint32_t>& bindingMap) {
		auto& PipelineLayoutMap = getPipelineLayoutMap();
		PieplineLayoutHash HashVal(info, pcRange, (uint32_t)pcRange.usingStage);

		// push constant 范围：只有声明了 size 才进布局。顶点/片元/计算三个阶段按位选，
		// 与描述符绑定的 stage 选择共用同一套映射。
		VkPushConstantRange pcRangeVk{};
		if (pcRange.size > 0) {
			pcRangeVk.stageFlags = ChoiceDescriptorStage(pcRange.usingStage);
			pcRangeVk.offset     = pcRange.offset;
			pcRangeVk.size       = pcRange.size;
		}

		std::vector<VkDescriptorSetLayoutBinding> bindings;
		if (!mDevice->isDescriptorHeapSupported()) {
			// ── 降级路径（无 VK_EXT_descriptor_heap）────────────────────────────
			// binding = 资源在 describeInfo / ResourcePack 里的**下标**，两条硬约束：
			//   · 同一个 set 内 binding 必须唯一 —— 采样器不能再和常量缓冲/纹理共用寄存器号
			//     （describeInfo 里允许重复号是给描述符堆用的：堆按「资源类型」区分采样器与
			//      资源，经典 DescriptorSet 没有这个余地）；
			//   · 着色器里 register(...) 的号必须等于它在 describeInfo 里的下标
			//     （Nanite 那一套 u0..u12 + b4/b5/b8 就是照这个排的）。
			// stageFlags 用 ALL：与 DescriptorSet 侧（资源包）自建的布局逐字段一致，
			// 于是 vkCmdBindDescriptorSets 无论传哪一边的 pipelineLayout 都满足兼容性要求。
			auto cached = mData->DescriptorSetLayoutMap.find(info);
			if (cached != mData->DescriptorSetLayoutMap.end()) {
				if (!PipelineLayoutMap.contains(HashVal)) {
					VkPipelineLayoutCreateInfo PipelineLayoutInfo{
						.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
						.setLayoutCount = 1,
						.pSetLayouts = &cached->second,
						.pushConstantRangeCount = pcRange.size > 0 ? 1u : 0u,
						.pPushConstantRanges = pcRange.size > 0 ? &pcRangeVk : nullptr,
					};
					vkCreatePipelineLayout(mDevice->getLogicalDevice(), &PipelineLayoutInfo, nullptr, &Pipelinelayout);
					PipelineLayoutMap[HashVal] = Pipelinelayout;
				}
				else Pipelinelayout = PipelineLayoutMap[HashVal];
				return cached->second;
			}

			uint32_t index = 0;
			for (const auto& v : info.Bindings) {
				VkDescriptorSetLayoutBinding layoutBinding{
					.binding = index,
					.descriptorType = ChoiceDescriptorType(v.descriptorTyp),
					.descriptorCount = v.count,
					.stageFlags = VK_SHADER_STAGE_ALL
				};
				bindings.push_back(layoutBinding);
				index++;
			}
		}
		VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = static_cast<uint32_t>(bindings.size()),
			.pBindings = mDevice->isDescriptorHeapSupported() ?  nullptr : bindings.data()
		};
		VkDescriptorSetLayout layout;
		if (vkCreateDescriptorSetLayout(mDevice->getLogicalDevice(), &layoutCreateInfo, nullptr, &layout) != VK_SUCCESS) {
			Error("Failed to create descriptor set layout!");
			return nullptr;
		}
		mData->DescriptorSetLayoutMap[info] = layout;


		if (!PipelineLayoutMap.contains(HashVal)) {
			VkPipelineLayoutCreateInfo PipelineLayoutInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &layout,
				.pushConstantRangeCount = pcRange.size > 0 ? 1u : 0u,
				.pPushConstantRanges = pcRange.size > 0 ? &pcRangeVk : nullptr,
			};
			vkCreatePipelineLayout(mDevice->getLogicalDevice(), &PipelineLayoutInfo, nullptr, &Pipelinelayout);
			PipelineLayoutMap[HashVal] = Pipelinelayout;
		}
		else Pipelinelayout = PipelineLayoutMap[HashVal];
		
		return mData->DescriptorSetLayoutMap[info];
	}

	RHIResourcePackResult VulkanDescriptorPool::createResourcePack(const std::vector<RHIResource*>& resources) {
		if (mDevice->isDescriptorHeapSupported()) {
			Debug("Create Vk Heap");
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
		Debug("Create Vk Descriptor Set");
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

		
	
	// 预留区用固定偏移 kDescriptorHeapReservedOffset（声明在文件上方，理由见那里）。
	void CmdPushConstant(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIPipeline* pipeline,
	                     uint32_t offset, uint32_t size, const void* data, RHIUsingStageFlags stage) {
		if (!cmd || !data || size == 0) return;

		if (device->isDescriptorHeapSupported()) {
			// 描述符堆路径：push constant 走 vkCmdPushDataEXT。
			// （该扩展下 vkCmdPushConstants 不会真正写入，校验层会报
			//   "uses push-constant statically ... no call to vkCmdPushDataEXT"。）
			if (!fpCmdPushDataEXT) return;
			VkPushDataInfoEXT info{
				.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
				.pNext = nullptr,
				.offset = offset,
				.data = { data, (size_t)size },
			};
			fpCmdPushDataEXT(cmd, &info);
			return;
		}

		if (!pipeline) return;
		vkCmdPushConstants(cmd, static_cast<VkPipelineLayout>(pipeline->getPipelineLayoutHandle()),
			ChoiceDescriptorStage(stage), offset, size, data);
	}

	void CmdBindResourcePack(VulkanDevice* device, VkCommandBuffer_T* cmd, RHIPipeline* pipeline, RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack, uint32_t bindPoint) {
		const auto& sizes = device->getHeapSizeInfo();
		if (Resourcepack) {
			if (device->isDescriptorHeapSupported()) {
				auto PackHandle = static_cast<DescriptorHeap*>(Resourcepack);
				VkDeviceSize reservedSize = DescriptorHeapReservedBytes(sizes);
				VkBindHeapInfoEXT info{
					.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
					.heapRange = {
						.address = PackHandle->mHeadBuffer->getDeviceAddress(),
						.size = PackHandle->mHeadBuffer->getSize(),

					},
					.reservedRangeOffset = kDescriptorHeapReservedOffset,
					.reservedRangeSize = reservedSize
				};
				fpCmdBindResourceHeap(cmd, &info);
			}
			else {
				// 降级路径：绑定单个 DescriptorSet。
				// pipelineLayout 用**当前管线自己的**布局，而不是资源包自建的那个：
				// 规范要求 vkCmdBindDescriptorSets 传入的布局与管线创建时的布局「兼容」，
				// 而资源包自建的布局不含 push constant 范围 —— 实测 HZBBuild（唯一带 push
				// constant 的降级路径管线）会报 "set 0 is not compatible with the pipeline
				// layout bound"。set 本身与管线布局逐字段一致（绑定号/类型/数量/阶段），
				// 所以借用管线的布局既合法又最稳。
				auto* setPack = static_cast<DescriptorSet*>(Resourcepack);
				VkPipelineLayout layout = pipeline
					? static_cast<VkPipelineLayout>(pipeline->getPipelineLayoutHandle())
					: setPack->pipelineLayout;
				vkCmdBindDescriptorSets(cmd, (VkPipelineBindPoint)bindPoint, layout,
					0, 1, &setPack->set, 0, nullptr);
			}
		}

		if (Samplerpack) {
			if (device->isDescriptorHeapSupported()) {
				auto PackHandle = static_cast<DescriptorHeap*>(Samplerpack);
				VkDeviceSize reservedSize = DescriptorHeapReservedBytes(sizes);
				VkBindHeapInfoEXT info{
					.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
					.heapRange = {
						.address = PackHandle->mHeadBuffer->getDeviceAddress(),
						.size = PackHandle->mHeadBuffer->getSize(),
					},
					.reservedRangeOffset = kDescriptorHeapReservedOffset,
					.reservedRangeSize = reservedSize
				};
				fpCmdBindSamplerHeap(cmd, &info);


			}
			// 降级路径下 Samplerpack 恒为空（单 set 已包含采样器），无需额外处理
		}

	}

}
