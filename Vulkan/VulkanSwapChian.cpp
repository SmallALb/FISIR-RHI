#include "VulkanSwapChian.h"

#include <algorithm>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "../RHIShader.h"
#include "../ShaderComplier.h"
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
		SamplerState g_LinearSampler : register(s1);

		struct PSInput {
			float4 position : SV_POSITION;
			float2 uv : TEXCOORD0;
		};

		float4 main(PSInput input) : SV_Target {
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
            ShaderComplier vsCompiler, psCompiler;
            vsCompiler.compileShader(FullscreenVS, wcslen(FullscreenVS) * sizeof(wchar_t), L"main", L"vs_6_0");
            psCompiler.compileShader(FullscreenPS, wcslen(FullscreenPS) * sizeof(wchar_t), L"main", L"ps_6_0");
            VShader = usingRHI->RHICreateShader(ShaderTYP::__VERTEXSHADER__, vsCompiler.getShaderData(), vsCompiler.getShaderDataSize());
            FShader = usingRHI->RHICreateShader(ShaderTYP::__FRAGMENTSHADER__, psCompiler.getShaderData(), psCompiler.getShaderDataSize());
        }
        
        if (!PresentQueue) PresentQueue = new VulkanQueue(mDevice, mPresentQueFamilyIndex, "SwapChainPresentQue");

        
        return createPipelineandRenderPass() && createSwapChian();
    }

    VulkanSwapChain::~VulkanSwapChain() {
        for (uint32_t i = 0; i < MaxSwapChianFramCount; i++)  {
            delete SwapChainTextures[i]; 
            delete SwapChainFrameBuffers[i];
            
        }
        if (mData->swapchain) vkDestroySwapchainKHR(mDevice->getLogicalDevice(), mData->swapchain, nullptr);
        delete PresentQueue;
    }


    uint32_t VulkanSwapChain::acquireGetImageInfoID() {
        
        
        if (!mData->swapchain || needReBuildSwapChain.load(std::memory_order_acquire)) {
            if (!recreateSwapChain()) return RHISwapChain::FAILEID;
        }
        
        uint32_t frameidx = RHISwapChain::FAILEID;
        for (uint32_t i = 0; i < MaxSwapChianFramCount; i++) if (!SwapChainFrameInfos[i].inUse) {
            frameidx = i;
            break;
        }

        if (frameidx == UINT32_MAX) {
            Error("No Frames Can Use!");
            return RHISwapChain::FAILEID;
        }

        auto& [available, finish, index, inUse] = SwapChainFrameInfos[frameidx];


        auto start = std::chrono::steady_clock::now();
        //if (fence->getFenceStage() != RHIFence::Statue::Signaled) {
        //    Debug("acquire frame fence not signaled");
        //    return RHISwapChain::FAILEID;
        //}
        
        auto end = std::chrono::steady_clock::now();
        Debug("Acquire Frame Fence Duration: {}", std::chrono::duration<double>(end - start).count() * 1e9);

        auto res = vkAcquireNextImageKHR(
            mDevice->getLogicalDevice(), 
            mData->swapchain,
            (uint64_t)(2*1e9),
            static_cast<VkSemaphore>(available->getSemaphoreHandle()),
            VK_NULL_HANDLE,
            &index
        );


        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
            Error("SwapChain Out Of Data!");
            needReBuildSwapChain.store(1,std::memory_order_release);
            return RHISwapChain::FAILEID;
        }


        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            Error("Acquire next image failed: {}", (uint32_t)res);
            return RHISwapChain::FAILEID;
        }

        //fence->reset();
        inUse = true;
        return frameidx;
    }

    void VulkanSwapChain::present(uint32_t infoid) {
        if (infoid >= MaxSwapChianFramCount) return;

        auto& [available, finish,  imageindex, inUse] = SwapChainFrameInfos[infoid];

        if (!PresentQueue || !mData->swapchain) {
            Error("PresentQueue is empty or swapchainHandle not exits!");
            return;
        }

        VkSemaphore Vksem = static_cast<VkSemaphore>(finish->getSemaphoreHandle());
        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &Vksem,
            .swapchainCount = 1,
            .pSwapchains = &mData->swapchain,
            .pImageIndices = &imageindex,
            .pResults = nullptr
        };

        inUse = false;

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
        if (id < MaxSwapChianFramCount) return SwapChainFrameInfos[id];
        return {};
    }

    RHIFrameBuffer* VulkanSwapChain::getSwapChainFrameBuffer(uint32_t imageindex) {
        if (imageindex < MaxSwapChianFramCount) return SwapChainFrameBuffers[imageindex];
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
        //getCapabilities
        uint32_t formatCount = 0;
        std::vector<VkSurfaceFormatKHR> vkSurfaceFormats;
        vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice->getPhysicalDevice(), Surface, &formatCount, nullptr);
        vkSurfaceFormats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice->getPhysicalDevice(), Surface, &formatCount, vkSurfaceFormats.data());

        VkSurfaceCapabilitiesKHR vkSurfaceCapabilitiesKHR;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice->getPhysicalDevice(), Surface, &vkSurfaceCapabilitiesKHR);

        //Get Size
        VkExtent2D actualExtent;
        if (vkSurfaceCapabilitiesKHR.currentExtent.width != UINT32_MAX) {
            actualExtent = vkSurfaceCapabilitiesKHR.currentExtent;
        }

        else {
            actualExtent.width = std::clamp(Surfaceviewport->getViewportWidth(),
                vkSurfaceCapabilitiesKHR.minImageExtent.width,
                vkSurfaceCapabilitiesKHR.maxImageExtent.width);
            actualExtent.height = std::clamp(Surfaceviewport->getViewportHeight(),
                vkSurfaceCapabilitiesKHR.minImageExtent.height,
                vkSurfaceCapabilitiesKHR.maxImageExtent.height);
        }

        Info("Actual viewport size: {} x {}", actualExtent.height, actualExtent.width);

        //CreatSwapChain
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
            .minImageCount = vkSurfaceCapabilitiesKHR.minImageCount,
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

        //Create Texture and Frame
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

        for (uint32_t i = 0; i < MaxSwapChianFramCount; i++) {
            auto& [available, finish, index, inUsed] = SwapChainFrameInfos[i];
            if (!available) available = usingRHI->RHICreateSemaphore("SwapAvailableSemphore");
            if (!finish) finish = usingRHI->RHICreateSemaphore("FinishSemphore");
            inUsed = false;
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
	//Create Pipeline
		
		RHIPipelineDescribeInfo desinfo ({
			{0, 1, RHIDescriptorTyp::SamplerImage, FragmentShaderStage},  // 纹理 binding 0
			{1, 1, RHIDescriptorTyp::Sampler, FragmentShaderStage}        // 采样器 binding 1
		});
		RHIPipelineState pipelineState {
			.describeInfo = desinfo,
			.topologyType = TopologyType::Triangle,
			.multiSampleState = {
				.SamplerBit = 1
			},
			.depthStencilState = {
				.DepthTestEnable = false,
				.DepthWriteEnable = false,
				.StencilTestEnable = false,
			},
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
