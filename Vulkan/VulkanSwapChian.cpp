#include "VulkanSwapChian.h"

#include <algorithm>
#include <vector>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "../RHIBuffer.h"
#include "../RHISampler.h"
#include "../RHIShader.h"
#include "../RHITexture.h"
#include "../ShaderComplier.h"
#include "VulkanBuffer.h"
#include "VulkanCommandPool.h"
#include "VulkanDevice.h"
#include "VulkanFrameBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"
#include "VulkanRenderPass.h"
#include "VulkanRHI.h"
#include "VulkanTexture.h"
#include "VulkanViewport.h"


static const wchar_t* FullscreenVS = LR"(
		struct VSInput {
			uint vertexIndex : SV_VertexID;
		};
		struct VSOutput {
			float4 position : SV_POSITION;
			float2 uv : TEXCOORD0;
		};
		[shader("vertex")]
		VSOutput main(VSInput input) {
			VSOutput output;
			float2 vertices[3] = {
				float2(-1.0, -1.0),
				float2( 3.0, -1.0),
				float2(-1.0,  3.0)
			};
			float2 uvCoords[3] = {
				float2(0.0, 1.0),
				float2(2.0, 1.0),
				float2(0.0, -1.0)
			};
			output.position = float4(vertices[input.vertexIndex], 0.0, 1.0);
			output.uv = uvCoords[input.vertexIndex];
			return output;
		}
		)";

static const wchar_t* FullscreenPS = LR"(
		Texture2D<float4> g_OffscreenTexture : register(t0);
		ByteAddressBuffer g_FrameBuffer : register(t1);


		SamplerState g_LinearSampler : register(s2);

		cbuffer BufferToOutPutData : register(b3) {
			uint2 viewport;
			uint  BufferEnable;
		}

		struct PSInput {
			float4 position : SV_POSITION;
			float2 uv : TEXCOORD0;
		};

		[shader("pixel")]
		float4 main(PSInput input) : SV_Target {
			if (BufferEnable) {
				uint2 pixel = uint2(input.uv * float2(viewport));
				uint pixelIndex = pixel.y * viewport.x + pixel.x;
				uint packed = g_FrameBuffer.Load(pixelIndex * 8 + 0);
				float3 rgb = float3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu) / 255.0f;
				return float4(rgb, 1.0f);
			}
			return g_OffscreenTexture.Sample(g_LinearSampler, input.uv);
		}
		)";



namespace FISIR {

    static RHIShader* VShader = nullptr;
    static RHIShader* FShader = nullptr;

    struct __VkSwapChainData {
        VkSwapchainKHR swapchain {VK_NULL_HANDLE};
        VkImage swapChainImageHandles[MAX_SWAPCHAIN_FRAME] {VK_NULL_HANDLE};
    };


    VulkanSwapChain::VulkanSwapChain(VulkanViewport* viewport,  uint32_t QueFamilyIndex) {
        mData = new __VkSwapChainData();
        Surfaceviewport = viewport;
        mPresentQueFamilyIndex = QueFamilyIndex;
    }

    bool VulkanSwapChain::init(VulkanDevice* device, DynamicRHI* rhi) {
        usingRHI = rhi;
        mDevice = device;
        Debug("Try Init SwapChain 0x{:x}", (size_t)this);
        if (VShader == nullptr) {
            ShaderComplier vsCompiler, psCompiler, psBufferCompiler;
            vsCompiler.compileShader(FullscreenVS, wcslen(FullscreenVS) * sizeof(wchar_t), L"main", L"vs_6_0");
            psCompiler.compileShader(FullscreenPS, wcslen(FullscreenPS) * sizeof(wchar_t), L"main", L"ps_6_0");
            VShader = usingRHI->RHICreateShader(ShaderTYP::__VERTEXSHADER__, "main", vsCompiler.getShaderData(), vsCompiler.getShaderDataSize());
            FShader = usingRHI->RHICreateShader(ShaderTYP::__FRAGMENTSHADER__, "main", psCompiler.getShaderData(), psCompiler.getShaderDataSize());
        }

        if (!PresentQueue) PresentQueue = new VulkanQueue(mDevice, mPresentQueFamilyIndex, "SwapChainPresentQue");

        // swapchain 自带的 BufferToOutPutData cbuffer 后备缓冲，默认为关闭（BufferEnable=0，
        // PS 退回采样纹理）。示例把此缓冲绑到 b3；enableBufferInput 后置 viewport 并开启。
        if (!BufferToOutPutData) {
            FISIR::BufferInfo obInfo{
                .size = 16,
                .bufferlayout = FISIR::UniformBuffer,
                .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
            };
            BufferToOutPutData = new VulkanBuffer(mDevice, obInfo, 0, "SwapChainBufferToOutPutData");
        }
        uint32_t defaultOutPut[4] = { 0, 0, 0, 0 };   // viewport=0, BufferEnable=0
        BufferToOutPutData->updateBufferData(defaultOutPut, sizeof(defaultOutPut));

        return createPipelineandRenderPass() && createSwapChian();
    }

    void VulkanSwapChain::UpdateOutputData(uint32_t width, uint32_t height, uint32_t bufferEnable) {
        if (!BufferToOutPutData) return;
        uint32_t outPut[4] = { width, height, bufferEnable, 0 };
        BufferToOutPutData->updateBufferData(outPut, sizeof(outPut));
    }

    RHIBuffer* VulkanSwapChain::EnsureFallbackBuffer() {
        if (!FallbackBuffer) {
            FISIR::BufferInfo info{
                .size = 64,
                .bufferlayout = FISIR::RBuffer,
                .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
            };
            FallbackBuffer = usingRHI->RHICreateBuffer(info);
        }
        return FallbackBuffer;
    }

    RHITexture* VulkanSwapChain::EnsureFallbackTexture() {
        if (!FallbackTexture) {
            FISIR::TextureInfo info{
                .size = {1, 1, 1},   // TextureSize 字段序 {height, width, depth}
                .colorType = FISIR::TextureCOLORType::RGBA_8,
                .type = FISIR::TextureType::TEXTURE2D,
                .useFor = FISIR::TextureUseForShaderReadOnly,
                .mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,   // 0 = 非 MSAA（本 RHI 约定）
            };
            FallbackTexture = usingRHI->RHICreateTexture(info);
        }
        return FallbackTexture;
    }

    RHISampler* VulkanSwapChain::EnsureFallbackSampler() {
        if (!FallbackSampler) {
            FISIR::SamplerInfo info;
            FallbackSampler = usingRHI->RHICreateSampler(info);
        }
        return FallbackSampler;
    }

    void VulkanSwapChain::RebuildPresentPack() {
        if (PresentResourcePack.ResourcePack || PresentResourcePack.SamplerPack) {
            usingRHI->RHIDestroyResourcePack(PresentResourcePack);
        }

        // 槽位与管线 describeInfo 顺序一致：0=texture(t0), 1=frameBuffer(t1), 2=sampler(s2), 3=cbuffer(b3)。
        // texture 模式（BufferEnabled=false）时 t1 用占位缓冲；buffer 模式未给纹理/采样器时用占位纹理/采样器。
        RHITexture* tex = PresentTexture ? PresentTexture : EnsureFallbackTexture();
        RHIBuffer*  fb  = (BufferEnabled && PresentBuffer) ? PresentBuffer : EnsureFallbackBuffer();
        RHISampler* sam = PresentSampler ? PresentSampler : EnsureFallbackSampler();

        std::vector<RHIResource*> resources;
        resources.reserve(4);
        resources.push_back(tex);
        resources.push_back(fb);
        resources.push_back(sam);
        resources.push_back(BufferToOutPutData);

        PresentResourcePack = usingRHI->RHICreateResourcePack(resources);
    }

    void VulkanSwapChain::enableTextureInput(RHITexture* texture, RHISampler* sampler) {
        PresentBuffer = nullptr;            // 切到纹理模式，清掉缓冲模式输入
        PresentTexture = texture;
        PresentSampler = sampler;
        BufferEnabled = false;
        UpdateOutputData(0, 0, 0);          // BufferEnable=0
        RebuildPresentPack();
    }

    void VulkanSwapChain::enableBufferInput(uint32_t width, uint32_t height, RHIBuffer* frameBuffer) {
        PresentTexture = nullptr;           // 切到缓冲模式，清掉纹理模式输入
        PresentSampler = nullptr;
        PresentBuffer = frameBuffer;
        BufferEnabled = true;
        UpdateOutputData(width, height, 1); // BufferEnable=1
        RebuildPresentPack();
    }

    RHIResourcePackResult VulkanSwapChain::getSwapchainResourcePack() const {
        return PresentResourcePack;
    }

    VulkanSwapChain::~VulkanSwapChain() {
        if (PresentResourcePack.ResourcePack || PresentResourcePack.SamplerPack) {
            if (usingRHI) usingRHI->RHIDestroyResourcePack(PresentResourcePack);
        }
        if (FallbackTexture) usingRHI->RHIDestroyTexture(FallbackTexture);
        if (FallbackSampler) usingRHI->RHIDestroySampler(FallbackSampler);
        if (FallbackBuffer)  usingRHI->RHIDestroyBuffer(FallbackBuffer);
        for (uint32_t i = 0; i < MaxSwapChianFramCount; i++)  {
            delete SwapChainTextures[i];
            delete SwapChainFrameBuffers[i];
        }
        if (mData->swapchain) vkDestroySwapchainKHR(mDevice->getLogicalDevice(), mData->swapchain, nullptr);
        delete BufferToOutPutData;
        delete PresentQueue;
    }


    uint32_t VulkanSwapChain::acquireGetImageInfoID() {

        if (!mData->swapchain || needReBuildSwapChain.load(std::memory_order_acquire)) {
            if (!recreateSwapChain()) return RHISwapChain::FAILEID;
        }

        CurrentFrameID %= SWAPCHAIN_SLOT_COUNT;

        auto& [avaliable, renderFinish, finishFence, index] = SwapChainFrameInfos[CurrentFrameID];
        if (index != UINT32_MAX) {
            //finishFence->wait();
        }

        auto res = vkAcquireNextImageKHR(
            mDevice->getLogicalDevice(),
            mData->swapchain,
            UINT64_MAX,
            static_cast<VkSemaphore>(avaliable->getSemaphoreHandle()),
            VK_NULL_HANDLE,
            &index
        );
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
            Error("SwapChain Out Of Data!");
            needReBuildSwapChain.store(1,std::memory_order_release);
            return RHISwapChain::FAILEID;
        }

        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            //Error("Acquire next image failed: {}", (uint32_t)res);
            return RHISwapChain::FAILEID;
        }

        // VK_KHR_swapchain_maintenance1: ensure the image has left the
        // display engine before we start rendering to it again.
        if (mDevice->isSwapchainMaintenance1Supported() && PresentFence[index]) {
            PresentFence[index]->wait();
        }

        finishFence->reset();
        return CurrentFrameID++;
    }

    void VulkanSwapChain::present(uint32_t infoid) {
        if (infoid >= SWAPCHAIN_SLOT_COUNT) return;

        auto& slot = SwapChainFrameInfos[infoid];

        if (!PresentQueue || !mData->swapchain) {
            Error("PresentQueue is empty or swapchainHandle not exits!");
            return;
        }

        VkFence vkFence = VK_NULL_HANDLE;
        VkSwapchainPresentFenceInfoKHR presentFenceInfo{};
        if (mDevice->isSwapchainMaintenance1Supported()) {
            PresentFence[slot.imageIndex]->reset();
            vkFence = static_cast<VkFence>(PresentFence[slot.imageIndex]->getFenceHandle());
            presentFenceInfo = {
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
                .swapchainCount = 1,
                .pFences = &vkFence,
            };
        }

        VkSemaphore waitSem = static_cast<VkSemaphore>(ImageRenderFinish[slot.imageIndex]->getSemaphoreHandle());
        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = mDevice->isSwapchainMaintenance1Supported() ? &presentFenceInfo : nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &waitSem,
            .swapchainCount = 1,
            .pSwapchains = &mData->swapchain,
            .pImageIndices = &slot.imageIndex,
            .pResults = nullptr
        };

        vkQueuePresentKHR(PresentQueue->getQueueHandle(), &presentInfo);
    }

    RHITexture* VulkanSwapChain::getSwapChainFrameTexture(uint32_t imageindex) const {
        if (imageindex < MaxSwapChianFramCount) return SwapChainTextures[imageindex];
        else return nullptr;
    }

    uint32_t VulkanSwapChain::getImageCount() const {
        return MaxSwapChianFramCount;
    }

    SwapChainGetImageInfo VulkanSwapChain::getSwapChainGetImageInfo(uint32_t id) {
        if (id >= SWAPCHAIN_SLOT_COUNT) return {};
        auto& slot = SwapChainFrameInfos[id];
        SwapChainGetImageInfo info = slot;
        // Override renderFinish with the per-image present semaphore so
        // present() waits on the correct semaphore per swapchain image.
        info.renderFinish = ImageRenderFinish[slot.imageIndex];
        return info;
    }

    RHIFrameBuffer* VulkanSwapChain::getSwapChainFrameBuffer(uint32_t imageindex) {
        if (imageindex < MaxSwapChianFramCount) return SwapChainFrameBuffers[imageindex];
        return nullptr;
    }

    RHIPipeline* VulkanSwapChain::getSwapChainRenderPipeline() const {
        return VulkanViewportPipeline;
    }

    bool VulkanSwapChain::recreateSwapChain() {
        vkDeviceWaitIdle(mDevice->getLogicalDevice());
        return createSwapChian();
    }

    bool VulkanSwapChain::createSwapChian() {

        auto Surface = Surfaceviewport->getVkSurface();
        uint32_t formatCount = 0;
        std::vector<VkSurfaceFormatKHR> vkSurfaceFormats;
        vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice->getPhysicalDevice(), Surface, &formatCount, nullptr);
        vkSurfaceFormats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice->getPhysicalDevice(), Surface, &formatCount, vkSurfaceFormats.data());

        VkSurfaceCapabilitiesKHR vkSurfaceCapabilitiesKHR;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice->getPhysicalDevice(), Surface, &vkSurfaceCapabilitiesKHR);

        VkExtent2D actualExtent;
        if (vkSurfaceCapabilitiesKHR.currentExtent.width != UINT32_MAX) {
            actualExtent = vkSurfaceCapabilitiesKHR.currentExtent;
        } else {
            actualExtent.width = std::clamp(Surfaceviewport->getViewportWidth(),
                vkSurfaceCapabilitiesKHR.minImageExtent.width,
                vkSurfaceCapabilitiesKHR.maxImageExtent.width);
            actualExtent.height = std::clamp(Surfaceviewport->getViewportHeight(),
                vkSurfaceCapabilitiesKHR.minImageExtent.height,
                vkSurfaceCapabilitiesKHR.maxImageExtent.height);
        }

        Info("Actual viewport size: {} x {}", actualExtent.height, actualExtent.width);

        VkSurfaceFormatKHR choiceFormat;
        for (auto& F : vkSurfaceFormats) if (F.format == (VkFormat)Surfaceviewport->getVulkanColorFormat() && F.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
            choiceFormat = F;
        }
        Surfaceviewport->setViewportResize(actualExtent.width, actualExtent.height);

        uint32_t QuefamilyIndex[] = { mDevice->getGraphicQueue()->getFamilyIndex(), PresentQueue->getFamilyIndex() };
        bool isCxclusive = (QuefamilyIndex[0] == QuefamilyIndex[1]);
        Debug("Cxclusive Mode : {}",(isCxclusive ? "Yes" : "No"));

        auto oldSwapChain = mData->swapchain;
        uint32_t LstMaxCount = MaxSwapChianFramCount;
        VkSwapchainCreateInfoKHR swapChainInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = Surface,
            .minImageCount = vkSurfaceCapabilitiesKHR.minImageCount + 1,
            .imageFormat = choiceFormat.format,
            .imageColorSpace = choiceFormat.colorSpace,
            .imageExtent = {actualExtent.width, actualExtent.height},
            .imageArrayLayers = vkSurfaceCapabilitiesKHR.maxImageArrayLayers,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = isCxclusive ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
            .queueFamilyIndexCount = 2,
            .pQueueFamilyIndices = QuefamilyIndex,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_MAILBOX_KHR,
            .oldSwapchain = oldSwapChain,
        };

        if (vkCreateSwapchainKHR(mDevice->getLogicalDevice(), &swapChainInfo, nullptr, &mData->swapchain) != VK_SUCCESS) {
            Error("SwapChain Creat Failed");
            return false;
        }

        vkGetSwapchainImagesKHR(mDevice->getLogicalDevice(), mData->swapchain, &MaxSwapChianFramCount, nullptr);
        vkGetSwapchainImagesKHR(mDevice->getLogicalDevice(), mData->swapchain, &MaxSwapChianFramCount, mData->swapChainImageHandles);
        Debug("Max Image Cout Can Swapchian Use {}", MaxSwapChianFramCount);

        for (uint32_t i = 0; i < LstMaxCount; i++) {
            delete SwapChainFrameBuffers[i];
            SwapChainFrameBuffers[i] = nullptr;
            delete SwapChainTextures[i];
            SwapChainTextures[i] = nullptr;
        }
        if (oldSwapChain) {
            vkDestroySwapchainKHR(mDevice->getLogicalDevice(), oldSwapChain, nullptr);
        }

        for (uint32_t i = 0; i < SWAPCHAIN_SLOT_COUNT; i++) {
            auto& [avaliable, renderFinish, finishFence, index] = SwapChainFrameInfos[i];
            if (!avaliable)   avaliable   = usingRHI->RHICreateSemaphore("SwapAvailableSemphore");
            if (!renderFinish) renderFinish = usingRHI->RHICreateSemaphore("SlotFinishSemphore");
            if (!finishFence) finishFence = usingRHI->RHICreateFence(false, "FinishFence");
            index = UINT32_MAX;
        }

        // Per-image semaphores & present fences
        for (uint32_t i = 0; i < MaxSwapChianFramCount; i++) {
            if (!ImageRenderFinish[i])
                ImageRenderFinish[i] = usingRHI->RHICreateSemaphore("ImageFinishSemphore");
            // VK_KHR_swapchain_maintenance1: signaled when image leaves display engine.
            // Start signaled so the first acquire on each image passes.
            if (mDevice->isSwapchainMaintenance1Supported() && !PresentFence[i])
                PresentFence[i] = usingRHI->RHICreateFence(true, "PresentFence");
        }

        for (uint32_t i = 0; i < MaxSwapChianFramCount; i++) {
            SwapChainTextures[i] = new VulkanTexture(mDevice,
                mData->swapChainImageHandles[i],
                Surfaceviewport->getVulkanColorFormat(),
                {actualExtent.width, actualExtent.height, 1},
                1);
            SwapChainFrameBuffers[i] = new VulkanFrameBuffer(
                mDevice, { SwapChainTextures[i] },
                actualExtent.width,
                actualExtent.height,
                (RHIRenderPass*)VulkanSwapChainRednerPass
               );
        }

        needReBuildSwapChain.store(0, std::memory_order_release);
        return true;
    }

    bool VulkanSwapChain::createPipelineandRenderPass() {
    	ColorEntry entry {
			.EntryPros = {
				.loadOp = RenderTargetLoadAction::Clear,
				.storeOp = RenderTargetStoreAction::Store,
				.initLayout = TextureLayout::Undefined,
				.dstLayout = TextureLayout::Present,
				.colorType = TextureCOLORType::RGBA_8,
				.sampleCount = 0,
			}
		};

		SubPassInfo subpass {
			.ColorEntryMask = 1<<0,
			.UseDepthStencil = false,
			.ReadDepthAsInput = false,
		};

		RHIRenderPassInfo renderpassinfo({{0, entry}}, {.exeit = 0}, {subpass});
		VulkanSwapChainRednerPass = static_cast<VulkanRenderPass*>(usingRHI->RHICreateRenderPass(renderpassinfo));
        if (!VulkanSwapChainRednerPass) {
            Error("SwapChain : 0x{:x} RenderPass Create Failed", (size_t)VulkanSwapChainRednerPass);
            return false;
        }

		RHIPipelineDescribeInfo desinfo ({
			{0, 1, RHIDescriptorTyp::SamplerImage, FragmentShaderStage},    // b0 t0 g_OffscreenTexture
			{1, 1, RHIDescriptorTyp::RBuffer, FragmentShaderStage},        // b1 t1 g_FrameBuffer
			{2, 1, RHIDescriptorTyp::Sampler, FragmentShaderStage},        // b2 s2 g_LinearSampler
			{3, 1, RHIDescriptorTyp::UniformBuffer, FragmentShaderStage}   // b3 b3 BufferToOutPutData
		});
		RHIPipelineState pipelineState {
			.describeInfo = desinfo,
			.topologyType = TopologyType::Triangle,
			.multiSampleState = { .SamplerBit = 1 },
			.depthStencilState = { .DepthTestEnable = false, .DepthWriteEnable = false, .StencilTestEnable = false },
            .colorblendState = {
                .ColorBlenEnable = false,
                .UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_ | FISIR::_A_PASS)
            },
			.renderpass = VulkanSwapChainRednerPass,
		};
		pipelineState.Shaders[__VERTEXSHADER__] = VShader;
		pipelineState.Shaders[__FRAGMENTSHADER__] = FShader;

		VulkanViewportPipeline = static_cast<VulkanPipeline*>(usingRHI->RHICreatePipeline(pipelineState));
        if (!VulkanViewportPipeline) {
            Error("SwapChain : 0x{:x} Pipeline Create Failed", (size_t)VulkanSwapChainRednerPass);
            return false;
        }

        return true;
    }
}
