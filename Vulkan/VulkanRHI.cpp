#define VK_USE_PLATFORM_WIN32_KHR

#include "VulkanRHI.h"
#include <vector>
#include <cstdio>
#include <algorithm>
#include <string>
#include "../../Log/Logger.h"
#include "../LockFreeQue.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanShader.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanRenderPass.h"
#include "VulkanFrameBuffer.h"
#include "VulkanViewport.h"
#include "VulkanSwapChian.h"
#include "../RHICommandList.h"
#include "VulkanSampler.h"
#include "ChangeImageFlagsToVulkanFlags.h"
namespace FISIR {

    /*

        CommandPool Manager and Thread Context

    */
    //Pending Release Command Buffers
    struct PendingReleaseInfo {
        PendingReleaseInfo() {}
        PendingReleaseInfo(VulkanFence* f, std::vector<CBInfo>&& c, bool infence = 0) :
            fence(f),
            cbInfos(std::move(c)),
            inputFence(infence)
        {}
        PendingReleaseInfo(VulkanFence* f, const std::vector<CBInfo>& c) : fence(f), cbInfos(c) {}


        PendingReleaseInfo(PendingReleaseInfo&& other) noexcept
            : fence(other.fence), cbInfos(std::move(other.cbInfos)), inputFence(other.inputFence) {
            other.fence = nullptr;
        }

        PendingReleaseInfo& operator=(PendingReleaseInfo&& other) noexcept {
            if (this != &other) {
                fence = other.fence;
                cbInfos = std::move(other.cbInfos);
                inputFence = other.inputFence;
                other.fence = nullptr;
            }
            return *this;
        }

        VulkanFence* fence{ nullptr };
        std::vector<CBInfo> cbInfos{};
        bool inputFence{ 0 };
    };


    static LockFreeQue<PendingReleaseInfo> PendingReleaseCBs;
    static LockFreeQue<RingCommandPool::Page*> NeedUsingPages;

    static std::vector<PendingReleaseInfo> PendingReleaseCBsInThread;

    //Pending Upload Command Buffers
    std::thread RHIThread, RHIResourceThread, PrepareThread;



    /*
        ViewPort SwapChain Cache

    */
    std::vector<VulkanViewport*> ViewPortsPool;

    static std::unordered_map<RHIViewport*, VulkanSwapChain*> ViewPortSwapChainCache;

    static std::vector<VulkanShader*> ShadersPool;

    /*
    *
        Pipeline Layout Cache

    */
#include "VulkanPipelineLayoutCache.h"


    /*

        Vulkan Instance and Device Initialization

    */
#include "VulkanInstance.h" 



    /*

        Vulkan Render Pass Cache

    */

#include "VulkanRenderPassCache.h"

    VulkanRHI::VulkanRHI() {
        if (!MakeVkInstance()) return;
        Info("vulkan Instance Create Success!");
    }

    /*
      __RHI__
  */
    VulkanRHI::~VulkanRHI() {
        Debug("Destroy Vulkan RHI!");

        stopTag = 1;

        PrepareThread.join();
        Debug("Prepare Thread Join");

        RHIThread.join();
        Debug("VulkanRHI Thread Join");

        RHIResourceThread.join();
        Debug("VulkanResource Thread Join");



        vkDeviceWaitIdle(mDevice->getLogicalDevice());

        PendingReleaseCBs.forceClear();
        NeedUsingPages.forceClear();
        PendingReleaseCBsInThread.clear();
        for (auto& pool : CmdMemoryPool) {
            for (auto& page : pool.Pages) {
                page.BatchQueue.forceClear();
            }
            pool.FreePages.forceClear();
        }

        delete ThreadPool;
        Debug("Vulkan Thread Pool Join");

        for (auto& [viewport, swapchain] : ViewPortSwapChainCache) {
            delete viewport;
            delete swapchain;
        }

        for (auto& [name, pipeline] : PipelineCacheMap) delete pipeline;

        for (auto& [info, renderPass] : RenderPassCache) delete renderPass;

        for (auto& shader : ShadersPool) delete shader;

        Debug("Destroy Fence and Semaphore Pool");
        mFencePool->destroyPool();
        delete mFencePool;
        delete mSemaphorePool;
        delete mDescriptorPool;
        delete mCmdPoolManager;

        Debug("Destroy Device");
        mDevice->Destory();
        Debug("Vulkan RHI Destroyed!");

        DestroyDebugReportCallback();
        Debug("Vulkan RHI DebugCall Destroyed!");

    }

    bool VulkanRHI::Init() {
        if (!gInstance) return false;
        if (!MakeDebugReportCallback()) return false;
        mDevice = new VulkanDevice(SelectDevice(gInstance));

        if (ViewPortsPool.empty()) Warn("The ViewPorts Are Empty!");
        if (!mDevice->Init(ViewPortsPool, ViewPortSwapChainCache)) {
            Error("Failed to initialize Vulkan Device!");
            return false;
        }

        mFencePool = new VulkanFencePool(mDevice);
        mSemaphorePool = new VulkanSemaphorePool(mDevice);
        mDescriptorPool = new VulkanDescriptorPool(mDevice);

        mCmdPoolManager = new VulkanCommandPoolManager(mDevice);

        ThreadPool = new CommandExecuteThreadPool(mDevice, mCmdPoolManager);

        for (int i = 0; i < 3; i++) {
            CmdMemoryPool[i].cmdType = static_cast<CmdType>(i + 1);
        }

        if (ViewPortSwapChainCache.empty()) Error("ViewPortSwapChainCache is empty!");

        std::vector<RHIViewport*> neederase;
        for (auto& [viewport, swapchain] : ViewPortSwapChainCache) {
            Debug("Init Viewport 0x{:x} SwapChain", (size_t)viewport);
            auto res = swapchain->init(mDevice, this);
        }

        for (auto& e : neederase) {
            ViewPortSwapChainCache.erase(e);
        }

        RHIThread = std::thread(&VulkanRHI::VulkanRHILoop, this);
        RHIResourceThread = std::thread(&VulkanRHI::VulkanResourceLoop, this);
        PrepareThread = std::thread(&VulkanRHI::PagePrepareLoop, this);

        gIsShuttingDown = 0;

        return true;
    }

    RHITexture* VulkanRHI::RHICreateTexture(const TextureInfo& textureInfo) {
        return new VulkanTexture(mDevice, textureInfo);
    }

    RHIBuffer* VulkanRHI::RHICreateBuffer(const BufferInfo& bufferInfo) {
        return new VulkanBuffer(mDevice, bufferInfo);
    }

    RHIViewport* VulkanRHI::RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type, void* WindowHandle) {
        ViewPortsPool.push_back(new VulkanViewport(this, type, iniWidth, initHeight, WindowHandle));
        return ViewPortsPool.back();
    }

    RHIPipeline* VulkanRHI::RHICreatePipeline(const RHIPipelineState& PipelineState) {
        if (!PipelineCacheMap.contains(PipelineState)) {
            PipelineCacheMap[PipelineState] = new VulkanPipeline(mDevice, mDescriptorPool, PipelineState);
        }
        return PipelineCacheMap[PipelineState];
    }


    RHIShader* VulkanRHI::RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) {
        Debug("Create Vulkan Shader");
        if (!Data) {
            Error("Shader Data is Empty!");
            return nullptr;
        }
        ShadersPool.push_back(new VulkanShader(mDevice, Data, size));
        return ShadersPool.back();
    }

    RHIResourcePackResult VulkanRHI::RHICreateResourcePack(const std::vector<RHIResource*>& resources) {
        return mDescriptorPool->createResourcePack(resources);
    }

    RHIRenderPass* VulkanRHI::RHICreateRenderPass(const RHIRenderPassInfo& info) {
        Debug("Getting Render Pass");
        if (RenderPassCache.find(info) != RenderPassCache.end()) {
            Debug("RenderPass cache hit 0x{:x}", (size_t)RenderPassCache[info]);
            return RenderPassCache[info];
        }
        Debug("Creating new Render Pass");
        auto res = new VulkanRenderPass(mDevice, info);
        if (!res || !res->getRenderPassHandle()) {
            Error("Failed to create VulkanRenderPass");
            return nullptr;
        }
        RenderPassCache[info] = res;
        return res;
    }

    RHIFrameBuffer* VulkanRHI::RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) {
        RHIRenderPass* renderpass = RHICreateRenderPass(info);
        Debug("renderpass pointer = 0x{:x}", (size_t)renderpass);
        auto res = new VulkanFrameBuffer(mDevice, textures, width, height, renderpass);
        Debug("Frame Buffer Handle 0x{:x}", (size_t)res);
        return res;
    }

    RHISemaphore* VulkanRHI::RHICreateSemaphore(const char* name) {
        return mSemaphorePool->createSemaphore(name);
    }

    RHISwapChain* VulkanRHI::RHIGetSwapChain(RHIViewport* viewport) {
        if (!ViewPortSwapChainCache.contains(viewport)) {
            Error("Viwport: 0x{:x} Not Creating viewport!", (size_t)viewport);
            return nullptr;

        }
        return ViewPortSwapChainCache[viewport];
    }

    void VulkanRHI::RHIDestroySemaphore(RHISemaphore* semaphore) {
        mSemaphorePool->release(static_cast<VulkanSemaphore*>(semaphore));
    }

    RHISampler* VulkanRHI::RHICreateSampler(const SamplerInfo& info) {
        return new VulkanSampler(mDevice, info);

    }

    RHIFence* VulkanRHI::RHICreateFence(bool signaled, const char* name) {
        return mFencePool->createFence(signaled, name);
    }

    RingCommandPool::Page* VulkanRHI::RHIGetCommandPoolPage(CmdType cmdtype) {
        return CmdMemoryPool[static_cast<int>(cmdtype) - 1].acquireQue();
    }


    void VulkanRHI::RHIDestroyFence(RHIFence* fence) {
        mFencePool->release(static_cast<VulkanFence*>(fence));
    }

    template<class T_, class F_>
    bool TwoPointerSwapPop(std::vector<T_>& Target, F_ Judgefunc) {
        for (int l = 0, r = Target.size() - 1; l <= r; ) {
            if (Judgefunc(Target[r])) {
                Target.pop_back();
                r--;
            }
            else if (l < r) {
                std::swap(Target[l], Target[r]);
                l++;
            }
            else break;
        }
        return Target.empty();
    }

    void VulkanRHI::PagePrepareLoop() {
        while (!stopTag) {
            bool pushFailed = 1;
            for (auto& CmdPool : CmdMemoryPool) {
                for (auto& page : CmdPool.Pages) {
                    RingCommandPool::PageFlag expect = RingCommandPool::CanRecord;
                    if (page.flags.compare_exchange_strong(expect, RingCommandPool::IsRecording, std::memory_order_acq_rel)) {
                        NeedUsingPages.push(&page);
                        pushFailed = 0;
                    }
                }
            }
            if (pushFailed) std::this_thread::yield();
        }
    }

    void VulkanRHI::VulkanRHILoop() {
        Debug("RHI Thread ID: {}", std::this_thread::get_id());

        struct ResultInfo {
            std::unique_ptr<std::atomic_uint32_t> FinishCount{ std::make_unique<std::atomic_uint32_t>(0) };
            std::vector<std::unique_ptr<ExecuteResultData>> ExecuteResults;

            ResultInfo() {
                FinishCount.reset(new std::atomic_uint32_t(0));
            }

            ResultInfo(ResultInfo&& info) {
                ExecuteResults = std::move(info.ExecuteResults);
                FinishCount = std::move(info.FinishCount);
            }
        };

        std::unordered_map<RingCommandPool::Page*, ResultInfo> ResultCache;

        auto VkRenderCommandPool = mCmdPoolManager->getCommandPool(CmdType::Render);
        auto VkComputeCommandPool = mCmdPoolManager->getCommandPool(CmdType::Compute);
        auto VkTransferCommandPool = mCmdPoolManager->getCommandPool(CmdType::Transfer);
        auto getByType = [VkRenderCommandPool, VkComputeCommandPool, VkTransferCommandPool](CmdType type) -> VulkanCommandPool* {
            switch (type) {
            case CmdType::Render: return VkRenderCommandPool;
            case CmdType::Compute: return VkComputeCommandPool;
            case CmdType::Transfer: return VkTransferCommandPool;
            default: return nullptr;
            }
            };

        std::vector<VkCommandBuffer> RenderCmds, ComputeCmds, TransferCmds;
        auto getCmdsByType = [&RenderCmds, &ComputeCmds, &TransferCmds](CmdType type) -> std::vector<VkCommandBuffer>&{
            switch (type) {
            case CmdType::Render: return RenderCmds;
            case CmdType::Compute: return ComputeCmds;
            case CmdType::Transfer: return TransferCmds;
            default: return RenderCmds; // Default to RenderCmds if type is unknown
            }
            };

        std::vector<CBInfo> RnederCBInfos, ComputeCBInfos, TransferCBInfos;
        auto getCBInfosByType = [&RnederCBInfos, &ComputeCBInfos, &TransferCBInfos](CmdType type) -> std::vector<CBInfo>&{
            switch (type) {
            case CmdType::Render: return RnederCBInfos;
            case CmdType::Compute: return ComputeCBInfos;
            case CmdType::Transfer: return TransferCBInfos;
            default: return RnederCBInfos; // Default to RenderCBInfos if type is unknown
            }
            };


        while (!stopTag) {
            std::vector<RingCommandPool::Page*> NeedClearInThisLoop;
            while (!NeedUsingPages.empty()) {
                RingCommandPool::Page* page;
                if (NeedUsingPages.pop(page)) {
                    auto& entry = ResultCache[page];
                    entry.ExecuteResults.clear();
                    entry.FinishCount->store(0, std::memory_order_release);
                }
            }
            if (NeedUsingPages.empty() && ResultCache.empty()) {
                std::this_thread::yield();
                continue;
            }
            NeedClearInThisLoop.clear();
            for (auto& [page, result] : ResultCache) {
                //get record
                while (!page->BatchQueue.empty()) {
                    RingCommandPool::Page::BatchInfo Batch;
                    page->BatchQueue.pop(Batch);

                    result.ExecuteResults.emplace_back(new ExecuteResultData());
                    auto& info = result.ExecuteResults.back();

                    ThreadPool->pushCommandBatch(Batch, info.get(), result.FinishCount.get());
                }

                //check 
                VulkanFence* fence = nullptr;
                std::vector<RHISemaphore*> waits;
                std::vector<RHISemaphore*> signals;
                if (result.FinishCount->load(std::memory_order_acquire) == result.ExecuteResults.size() && result.ExecuteResults.back()->commandsEndTag) {
                    auto commandPool = getByType(page->cmdtype);
                    auto MCB = commandPool->createCommandBuffer(_Primary_);
                    VkCommandBufferBeginInfo beginInfo{
                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    };
                    VulkanFrameBuffer* currentFrameBuffer = nullptr;
                    std::vector<CBInfo> SecondCBs;
                    vkBeginCommandBuffer(MCB.buffer, &beginInfo);
                    for (auto& SCBRes : result.ExecuteResults) {

                        if (SCBRes->frameBuffer) {
                            if (currentFrameBuffer == nullptr) {
                                currentFrameBuffer = SCBRes->frameBuffer;
                                VkClearValue clearValue[2]{{}, {}};
                                clearValue[0].color = {
                                    SCBRes->clearValue.colorinfo.R,
                                    SCBRes->clearValue.colorinfo.G,
                                    SCBRes->clearValue.colorinfo.B,
                                    SCBRes->clearValue.colorinfo.A
                                };
                                if (SCBRes->clearValue.DepthStencilClear){
                                    clearValue[1].depthStencil = {
                                        .depth = SCBRes->clearValue.depthclearval,
                                        .stencil = SCBRes->clearValue.stencilVal
                                    };
                                }
                                VkRenderPassBeginInfo Info{
                                    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                    .renderPass = static_cast<VkRenderPass>((currentFrameBuffer)->getFrameRenderPass()->getRenderPassHandle()),
                                    .framebuffer = static_cast<VkFramebuffer>((currentFrameBuffer)->getResourceAPIHandle()),
                                    .renderArea = {{0,0},{currentFrameBuffer->getFrameWidth(), currentFrameBuffer->getFrameHeight()}},
                                    .clearValueCount = SCBRes->clearValue.DepthStencilClear ? 2u : 1u,
                                    .pClearValues = clearValue,
                                };
                                vkCmdBeginRenderPass(MCB.buffer, &Info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
                            }
                            else {
                                Error("Nested render pass not supported");
                            }
                        }
                        vkCmdExecuteCommands(MCB.buffer, 1, &SCBRes->ExecutedCB.buffer);

                        if (SCBRes->renderPassEndTag) {
                            if (currentFrameBuffer) {
                                vkCmdEndRenderPass(MCB.buffer);
                                currentFrameBuffer = nullptr;
                            }
                            else {
                                Error("EndRenderPass without active render pass");
                            }
                        }
                        SecondCBs.emplace_back(std::move(SCBRes->ExecutedCB));
                        if (SCBRes->commandsEndTag) {
                            fence = SCBRes->fence.load(std::memory_order_acquire);


                            Debug("RHILoop: SCBRes�����ַ = 0x{:x}, fence��ַ = 0x{:x}, ��ȡֵ = 0x{:x}",
                                (size_t)SCBRes.get(),
                                (size_t) & (SCBRes->fence),
                                (size_t)SCBRes->fence.load());
                            waits = std::move(SCBRes->waits);
                            signals = std::move(SCBRes->signals);
                        }
                    }
                    vkEndCommandBuffer(MCB.buffer);

                    if (fence || waits.empty() || signals.empty()) {
                        VulkanFence* submitFence = fence ? static_cast<VulkanFence*>(fence) : mFencePool->createFence();
                        Debug("Page Fene is 0x{:x}, Pushed fence 0x{:x} to PendingReleaseCBs", (size_t)fence, (size_t)submitFence);
                        mDevice->submitCommandBuffer({ MCB.buffer }, page->cmdtype, signals, waits, submitFence);
                        SecondCBs.emplace_back(std::move(MCB));
                        PendingReleaseCBs.push(PendingReleaseInfo(submitFence, std::move(SecondCBs), 1));
                    }
                    else {
                        getCmdsByType(page->cmdtype).push_back(MCB.buffer);
                        getCBInfosByType(page->cmdtype).emplace_back(std::move(MCB));

                        auto& cbinfos = getCBInfosByType(page->cmdtype);
                        cbinfos.insert(cbinfos.end(), SecondCBs.begin(), SecondCBs.end());
                    }
                    NeedClearInThisLoop.push_back(page);
                    page->flags.store(RingCommandPool::IsEnd, std::memory_order_release);
                }
            }

            for (auto& page : NeedClearInThisLoop) {
                ResultCache.erase(page);
                page->Pool->recycleQue(page);
            }

            for (int i = (int)CmdType::Render; i <= (int)CmdType::Transfer; i++) {
                auto& cmds = getCmdsByType(CmdType(i));
                auto& cmdInfos = getCBInfosByType(CmdType(i));
                if (cmds.size() < 8) continue;
                auto fence = mFencePool->createFence(false, "RHI Fence");
                mDevice->submitCommandBuffer(cmds, CmdType(i), {}, {}, fence);
                PendingReleaseCBs.push(PendingReleaseInfo(static_cast<VulkanFence*>(fence), std::move(cmdInfos), 0));
                cmds.clear();
            }
            
        }

        for (auto& [page, result] : ResultCache) {
            while (page->BatchQueue.pop()) {}
            page->flags.store(RingCommandPool::IsEnd, std::memory_order_release);
        }
        ResultCache.clear();
        while (NeedUsingPages.pop()) {}

        Debug("RHI Loop drain complete");
    }


    void VulkanRHI::VulkanResourceLoop() {
        Debug("Resource Thread ID: {}", std::this_thread::get_id());
        while (!stopTag || !PendingReleaseCBs.empty() || !PendingReleaseCBsInThread.empty()) {
            static int shrink_counter = 0;
            if (++shrink_counter % 60 == 0 && PendingReleaseCBsInThread.empty()) {
                PendingReleaseCBsInThread.shrink_to_fit();
            }

            if (!PendingReleaseCBsInThread.empty()) {
                std::vector<VkFence> fences;
                std::vector<size_t> indices;
                fences.reserve(PendingReleaseCBsInThread.size());
                indices.reserve(indices.size());

                for (size_t i = 0; i<PendingReleaseCBsInThread.size(); i++) {
                    auto& [fence, cbs, fromframe] = PendingReleaseCBsInThread[i];
                    fences.push_back(static_cast<VkFence>(fence->getFenceHandle()));
                    indices.push_back(i);
                }

                bool needClear = 0;
                if (!fences.empty()) {
                    VkResult res = vkWaitForFences(
                        mDevice->getLogicalDevice(), 
                        static_cast<uint32_t>(fences.size()),
                        fences.data(),
                        VK_FALSE,
                        1000000
                    );
                    needClear = (res == VK_SUCCESS);
                }
                if (needClear)
                    TwoPointerSwapPop(PendingReleaseCBsInThread, [this](PendingReleaseInfo& value)->bool {
                        auto& [fence, cbs, fromframe] = value;
                        //if (fence) Debug("Pendding fence: 0x{:x}, signaled: {}", (size_t)fence, fence ? fence->isSignaled() : true);
                        if (!fence || fence->isSignaled()) {
                            if (fence && !fromframe) mFencePool->release(static_cast<VulkanFence*>(fence));
                            for (auto& cb : cbs) {
                                cb.pool->releaseCommandBuffer(cb);
                                for (auto& [resource, change] : cb.QuoteResources) {
                                    if (resource->getResourceType() == Type::Texture) {
                                        static_cast<VulkanTexture*>(resource)->transitionLayout(change.layout);
                                    }
                                }
                                cb.QuoteResources.clear();
                            }
                            cbs.clear();
                            return true;
                        }
                        return stopTag;
                    });
            }

            PendingReleaseInfo Info;
            if (!PendingReleaseCBs.empty()) {
                if (!PendingReleaseCBs.pop_wait(Info)) {
                    Warn("PendingReleaseCBs Pop Failed");
                    continue;
                }
                auto& [fence, cbs, fromframe] = Info;

                if (!fence || fence->isSignaled()) {
                    for (auto& cb : cbs) {
                        cb.pool->releaseCommandBuffer(cb);
                        for (auto& [resource, change] : cb.QuoteResources) {
                            if (resource->getResourceType() == Type::Texture) {
                                static_cast<VulkanTexture*>(resource)->transitionLayout(change.layout);
                            }
                        }
                        cb.QuoteResources.clear();
                    }
                    if (fence && !fromframe) mFencePool->release(static_cast<VulkanFence*>(fence));
                }
                else {
                    PendingReleaseCBsInThread.push_back(std::move(Info));
                }

            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

        }
    }

    /*
        __VK_GLOBAL_FUNC__
    */
    VkInstance GetGlobalInstance() {
        return gInstance;
    }

}


/*
  EXPORT
*/
FISIR::DynamicRHI* RHICreate() {
    Info("Vulkan RHI Create!");
    return new FISIR::VulkanRHI();
}


void RHIDestroy(FISIR::DynamicRHI* rhi) {
    FISIR::gIsShuttingDown = 1;

    delete rhi;
    vkDestroyInstance(FISIR::gInstance, nullptr);
}
