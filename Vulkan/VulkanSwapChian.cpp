#include "VulkanSwapChian.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
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
		[[vk::binding(0, 0)]] Texture2D<float4> g_OffscreenTexture  : register(t0);
		[[vk::binding(1, 0)]] ByteAddressBuffer g_FrameBuffer  : register(t1);


		[[vk::binding(2, 0)]] SamplerState g_LinearSampler  : register(s2);

		[[vk::binding(3, 0)]] cbuffer BufferToOutPutData  : register(b3) {
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

    // 呈现通道着色器是懒建的进程级静态，**必须在设备销毁时一起复位**。
    // 否则「窗口被回收后重建 RHI」时（Android 切后台/锁屏回来）这里仍然返回 true 分支、
    // 把上一代已被销毁的 RHIShader* 塞进管线状态 ⇒ VulkanPipeline 构造里解引用野指针
    // （实测 fault addr 0x20，栈是 VulkanSwapChain → VulkanRHI → VulkanPipeline）。
    void VulkanSwapChain::ResetPresentShaders() {
        VShader = nullptr;
        FShader = nullptr;
    }

    struct __VkSwapChainData {
        VkSwapchainKHR swapchain {VK_NULL_HANDLE};
        std::vector<VkImage> swapChainImageHandles;   // 大小 = 交换链实际图像数
    };

    // 呈现模式选择：sync 请求 FIFO（垂直同步）；否则优先无同步的 MAILBOX，其次 IMMEDIATE，
    // 两者都不支持时退回 FIFO（规范保证 FIFO 一定可用）。
    static VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, bool sync) {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        if (count) vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, modes.data());
        auto has = [&modes](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };

        if (sync) return VK_PRESENT_MODE_FIFO_KHR;
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))   return VK_PRESENT_MODE_MAILBOX_KHR;
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    static const char* PresentModeName(VkPresentModeKHR mode) {
        switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO(vsync)";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "UNKNOWN";
        }
    }


    VulkanSwapChain::VulkanSwapChain(VulkanViewport* viewport,  uint32_t QueFamilyIndex, uint32_t slotCount) {
        mData = new __VkSwapChainData();
        Surfaceviewport = viewport;
        mPresentQueFamilyIndex = QueFamilyIndex;
        // 请求值先记下；有效槽位数（夹取到 ≤ 图像数）在 createSwapChian() 里算出。
        RequestedSlotCount = slotCount ? slotCount : DEFAULT_SWAPCHAIN_SLOT_COUNT;
        SlotCount = RequestedSlotCount;
        SwapChainFrameInfos.resize(SlotCount);
    }

    void VulkanSwapChain::sync(bool enable) {
        if (SyncEnabled.load(std::memory_order_acquire) == enable) return;
        SyncEnabled.store(enable, std::memory_order_release);
        // 呈现模式在 vkCreateSwapchainKHR 时固定、无法原地修改：这里只登记目标模式并请求重建，
        // 真正的重建发生在下一次 acquireGetImageInfoID()（重建前 vkDeviceWaitIdle）。
        needReBuildSwapChain.store(1, std::memory_order_release);
        Info("SwapChain sync({})：Present Mode {}",
            enable, enable ? "FIFO" : "MAILBOX/IMMEDIATE");
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

            // 纹理创建出来是 VK_IMAGE_LAYOUT_UNDEFINED，而呈现 PS 会把它当采样图像用
            // （描述符里声明的 layout 是 SHADER_READ_ONLY_OPTIMAL）—— 采样一张 UNDEFINED 布局的
            // 图像是未定义行为。经典 DescriptorSet 路径下验证层会直接报
            //   expects VkImage ... SHADER_READ_ONLY_OPTIMAL -- instead, current layout is UNDEFINED
            // （描述符堆路径因为校验层还没跟踪该扩展的布局，反而看不出来）。
            // 这里立刻记一次布局转换：与字体图集上传同一套写法，用的是**渲染队列**命令列表，
            // 因此按「提交顺序 == 录制顺序」的不变量，它必然排在第一次呈现之前 —— 无需等 fence。
            if (FallbackTexture) {
                FISIR::RHITexture* textures[] = { FallbackTexture };
                FISIR::RHIRenderCommandList transitionList(usingRHI);
                transitionList.TransitionTextures(textures, 1,
                    FISIR::ResourceAccess::Undefined, FISIR::ResourceAccess::ShaderReadOnly,
                    FISIR::TextureLayout::Undefined, FISIR::TextureLayout::ShaderReadOnlyOptimal,
                    FISIR::RHIUsingStage::NoneStage, FISIR::RHIUsingStage::FragmentShaderStage);
                transitionList.End(nullptr);
            }
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
        // 先注销：RHI 线程每轮会轮询注册表，绝不能让它再碰正在析构的交换链。
        VulkanRHI::UnregisterSwapChain(this);
        if (PresentResourcePack.ResourcePack || PresentResourcePack.SamplerPack) {
            if (usingRHI) usingRHI->RHIDestroyResourcePack(PresentResourcePack);
        }
        if (FallbackTexture) usingRHI->RHIDestroyTexture(FallbackTexture);
        if (FallbackSampler) usingRHI->RHIDestroySampler(FallbackSampler);
        if (FallbackBuffer)  usingRHI->RHIDestroyBuffer(FallbackBuffer);
        for (auto* tex : SwapChainTextures)    delete tex;
        for (auto* fb  : SwapChainFrameBuffers) delete fb;
        if (mData->swapchain) vkDestroySwapchainKHR(mDevice->getLogicalDevice(), mData->swapchain, nullptr);
        delete BufferToOutPutData;
        delete PresentQueue;
        delete mData;
    }

    uint32_t VulkanSwapChain::acquireGetImageInfoID() {
        // 上一次请求已经完成（上一次兜底跳过时结果留到了现在）：先把结果取走。
        // 少了这一步，mAcquire 会永远停在 2 —— 交换链即彻底卡死。
        if (mAcquire.load(std::memory_order_acquire) == 2) {
            mAcquire.store(0, std::memory_order_release);
            return mAcquiredFrameID;
        }
        if (mAcquire.exchange(1, std::memory_order_acq_rel) != 0)
            return RHISwapChain::FAILEID;   // 上一次请求还没被 RHI 处理完

        // 兜底时刻交给 RHI 侧判（原子等待没有超时版本）：到点仍未取到图，RHI 直接回 FAILEID。
        const uint64_t nowNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        mAcquireDeadlineNs.store(nowNs + kAcquireGiveUpNs, std::memory_order_relaxed);

        // 原子等待：无锁、不烧 CPU，值离开 1 即醒（RHI 的 publishAcquire 里 notify_one）。
        mAcquire.wait(1, std::memory_order_acquire);

        if (mAcquire.load(std::memory_order_acquire) != 2) return RHISwapChain::FAILEID;
        mAcquire.store(0, std::memory_order_release);
        return mAcquiredFrameID;
    }

    // RHI 线程专用：置取图结果并唤醒主线程。
    void VulkanSwapChain::publishAcquire(uint32_t frameID) {
        mAcquiredFrameID = frameID;
        mAcquire.store(2, std::memory_order_release);
        mAcquire.notify_one();
    }

    // ── 取图：RHI 线程每轮轮询（非阻塞，绝不长时间等待）──────────────────
    void VulkanSwapChain::tryAcquire() {
        if (mAcquire.load(std::memory_order_acquire) != 1) return;   // 没有待处理的请求

        // 兜底：请求已经被轮询到超过兜底时刻仍未取到图（图一直没空出来、最小化窗口、
        // 驱动持续 VK_TIMEOUT……）。回一个 FAILEID 让主线程继续跑，而不是一直睡在原子等待上。
        const uint64_t nowNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowNs >= mAcquireDeadlineNs.load(std::memory_order_relaxed)) {
            static std::atomic<int> sGiveUps{ 0 };
            if (sGiveUps.fetch_add(1, std::memory_order_relaxed) < 3)
                Debug("tryAcquire: image not available within give-up window, returning FAILEID for this frame");
            publishAcquire(RHISwapChain::FAILEID);
            return;
        }

        if (SlotCount == 0) SlotCount = 1;      // 防御：有效槽位数恒 ≥ 1
        CurrentFrameID %= SlotCount;

        // 交换链不存在 / 被请求重建（sync()）或上次报 OUT_OF_DATE：重建只能发生在本线程
        //（vkDeviceWaitIdle + vkDestroy/CreateSwapchainKHR 必须与队列操作同线程）。
        // 重建失败就留着请求，下一轮再试。
        if (!mData->swapchain || needReBuildSwapChain.load(std::memory_order_acquire)) {
            const bool rebuilt = recreateSwapChain();
            if (!rebuilt) return;
            CurrentFrameID = 0;
        }

        auto& slot = SwapChainFrameInfos[CurrentFrameID];   // 重建会整表换新，引用必须在其后取

        // 槽复用前必须确认该槽上一轮的提交**已经发出**：围栏由本线程的 doSubmit 置
        // setSubmited，若那一页还在队列里没提交，vkWaitForFences 会永远等下去
        //（而唯一能提交它的也是本线程）—— 本轮先不取，下一轮再来。
        if (slot.imageIndex != UINT32_MAX && !slot.finishFence->isSubmited()) {
            // 上一轮取图后上层没有渲染/提交（例如拿到图就 return 了）：该槽的 acquire
            // 信号量还有未消费的 signal，既不能重新 signal、也不能等一个永不置位的围栏。
            // 把同一张图再交给上层一次，让这一轮正常渲染+提交，槽位自愈。
            publishAcquire(CurrentFrameID);
            return;
        }

        if (slot.imageIndex != UINT32_MAX && !slot.finishFence->isSignaled()) return;

        uint32_t imageIndex = slot.imageIndex;
        const VkResult res = vkAcquireNextImageKHR(
            mDevice->getLogicalDevice(),
            mData->swapchain,
            0,                                        // 非阻塞：图还没空出来就下轮再来
            static_cast<VkSemaphore>(slot.avaliable->getSemaphoreHandle()),
            VK_NULL_HANDLE,
            &imageIndex
        );
        if (res == VK_TIMEOUT || res == VK_NOT_READY) return;   // 保持请求，下一轮继续轮询

        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
            Error("SwapChain Out Of Data!");
            needReBuildSwapChain.store(1, std::memory_order_release);
            publishAcquire(RHISwapChain::FAILEID);      // 释放主线程（本帧跳过）
            return;
        }
        if (res != VK_SUCCESS) {
            //Error("Acquire next image failed: {}", (uint32_t)res);
            publishAcquire(RHISwapChain::FAILEID);
            return;
        }

        // VK_KHR_swapchain_maintenance1: ensure the image has left the
        // display engine before we start rendering to it again.
        if (mDevice->isSwapchainMaintenance1Supported() && PresentFence[imageIndex]) {
            PresentFence[imageIndex]->wait();
        }

        slot.imageIndex  = imageIndex;
        slot.finishFence->reset();
        const uint32_t frameID = CurrentFrameID;
        CurrentFrameID   = (CurrentFrameID + 1) % SlotCount;
        publishAcquire(frameID);                        // 释放主线程
    }

    void VulkanSwapChain::presentNow(uint32_t infoid) {
        if (infoid >= SlotCount) return;

        auto& slot = SwapChainFrameInfos[infoid];
        if (slot.imageIndex >= ImageRenderFinish.size()) {
            Error("SwapChain presentNow: imageIndex {} out of range (image count {})", slot.imageIndex, ImageRenderFinish.size());
            return;
        }

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
        if (imageindex < SwapChainTextures.size()) return SwapChainTextures[imageindex];
        else return nullptr;
    }

    uint32_t VulkanSwapChain::getImageCount() const {
        return MaxSwapChianFramCount;
    }

    SwapChainGetImageInfo VulkanSwapChain::getSwapChainGetImageInfo(uint32_t id) {
        if (id >= SlotCount) return {};
        auto& slot = SwapChainFrameInfos[id];
        SwapChainGetImageInfo info = slot;
        // Override renderFinish with the per-image present semaphore so
        // presentNow() waits on the correct semaphore per swapchain image.
        if (slot.imageIndex < ImageRenderFinish.size()) info.renderFinish = ImageRenderFinish[slot.imageIndex];
        return info;
    }

    RHIFrameBuffer* VulkanSwapChain::getSwapChainFrameBuffer(uint32_t imageindex) {
        if (imageindex < SwapChainFrameBuffers.size()) return SwapChainFrameBuffers[imageindex];
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

        // 选表面格式：优先「viewport 要的格式 + SRGB_NONLINEAR」；找不到就退回列表第一个。
        // 原实现是「循环里命中才赋值」，一个都没命中时 choiceFormat 是**未初始化**的，
        // 直接喂给 vkCreateSwapchainKHR —— 桌面驱动恰好总有得选才没暴露，Android 上
        // 各家 surface 的格式/色彩空间组合不一样（B8G8R8A8 / R8G8B8A8，色彩空间也可能不同），
        // 所以这里必须有个确定性的兜底。
        VkSurfaceFormatKHR choiceFormat{};
        if (!vkSurfaceFormats.empty()) {
            choiceFormat = vkSurfaceFormats[0];
            for (auto& F : vkSurfaceFormats) {
                if (F.format == (VkFormat)Surfaceviewport->getVulkanColorFormat() &&
                    F.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
                    choiceFormat = F;
                }
            }
            if (choiceFormat.format != (VkFormat)Surfaceviewport->getVulkanColorFormat()) {
                Warn("Surface 不提供想要的格式 {}，退用 {} / {}",
                     (uint32_t)Surfaceviewport->getVulkanColorFormat(),
                     (uint32_t)choiceFormat.format, (uint32_t)choiceFormat.colorSpace);
            }
        }
        else {
            Error("Surface 没有任何可用格式，交换链无法创建");
        }
        Surfaceviewport->setViewportResize(actualExtent.width, actualExtent.height);

        uint32_t QuefamilyIndex[] = { mDevice->getGraphicQueue()->getFamilyIndex(), PresentQueue->getFamilyIndex() };
        bool isCxclusive = (QuefamilyIndex[0] == QuefamilyIndex[1]);
        Debug("Cxclusive Mode : {}",(isCxclusive ? "Yes" : "No"));

        // 图像数：请求 minImageCount+1（避免贴驱动下界），并受 caps 上限约束。
        // 每图像资源按「实际图像数」分配（见下），因此这里不再假设固定 3 张。
        uint32_t desiredImageCount = vkSurfaceCapabilitiesKHR.minImageCount + 1;
        if (vkSurfaceCapabilitiesKHR.maxImageCount > 0 && desiredImageCount > vkSurfaceCapabilitiesKHR.maxImageCount)
            desiredImageCount = vkSurfaceCapabilitiesKHR.maxImageCount;

        // 呈现模式：sync(true) → FIFO，否则优先 MAILBOX / IMMEDIATE。
        const VkPresentModeKHR presentMode = ChoosePresentMode(mDevice->getPhysicalDevice(), Surface, SyncEnabled.load(std::memory_order_acquire));

        auto oldSwapChain = mData->swapchain;
        VkSwapchainCreateInfoKHR swapChainInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = Surface,
            .minImageCount = desiredImageCount,
            .imageFormat = choiceFormat.format,
            .imageColorSpace = choiceFormat.colorSpace,
            .imageExtent = {actualExtent.width, actualExtent.height},
            // 普通窗口表面就是单层 2D：maxImageArrayLayers 是**上限**不是应当请求的值
            // （某些平台会给 >1，直接用它就会建出分层交换链，与后面的 framebuffer 假设不符）。
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = isCxclusive ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
            .queueFamilyIndexCount = 2,
            .pQueueFamilyIndices = QuefamilyIndex,
            // 必须用 surface 当前的变换（Android 上横屏应用可能拿到 ROTATE_90/270；
            // 写死 IDENTITY 在只支持旋转的设备上会直接创建失败）。
            .preTransform = vkSurfaceCapabilitiesKHR.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = presentMode,
            .oldSwapchain = oldSwapChain,
        };

        if (vkCreateSwapchainKHR(mDevice->getLogicalDevice(), &swapChainInfo, nullptr, &mData->swapchain) != VK_SUCCESS) {
            Error("SwapChain Creat Failed");
            return false;
        }

        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(mDevice->getLogicalDevice(), mData->swapchain, &imageCount, nullptr);
        MaxSwapChianFramCount = imageCount;
        mData->swapChainImageHandles.assign(imageCount, VK_NULL_HANDLE);
        vkGetSwapchainImagesKHR(mDevice->getLogicalDevice(), mData->swapchain, &imageCount, mData->swapChainImageHandles.data());
        Debug("SwapChain images: {}", imageCount);

        // 槽位数：夹取到 [1, 图像数]。槽复用周期必须不短于图像复用周期，否则同一
        // 每图像 present 信号量会在上一帧 present 尚未消费时被再次 signal（信号量复用竞态）。
        const uint32_t requested = RequestedSlotCount ? RequestedSlotCount : DEFAULT_SWAPCHAIN_SLOT_COUNT;
        SlotCount = std::clamp(requested, 1u, imageCount ? imageCount : 1u);
        if (SlotCount != requested) {
            Warn("SwapChain requested {} slots, clamped to {} (slots must not exceed image count {})", requested, SlotCount, imageCount);
        }

        // 释放上一轮每图像资源（图像数可能变化），再按新图像数重建容器。
        for (auto*& fb : SwapChainFrameBuffers)  { delete fb;  fb = nullptr; }
        for (auto*& tex : SwapChainTextures)     { delete tex; tex = nullptr; }
        SwapChainFrameBuffers.assign(imageCount, nullptr);
        SwapChainTextures.assign(imageCount, nullptr);
        // 信号量/围栏尽量复用已有对象（图像数不变时不会增长）。
        ImageRenderFinish.resize(imageCount, nullptr);
        PresentFence.resize(imageCount, nullptr);

        if (oldSwapChain) {
            vkDestroySwapchainKHR(mDevice->getLogicalDevice(), oldSwapChain, nullptr);
        }

        SwapChainFrameInfos.resize(SlotCount);
        for (uint32_t i = 0; i < SlotCount; i++) {
            auto& [avaliable, renderFinish, finishFence, index] = SwapChainFrameInfos[i];
            if (!avaliable) {
                avaliable   = usingRHI->RHICreateSemaphore("SwapAvailableSemphore");
                // acquire 信号量由 vkAcquireNextImageKHR（驱动）signal，非任何命令缓冲。
                // 标记为外部信号，提交循环把等待它视为已满足，不作为跨 CB 依赖。
                avaliable->setExternalSignal(true);
            }
            if (!renderFinish) renderFinish = usingRHI->RHICreateSemaphore("SlotFinishSemphore");
            if (!finishFence) finishFence = usingRHI->RHICreateFence(false, "FinishFence");
            index = UINT32_MAX;
        }

        Info("SwapChain: {} images, {} slots, present mode = {}", imageCount, SlotCount, PresentModeName(presentMode));



        // Per-image semaphores & present fences
        for (uint32_t i = 0; i < imageCount; i++) {
            if (!ImageRenderFinish[i])
                ImageRenderFinish[i] = usingRHI->RHICreateSemaphore("ImageFinishSemphore");
            // VK_KHR_swapchain_maintenance1: signaled when image leaves display engine.
            // Start signaled so the first acquire on each image passes.
            if (mDevice->isSwapchainMaintenance1Supported() && !PresentFence[i])
                PresentFence[i] = usingRHI->RHICreateFence(true, "PresentFence");
        }

        for (uint32_t i = 0; i < imageCount; i++) {
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
