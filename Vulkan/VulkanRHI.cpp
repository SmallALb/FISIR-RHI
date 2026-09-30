

#include "VulkanRHI.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <unordered_map>
#include "../Log/Logger.h"
#include "../LockFreeQue.h"
#include "../RHICommandList.h"
#include "ChangeImageFlagsToVulkanFlags.h"
#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanFrameBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanSampler.h"
#include "VulkanShader.h"
#include "VulkanSwapChian.h"
#include "VulkanTexture.h"
#include "VulkanViewport.h"
namespace FISIR {

    /*

        CommandPool Manager and Thread Context

    */
    //Pending Release Command Buffers
    struct PendingReleaseInfo {
        PendingReleaseInfo() {}
        PendingReleaseInfo(VkFence f, std::vector<CBInfo>&& c, bool infence = 0, uint32_t tsQuery = UINT32_MAX) :
            fence(f),
            cbInfos(std::move(c)),
            inputFence(infence),
            timestampQueryStart(tsQuery)
        {}
        PendingReleaseInfo(VkFence f, const std::vector<CBInfo>& c) : fence(f), cbInfos(c) {}


        PendingReleaseInfo(PendingReleaseInfo&& other) noexcept
            : fence(other.fence), cbInfos(std::move(other.cbInfos)), inputFence(other.inputFence),
              timestampQueryStart(other.timestampQueryStart) {
            other.fence = nullptr;
        }

        PendingReleaseInfo& operator=(PendingReleaseInfo&& other) noexcept {
            if (this != &other) {
                fence = other.fence;
                cbInfos = std::move(other.cbInfos);
                inputFence = other.inputFence;
                timestampQueryStart = other.timestampQueryStart;
                other.fence = nullptr;
            }
            return *this;
        }

        VkFence fence{ nullptr };
        std::vector<CBInfo> cbInfos{};
        bool inputFence{ 0 };
        uint32_t timestampQueryStart{ UINT32_MAX };   // 时间戳查询起始索引，UINT32_MAX 表示无
    };


    static LockFreeQue<PendingReleaseInfo> PendingReleaseCBs;
    static LockFreeQue<RingCommandPool::Page*> NeedSubmitQue;


    //Pending Upload Command Buffers
    std::thread RHIThread, RHIResourceThread, PrepareThread;



    /*
        ViewPort SwapChain Cache

    */
    std::vector<VulkanViewport*> ViewPortsPool;

    static std::unordered_map<RHIViewport*, VulkanSwapChain*> ViewPortSwapChainCache;

    static std::vector<VulkanShader*> ShadersPool;

    /*
        SwapChain Registry

        取图（acquire）与呈现（present）全归 RHI 线程：主线程只置位取图请求，RHI 线程每轮
        轮询 tryAcquire()，present 则在它自己提交完这一页之后执行。VkSwapchainKHR 与 VkQueue
        都是外部同步对象，跨线程碰就会报 THREADING ERROR / 设备丢失。
    */
    static std::array<std::atomic<VulkanSwapChain*>, VulkanRHI::MaxSwapChainCount> mSwapChains{};

    void VulkanRHI::RegisterSwapChain(VulkanSwapChain* swapchain) {
        if (!swapchain) return;
        for (auto& e : mSwapChains) {
            VulkanSwapChain* expected = nullptr;
            if (e.compare_exchange_strong(expected, swapchain, std::memory_order_acq_rel)) return;
        }
        Error("RegisterSwapChain: registry full ({}), swapchain 0x{:x} NOT registered",
              MaxSwapChainCount, (size_t)swapchain);
    }

    void VulkanRHI::UnregisterSwapChain(VulkanSwapChain* swapchain) {
        if (!swapchain) return;
        for (auto& e : mSwapChains) {
            if (e.load(std::memory_order_acquire) == swapchain) {
                e.store(nullptr, std::memory_order_release);
                return;
            }
        }
    }

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

        vkDeviceWaitIdle(mDevice->getLogicalDevice());

        RHIThread.join();
        Debug("VulkanRHI Thread Join");

        RHIResourceThread.join();
        Debug("VulkanResource Thread Join");




        PendingReleaseCBs.forceClear();
        NeedSubmitQue.forceClear();
        for (auto& pool : CmdMemoryPool) {
            for (auto& page : pool.Pages) {
                page.BatchQueue.forceClear();
            }
            pool.FreePages.forceClear();
        }

        delete ThreadPool;
        Debug("Vulkan Thread Pool Join");

        for (auto& [name, pipeline] : getPipelineCacheMap()) delete pipeline;

        // ★ 下面这些容器都是**进程级 static**：只 delete 元素而不清表的话，同一个进程里第二次
        // 初始化（Android 窗口被回收后重建）会从表里取到上一代**已销毁**的对象 ——
        // 这正是「切后台回来就崩」的根因，所以每个都要 clear。
        for (auto& [info, renderPass] : RenderPassCache) delete renderPass;
        RenderPassCache.clear();

        for (auto& shader : ShadersPool) delete shader;
        ShadersPool.clear();

        for (auto& [viewport, swapchain] : ViewPortSwapChainCache) {
            delete swapchain;
            delete viewport;
        }
        ViewPortSwapChainCache.clear();
        for (auto& slot : mSwapChains) slot.store(nullptr);

        for (auto& [name, layout] : getPipelineLayoutMap()) {
			vkDestroyPipelineLayout(mDevice->getLogicalDevice(), layout, nullptr);
        }

        // ★ 这两个缓存是**进程级 static**（VulkanPipelineLayoutCache.h），必须清空：
        // 上面刚把 VkPipelineLayout / VulkanPipeline 释放掉，条目留着的话，同一个进程里
        // 第二次初始化会命中悬垂句柄/野指针 —— Android 上窗口重建走的就是这条路。
        getPipelineLayoutMap().clear();
        getPipelineCacheMap().clear();

        // ★ 呈现通道着色器同样是进程级静态（VulkanSwapChian.cpp 的 VShader/FShader），
        // 必须一起复位：否则窗口被回收后重建时会复用上一代**已销毁**的 RHIShader*，
        // 在 VulkanPipeline 构造里解引用野指针直接崩（实测 fault addr 0x20，
        // 栈为 VulkanSwapChain → VulkanRHI → VulkanPipeline）—— Android 切后台回来就是这个。
        VulkanSwapChain::ResetPresentShaders();

        Debug("Destroy Fence and Semaphore Pool");
        mFencePool->destroyPool();
        delete mFencePool;
        delete mSemaphorePool;
        delete mDescriptorPool;
        delete mCmdPoolManager;

        Debug("Destroy Device");
        if (mTimestampQueryPool) {
            vkDestroyQueryPool(mDevice->getLogicalDevice(), mTimestampQueryPool, nullptr);
            mTimestampQueryPool = nullptr;
        }
        mDevice->Destory();
        Debug("Vulkan RHI Destroyed!");

        DestroyDebugReportCallback();
        Debug("Vulkan RHI DebugCall Destroyed!");

    }

    bool VulkanRHI::Init() {
        if (!gInstance) return false;
        if (!MakeDebugReportCallback()) return false;
        mDevice = new VulkanDevice(SelectDevice(gInstance));

        if (!mDevice->Init()) {
            Error("Failed to initialize Vulkan Device!");
            return false;
        }

        mFencePool = new VulkanFencePool(mDevice);
        mSemaphorePool = new VulkanSemaphorePool(mDevice);
        mDescriptorPool = new VulkanDescriptorPool(mDevice);

        mCmdPoolManager = new VulkanCommandPoolManager(mDevice);

        // GPU 时间戳查询池：每帧首尾各一个时间戳，环式复用（最多 32 帧在飞，远大于交换链槽数）。
        mTimestampPeriod = mDevice->getTimestampPeriod();
        if (mTimestampPeriod > 0.0f) {
            constexpr uint32_t kTimestampQueryCount = 64;
            VkQueryPoolCreateInfo qpInfo{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = kTimestampQueryCount,
            };
            if (vkCreateQueryPool(mDevice->getLogicalDevice(), &qpInfo, nullptr, &mTimestampQueryPool) != VK_SUCCESS) {
                mTimestampQueryPool = nullptr;
                mTimestampPeriod = 0.0f;
            }
        }
        Debug("GPU timestamp period={} ns, queryPool=0x{:x}", mTimestampPeriod, (size_t)mTimestampQueryPool);

        ThreadPool = new CommandExecuteThreadPool(mDevice, mCmdPoolManager);

        for (int i = 0; i < 3; i++) {
            CmdMemoryPool[i].cmdType = static_cast<CmdType>(i + 1);
        }

        RHIThread = std::thread(&VulkanRHI::VulkanRHILoop, this);
        RHIResourceThread = std::thread(&VulkanRHI::VulkanResourceLoop, this);

        gIsShuttingDown = 0;

        return true;
    }

    RHITexture* VulkanRHI::RHICreateTexture(const TextureInfo& textureInfo) {
        return new VulkanTexture(mDevice, textureInfo);
    }

    RHIBuffer* VulkanRHI::RHICreateBuffer(const BufferInfo& bufferInfo) {
        return new VulkanBuffer(mDevice, bufferInfo);
    }

    RHIViewport* VulkanRHI::RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type,
                                              DisplayDeviceType deviceType, void* deviceHandle,
                                              uint32_t swapChainSlotCount) {
        ViewPortsPool.push_back(new VulkanViewport(this, type, iniWidth, initHeight, deviceType, deviceHandle, swapChainSlotCount));
        return ViewPortsPool.back();
    }

    RHIPipeline* VulkanRHI::RHICreatePipeline(const RHIPipelineState& PipelineState) {
        if (!getPipelineCacheMap().contains(PipelineState)) {
            getPipelineCacheMap()[PipelineState] = new VulkanPipeline(mDevice, mDescriptorPool, PipelineState);
        }
        return getPipelineCacheMap()[PipelineState];
    }


    RHIShader* VulkanRHI::RHICreateShader(ShaderTYP typ, const char* EntryPoint, const unsigned char* Data, size_t size) {
        Debug("Create Vulkan Shader");
        if (!Data) {
            Error("Shader Data is Empty!");
            return nullptr;
        }
        ShadersPool.push_back(new VulkanShader(mDevice, EntryPoint, Data, size));
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

    RHISemaphore* VulkanRHI::RHICreateSemaphore(const char* name, FenceType typ) {
        return mSemaphorePool->createSemaphore(name, typ);
    }

    RHISwapChain* VulkanRHI::RHIGetSwapChain(RHIViewport* viewport) {
        auto it = ViewPortSwapChainCache.find(viewport);
        if (it != ViewPortSwapChainCache.end()) return it->second;

        // Init() 之后的运行时视口（ImGui 把面板拖出主窗口新建的那些）不在 Init 的统一创建列表里，
        // 而 Init 那套「每视口一个独占呈现队列」必须在 vkCreateDevice 时就定，事后加不了队列。
        // 于是这里按需补建：复用既有队列族，并且**必须补上 Init() 里那步 swapchain->init()** ——
        // 那个 (viewport, family, slot) 构造函数只记录 surface/队列族/槽位数，不设 mDevice、
        // 不建交换链、不建呈现管线；少了 init() 的话 mDevice 还是未初始化值，下一次
        // acquireGetImageInfoID() → recreateSwapChain() 里 mDevice->getLogicalDevice() 会直接崩。
        if (!viewport) {
            Error("RHIGetSwapChain: viewport is null");
            return nullptr;
        }
        VulkanViewport* vkViewport = static_cast<VulkanViewport*>(viewport);

        // Headless（以及本后端未实现的设备类型）没有 surface，也就没有交换链可建 —— 这不是错误：
        // 这条路上调用方直接渲染到离屏目标（RHITexture/RHIFrameBuffer），present 由它自己决定。
        if (!vkViewport->hasSurface()) {
            Info("[Vulkan] viewport 0x{:x}（{}）没有 surface，RHIGetSwapChain 返回 nullptr",
                 (size_t)viewport, DisplayDeviceTypeName(vkViewport->getDisplayDeviceType()));
            ViewPortSwapChainCache[viewport] = nullptr;   // 记下来，后续调用同一路径返回
            return nullptr;
        }

        const int32_t family = mDevice->FindPresentQueueFamilyForSurface(vkViewport->getVkSurface());
        if (family < 0) {
            Error("Viwport: 0x{:x} Not Creating viewport!", (size_t)viewport);
            return nullptr;
        }
        VulkanSwapChain* created =
            new VulkanSwapChain(vkViewport, (uint32_t)family, vkViewport->getSwapChainSlotCount());
        created->init(mDevice, this);   // 与 VulkanRHI::Init() 里那句完全一致
        ViewPortSwapChainCache[viewport] = created;
        // 登记给 RHI 线程轮询（此后它的取图/呈现都由那一线程执行）。
        RegisterSwapChain(created);
        return created;
    }

    void VulkanRHI::RHIDestroySemaphore(RHISemaphore* semaphore) {
        mSemaphorePool->release(static_cast<VulkanSemaphore*>(semaphore));
    }

    RHISampler* VulkanRHI::RHICreateSampler(const SamplerInfo& info) {
        return new VulkanSampler(mDevice, info);

    }

    RHIFence* VulkanRHI::RHICreateFence(bool signaled, const char* name) {
        return new VulkanFence(mFencePool, signaled, name);
    }

    RingCommandPool::Page* VulkanRHI::RHIGetCommandPoolPage(CmdType cmdtype) {
        auto* page = CmdMemoryPool[static_cast<int>(cmdtype) - 1].acquireQue();
        // 页面分配即代表「这一次录制」的开始，而所有录制都发生在用户的提交线程上、顺序即录制顺序，
        // 因此在分配处发号即可得到全局单调的录制序号。RHI 线程据此保证提交顺序 == 录制顺序。
        page->recordOrder = mRecordSequence.fetch_add(1, std::memory_order_relaxed) + 1;
		NeedSubmitQue.push(page);
        return page;
    }




    void VulkanRHI::RHIDestroyFence(RHIFence* fence) {
        delete fence;
    }

    void VulkanRHI::RHIDestroyTexture(RHITexture* texture) {
        delete static_cast<VulkanTexture*>(texture);
    }

    void VulkanRHI::RHIDestroyBuffer(RHIBuffer* buffer) {
        delete static_cast<VulkanBuffer*>(buffer);
    }

    void VulkanRHI::RHIDestroySampler(RHISampler* sampler) {
        delete static_cast<VulkanSampler*>(sampler);
    }

    void VulkanRHI::RHIDestroyResourcePack(RHIResourcePackResult& pack) {
        if (pack.ResourcePack) { mDescriptorPool->destroyResourcePack(pack.ResourcePack); pack.ResourcePack = nullptr; }
        if (pack.SamplerPack)  { mDescriptorPool->destroyResourcePack(pack.SamplerPack);  pack.SamplerPack = nullptr; }
    }

    void VulkanRHI::RHIDestroyFrameBuffer(RHIFrameBuffer* frameBuffer) {
		delete static_cast<VulkanFrameBuffer*>(frameBuffer);
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


    void VulkanRHI::VulkanRHILoop() {
        Debug("RHI Thread ID: 0x{:x}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

        struct ResultInfo {
            std::unique_ptr<std::atomic_uint32_t> FinishCount{ std::make_unique<std::atomic_uint32_t>(0) };
            std::vector<std::unique_ptr<ExecuteResultData>> ExecuteResults;
            
            bool isDone() const {
				return FinishCount->load(std::memory_order_acquire) == ExecuteResults.size() && !ExecuteResults.empty() && ExecuteResults.back()->commandsEndTag.load(std::memory_order_acquire);
			}
            

            ResultInfo() {
                FinishCount.reset(new std::atomic_uint32_t(0));
            }

            ResultInfo(ResultInfo&& info) {
                Warn("ResultInfo moved from 0x{} to 0x{:x}\n", (void*)&info, (void*)this);
                ExecuteResults = std::move(info.ExecuteResults);
                FinishCount = std::move(info.FinishCount);
            }
        };

        struct SubmitNode{
            CBInfo MCB;
            std::vector<CBInfo> SCBs;
            std::vector<RHISemaphore*> waits;
            std::vector<RHISemaphore*> signals;
            // 本节点携带的呈现请求（Present 指令）。慢路径下节点会被推迟提交，
            // presents 必须跟着节点走 —— 丢一次 present，主线程下一次取图就白等一次。
            std::vector<std::pair<RHISwapChain*, uint32_t>> presents;
            VulkanFence* fence { nullptr };   // End() 传入的用户围栏，可为空
            CmdType cmdtype;
            bool HadSubmited { false };
            bool HadCB { false };
            uint32_t tsQuery { UINT32_MAX };   // 时间戳查询起始索引，UINT32_MAX 表示无
        };

        // 实际节点数据（本函数持有）。用 unique_ptr 保证回收某个节点时不会让其它节点
        // 的地址失效 —— SemaphoreSignaler 里保存的正是这些裸指针。
        std::deque<std::unique_ptr<SubmitNode>> SubmitNodes;


        std::vector<SubmitNode*> PendingNodes;

        // 信号量 -> 负责 signal 它的节点（所有者）。用于解析 wait 依赖。
        std::unordered_map<RHISemaphore*, SubmitNode*> SemaphoreSignaler;

        std::unordered_map<RingCommandPool::Page*, ResultInfo> ResultCache;
        std::vector<RingCommandPool::Page*> SubmitOrder;

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


        std::vector<CBInfo> RnederCBInfos, ComputeCBInfos, TransferCBInfos;
        auto getCBInfosByType = [&RnederCBInfos, &ComputeCBInfos, &TransferCBInfos](CmdType type) -> std::vector<CBInfo>&{
            switch (type) {
            case CmdType::Render: return RnederCBInfos;
            case CmdType::Compute: return ComputeCBInfos;
            case CmdType::Transfer: return TransferCBInfos;
            default: return RnederCBInfos; // Default to RenderCBInfos if type is unknown
            }
        };


		// 呈现落地：Present 指令由录制线程登记，真正 vkQueuePresentKHR 在这里 —— RHI 线程上、
		// 本页/本节点 vkQueueSubmit 之后 —— 执行。同一线程内 submit 在前，故 present 等待的
		// renderFinish 必然已有对应的 signal 提交在队列里（校验层不会再报 "has no way to be signaled"）。
		auto drainPresents = [](const std::vector<std::pair<RHISwapChain*, uint32_t>>& presents) {
			for (const auto& [swapchain, frameID] : presents)
				if (swapchain) static_cast<VulkanSwapChain*>(swapchain)->presentNow(frameID);
		};

        // 提交一个命令缓冲（无信号量与有信号量两条路径共用）：建围栏、提交、入队待回收。
        auto doSubmit = [&](CBInfo& MCB, std::vector<CBInfo>& SCBs, CmdType cmdtype,
                            const std::vector<RHISemaphore*>& waits, const std::vector<RHISemaphore*>& signals,
                            VulkanFence* userFence, uint32_t tsQuery = UINT32_MAX) {
            VkFence fencehandle = userFence
                ? static_cast<VkFence>(userFence->getFenceHandle())
                : mFencePool->createFence(false);
            if (mFencePool->isSignaled(fencehandle))
                vkResetFences(mDevice->getLogicalDevice(), 1, &fencehandle);
            mDevice->submitCommandBuffer({ MCB.buffer }, cmdtype, signals, waits, fencehandle);
            if (userFence) userFence->setSubmited();
            SCBs.emplace_back(std::move(MCB));
            PendingReleaseCBs.push(PendingReleaseInfo(fencehandle, std::move(SCBs), 0, tsQuery));
        };
        
		// 深度优先遍历提交节点：若依赖节点未提交，先递归提交它们。
		auto trysubmit = [&SemaphoreSignaler, &doSubmit, &drainPresents](SubmitNode* node, auto&& dfs_ref) -> bool {
            if (node->HadSubmited) return true;
			if (!node->HadCB) {
                bool allKnown = true;
                for (RHISemaphore* w : node->waits) {

                    if (w->isExternalSignal()) continue;
                    if (!SemaphoreSignaler.contains(w)) { allKnown = false; break; }
                }
                if (!allKnown) return false;
                node->HadCB = true;
            }
            // 解析依赖：wait 的信号量必须已在所有权表里登记生产者 —— 生产者可能尚未提交
            // （递归先提交它），也可能早已提交（HadSubmited，dfs 立即返回 true）。
            // 表里查不到说明生产者节点还没被录入（它的页还没处理完），本节点留在
            // PendingNodes 里下一轮再试。
            std::vector<std::pair<RHISemaphore*, SubmitNode*>> resolved;
            resolved.reserve(node->waits.size());
			for (auto* waitSem : node->waits) if (!waitSem->isExternalSignal()) {
                auto ownerIt = SemaphoreSignaler.find(waitSem);
                if (ownerIt == SemaphoreSignaler.end()) return false;
                if (!dfs_ref(ownerIt->second, dfs_ref)) return false;
                resolved.emplace_back(waitSem, ownerIt->second);
			}
            doSubmit(node->MCB, node->SCBs, node->cmdtype, node->waits, node->signals, node->fence, node->tsQuery);
			node->HadSubmited = true;
            // 同一个 RHI 线程、紧随本次 vkQueueSubmit 之后落地呈现（顺序天然正确）。
            drainPresents(node->presents);
            // 依赖已消费：只摘除「仍指向本次解析到的那个生产者」的表项，避免误删生产者
            // 新一轮（同一信号量被复用）刚登记的所有权。
            for (auto& [waitSem, owner] : resolved) {
                auto it = SemaphoreSignaler.find(waitSem);
                if (it != SemaphoreSignaler.end() && it->second == owner) SemaphoreSignaler.erase(it);
            }
			return true;
		};

        while (!stopTag && !mDevice->isDeviceLost()) {
            // ★ 顺序很关键：**先把本线程手里能提交的都提交掉，最后才替录制线程取图**。
            // 取图路径里会等「该槽上一帧的 GPU 完成」（tryAcquire），而那一帧的提交很可能还在等一个
            // 二元信号量（acquire / 解析 pass 的完成信号量）——能发出那些 signal 的只有本线程。
            // 原来把取图放在循环**开头**：一旦在那里阻塞，本线程就再也走不到下面的「排空页面 → 提交」，
            // 于是永远等不到自己该发出的 signal ⇒ 结构性自锁（Release 实测：一拖出新建窗口就卡死，
            // 栈为 VulkanRHILoop → tryAcquire → VulkanFence::wait → vkWaitForFences）。
            // 放到循环末尾之后，取图看到的永远是「本轮已提交完毕」的世界，依赖自然闭合。

            {
                RingCommandPool::Page* page;
                while (NeedSubmitQue.pop(page)) {
                    auto& entry = ResultCache[page];
                    entry.ExecuteResults.clear();
                    entry.FinishCount->store(0, std::memory_order_release);
                    SubmitOrder.push_back(page);
                }
            }

            for (auto* page : SubmitOrder) {
                auto& result = ResultCache[page];
                RingCommandPool::Page::BatchInfo Batch;
                while (!page->BatchQueue.empty() && page->BatchQueue.pop(Batch)) {
                    // 大批次拆分：单个渲染通道往往包含成百上千条绘制命令，而每条批次由
                    // 一个工作线程串行翻译。命令数超过阈值时，把该渲染通道切成多段，
                    // 分派给多个工作线程并行翻译（同一渲染通道内多个二级命令缓冲，
                    // 一级命令缓冲按序 execute 它们），从而把 5 个工作线程都用满。
                    constexpr size_t kSplitMinCmds = 64;
                    constexpr size_t kMaxSplits = 5;
                    if (Batch.page == nullptr || Batch.ReadBegin >= Batch.ReadEnd) break;

                    RHICommandT firstCmd = Batch.getCommandType();
                    if (firstCmd == RHICommandT::BeginRenderPass) {
                        // 用副本读取 BeginRenderPass，拿到续接段所需的继承信息（不扰动 Batch）。
                        RingCommandPool::Page::BatchInfo peek = Batch;
                        BeginRenderPass_CmdInfo rpInfo{};
                        peek.getBatchData(rpInfo);
                        VulkanFrameBuffer* rpFb = static_cast<VulkanFrameBuffer*>(rpInfo.frame);
                        uint32_t rpSubpass = rpInfo.subpassIndex;
                        size_t afterBegin = peek.ReadBegin;

                        // 收集 BeginRenderPass 之后每条命令的起始偏移（按命令边界）。
                        std::vector<size_t> starts;
                        size_t cur = afterBegin;
                        while (cur < Batch.ReadEnd) {
                            starts.push_back(cur);
                            RingCommandPool::Page::BatchInfo walker{ Batch.page, cur, Batch.ReadEnd, Batch.order };
                            walker.skipCommand();
                            cur = walker.ReadBegin;
                        }
                        size_t N = starts.size();

                        // 命令数足够多才拆分：小渲染通道（几组绑定 + 少量绘制）若被拆成
                        // 多段，会把视口/裁剪/描述符绑定与绘制命令分隔到不同二级命令缓冲，
                        // 破坏渲染通道内的状态连续，导致设备丢失。故低于阈值走单段。
                        size_t K = (N > kSplitMinCmds) ? std::min<size_t>(kMaxSplits, N) : 1;

                        for (size_t i = 0; i < K; ++i) {
                            size_t cmdStart = i * N / K;
                            size_t cmdEnd = (i + 1) * N / K;
                            size_t s = (i == 0) ? Batch.ReadBegin : starts[cmdStart];
                            size_t e = (cmdEnd >= N) ? Batch.ReadEnd : starts[cmdEnd];

                            result.ExecuteResults.emplace_back(new ExecuteResultData());
                            ExecuteResultData* res = result.ExecuteResults.back().get();
                            if (i > 0) {
                                // 续接段：不以 BeginRenderPass 开头，携带预置继承信息。
                                res->inheritFrameBuffer = rpFb;
                                res->inheritSubpass = rpSubpass;
                            }
                            ThreadPool->pushCommandBatch(
                                RingCommandPool::Page::BatchInfo{ Batch.page, s, e, Batch.order },
                                res, result.FinishCount.get());
                        }
                    } else {
                        // 非渲染通道批次（End 命令等）：单段，保持原逻辑。
                        result.ExecuteResults.emplace_back(new ExecuteResultData());
                        ExecuteResultData* res = result.ExecuteResults.back().get();
                        ThreadPool->pushCommandBatch(Batch, res, result.FinishCount.get());
                    }
                }
            }
            for (auto* page : SubmitOrder) {
                auto& result = ResultCache[page];
                if (!result.isDone()) continue;

                VulkanFence* fence = nullptr;
                std::vector<RHISemaphore*> waits;
                std::vector<RHISemaphore*> signals;

                auto commandPool = getByType(page->cmdtype);
                auto MCB = commandPool->createCommandBuffer(_Primary_);
                VkCommandBufferBeginInfo beginInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                };
                VulkanFrameBuffer* currentFrameBuffer = nullptr;
                std::vector<CBInfo> SecondCBs;

                // 分配时间戳查询索引（环式），首尾各写一个，围栏置位后由资源线程读回。
                // 只在 graphics（Render）队列写时间戳：Nanite 的 compute 与 graphics 分属
                // 不同队列族，跨族写时间戳/重置查询在部分驱动上会触发校验错误甚至设备丢失。
                uint32_t tsQuery = UINT32_MAX;
                if (mTimestampQueryPool && page->cmdtype == CmdType::Render) {
                    uint32_t ring = mTimestampRing.fetch_add(1, std::memory_order_relaxed);
                    tsQuery = (ring % 32u) * 2u;
                }

                vkBeginCommandBuffer(MCB.buffer, &beginInfo);
                if (tsQuery != UINT32_MAX) {
                    vkCmdResetQueryPool(MCB.buffer, mTimestampQueryPool, tsQuery, 2);
                    vkCmdWriteTimestamp(MCB.buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestampQueryPool, tsQuery);
                }
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

                        waits = std::move(SCBRes->waits);
                        signals = std::move(SCBRes->signals);
                    }
                }
                if (tsQuery != UINT32_MAX)
                    vkCmdWriteTimestamp(MCB.buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestampQueryPool, tsQuery + 1);
                vkEndCommandBuffer(MCB.buffer);

                if (waits.empty() && signals.empty()) {
                    // 无信号量：有围栏立即提交，无围栏累积批量化（与旧版一致）。
                    doSubmit(MCB, SecondCBs, page->cmdtype, waits, signals, fence, tsQuery);
                    // 已在本线程提交完，紧接着落地本页的呈现请求。
                    for (auto& SCBRes : result.ExecuteResults) drainPresents(SCBRes->presents);
                }
                else {
                    // 慢路径：依赖未满足，创建节点，交由下方拓扑序遍历提交。
                    SubmitNodes.emplace_back(std::make_unique<SubmitNode>());
                    SubmitNode& node = *SubmitNodes.back();
                    node.MCB = std::move(MCB);
                    node.SCBs = std::move(SecondCBs);
                    node.waits = std::move(waits);
                    node.signals = std::move(signals);
                    node.fence = fence;
                    node.cmdtype = page->cmdtype;
                    node.HadSubmited = false;
                    node.HadCB = true;
                    node.tsQuery = tsQuery;

                    // 呈现请求挂在节点上：页面在本轮末尾就会被回收（ResultCache 随之清空），
                    // 而节点可能要到后面几轮才真正 vkQueueSubmit —— presents 必须跟着走，
                    // 丢一次 present，主线程下一次取图就会白等。
                    for (auto& SCBRes : result.ExecuteResults)
                        node.presents.insert(node.presents.end(), SCBRes->presents.begin(), SCBRes->presents.end());

                    // 解析 HadCB：所有 wait 要么已知所有者，要么为外部信号量（交换链 acquire）。
                    //for (RHISemaphore* w : node.waits) {
                    //    if (w->isExternalSignal()) continue;
                    //    if (!SemaphoreSignaler.contains(w)) { node.HadCB = false; break; }
                    //}

                    // 登记所有权：本节点 signal 的每个信号量，其所有者即本节点。
                    // 表项必须活到「等待该信号量的消费者提交」为止，不能提前清空（见文件末尾
                    // 空闲回收处注释）。
                    for (RHISemaphore* s : node.signals) {
                        SemaphoreSignaler[s] = &node;
                    }
                    PendingNodes.push_back(&node);
                }


                page->flags.store(RingCommandPool::IsEnd, std::memory_order_release);
            }

            for (size_t i = 0; i < SubmitOrder.size();) {
                auto* page = SubmitOrder[i];
				if (page->flags.load() & RingCommandPool::IsEnd) {
                    std::swap(SubmitOrder[i], SubmitOrder.back());
                    SubmitOrder.pop_back();
                    ResultCache.erase(page);
                    page->Pool->recycleQue(page);
                } else {
                    ++i;
                }
            }

            for (size_t i=0; i<PendingNodes.size(); ) {
                if (trysubmit(PendingNodes[i], trysubmit)) {
                    std::swap(PendingNodes[i], PendingNodes.back());
                    PendingNodes.pop_back();
                } else {
                    ++i;
                }
            }
            // 空闲时回收节点存储。
            //
            // 关键：**不能**在这里清空 SemaphoreSignaler。生产者的登记必须活到「等待它的
            // 消费者提交」为止 —— 应用先录 compute 页、后录 render 页，而 RHI 线程完全可能
            // 在 render 页还没录入时就把 compute 页处理并提交掉（此时 PendingNodes 与
            // ResultCache 都为空，恰好满足下面这个条件）。若此刻整表 clear()：
            //   1. computeDone[slot] 的所有权记录被抹掉；
            //   2. 下一轮才录入的 render 节点 trysubmit 查不到生产者 → 永远返回 false；
            //   3. render 永不提交 → finishFence 永不 setSubmited() → 主线程死等在
            //      RHIFence::waitFenceSubmited()（即「waitSubmitDone」），RHI 线程则在
            //      PendingNodes 上空转 100% CPU。
            // 这里只回收「所有权表已不再引用」的节点；表项指向的节点保持存活。
            // SubmitNodes 用 unique_ptr 持有，erase 不会让其它节点地址失效。
            if (PendingNodes.empty() && ResultCache.empty()) {
                for (auto it = SubmitNodes.begin(); it != SubmitNodes.end(); ) {
                    SubmitNode* n = it->get();
                    bool referenced = false;
                    for (RHISemaphore* s : n->signals) {
                        auto f = SemaphoreSignaler.find(s);
                        if (f != SemaphoreSignaler.end() && f->second == n) { referenced = true; break; }
                    }
                    if (referenced) ++it;
                    else            it = SubmitNodes.erase(it);
                }
            }

            // ── 本轮把所有能提交的都提交完之后，才替录制线程取图（顺序原因见循环开头）──
            // 取图（acquire）与提交/呈现同在本线程：VkSwapchainKHR 与 VkQueue 都是外部同步
            // 对象，分到两个线程就会报 THREADING ERROR。这里 vkAcquireNextImageKHR 用 timeout=0，
            // 且槽复用前的等待是有界的（见 VulkanSwapChain::tryAcquire）。
            for (auto& entry : mSwapChains) {
                if (VulkanSwapChain* swapchain = entry.load(std::memory_order_acquire)) {
                    swapchain->tryAcquire();
                }
            }
        }

        for (auto& [page, result] : ResultCache) {
            while (page->BatchQueue.pop()) {}
            page->flags.store(RingCommandPool::IsEnd, std::memory_order_release);
        }
        ResultCache.clear();
        SubmitOrder.clear();
        NeedSubmitQue.clear();
		SemaphoreSignaler.clear();
		PendingNodes.clear();

        Debug("RHI Loop drain complete");
    }


    void VulkanRHI::VulkanResourceLoop() {
        Debug("Resource Thread ID: 0x{:x}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
            
        std::vector<PendingReleaseInfo> PendingReleaseCBsInThread;

        PendingReleaseCBsInThread.clear();

        while ((!stopTag && !mDevice->isDeviceLost()) || !PendingReleaseCBs.empty() || !PendingReleaseCBsInThread.empty()) {
            //if (stopTag && !PendingReleaseCBsInThread.empty()) Debug("Suck");
            // Drain the lock-free queue into the local pending list.

            {
                PendingReleaseInfo Info;
                while (PendingReleaseCBs.pop(Info)) PendingReleaseCBsInThread.push_back(std::move(Info));
            }

            // Process the local list: release ready items, keep pending ones.
            if (!PendingReleaseCBsInThread.empty()) {
                size_t writeIdx = 0;
                for (size_t i = 0; i < PendingReleaseCBsInThread.size(); ++i) {
                    auto& [fence, cbs, fromframe, tsQuery] = PendingReleaseCBsInThread[i];

                    if (!fence || mFencePool->isSignaled(fence) || stopTag || mDevice->isDeviceLost()) {
                        // 围栏置位：先读回 GPU 时间戳，再回收命令缓冲。
                        // 不用 WAIT_BIT：查询结果随围栏置位应当已可用，若不可用则返回
                        // VK_NOT_READY（本次跳过），避免资源线程在此永久阻塞、拖垮后续回收。
                        if (tsQuery != UINT32_MAX && mTimestampQueryPool && mTimestampPeriod > 0.0f) {
                            uint64_t ts[2]{ 0, 0 };
                            if (vkGetQueryPoolResults(mDevice->getLogicalDevice(), mTimestampQueryPool,
                                    tsQuery, 2, sizeof(ts), ts, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                                if (ts[1] >= ts[0])
                                    mLastGpuTimeNs.store((uint64_t)((ts[1] - ts[0]) * mTimestampPeriod), std::memory_order_release);
                            }
                        }
                        for (auto& cb : cbs) {
                            cb.pool->releaseCommandBuffer(cb);
                            for (auto& [resource, change] : cb.QuoteResources) {
                                if (resource->getResourceType() == Type::Texture) {
                                    static_cast<VulkanTexture*>(resource)->transitionLayout(change.layout);
                                }
                            }
                            cb.QuoteResources.clear();
                        }
                        mFencePool->release(fence);
                    }
                    else {
                        // Keep this item for the next iteration.
                        if (writeIdx != i)
                            PendingReleaseCBsInThread[writeIdx] = std::move(PendingReleaseCBsInThread[i]);
                        ++writeIdx;
                    }
                }
                PendingReleaseCBsInThread.resize(writeIdx);
            }

            if (PendingReleaseCBs.empty() && PendingReleaseCBsInThread.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else if (!PendingReleaseCBsInThread.empty() && PendingReleaseCBs.empty()) {
                auto& front = PendingReleaseCBsInThread.front();
                if (front.fence) {
                    mFencePool->wait(front.fence, 1e6); // 最多等 1ms，围栏置位立即返回
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    }

    /*
        __VK_GLOBAL_FUNC__
    */
    double VulkanRHI::getLastGPUTimeMs() const {
        return static_cast<double>(mLastGpuTimeNs.load(std::memory_order_acquire)) * 1e-6;
    }

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
