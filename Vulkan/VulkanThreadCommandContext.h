static std::thread::id MainID = std::thread::id();



class VulkanCommandPoolManager {
    std::vector<VulkanCommandPool*>& getPool(CommandPoolType type) {
        switch (type) {
        case _Graphics_:
            return RenderCommandPools;
        case _Compute_:
            return ComputeCommandPools;
        case _Transfer_:
            return TransferCommandPools;
        }
    }
    
    LockFreeQue<VulkanCommandPool*>& getQue(CommandPoolType type) {
        switch (type) {
        case _Graphics_:
            return FreeRenderCommandPools;
        case _Compute_:
            return FreeComputeCommandPools;
        case _Transfer_:
            return FreeTransferCommandPools;
        }
    }

    std::mutex& getMutex(CommandPoolType type) {
        switch (type) {
        case _Graphics_:
            return RenderCommandPoolsMTX;
        case _Compute_:
            return ComputeCommandPoolsMTX;
        case _Transfer_:
            return TransferCommandPoolsMTX;
        }
    }

public:
    VulkanCommandPoolManager(VulkanDevice* device) : mDevice(device) {
        RenderCommandPools.resize(10, nullptr);
        ComputeCommandPools.resize(10, nullptr);
        TransferCommandPools.resize(10, nullptr);

        for (auto& pool : RenderCommandPools) {
            pool = new VulkanCommandPool(mDevice, CommandPoolType::_Graphics_);
            FreeRenderCommandPools.push(pool);
        }

        for (auto& pool : ComputeCommandPools) {
            pool = new VulkanCommandPool(mDevice, CommandPoolType::_Compute_);
            FreeComputeCommandPools.push(pool);
        }

        for (auto& pool : TransferCommandPools) {
            pool = new VulkanCommandPool(mDevice, CommandPoolType::_Transfer_);
            FreeTransferCommandPools.push(pool);
        }
    }

    ~VulkanCommandPoolManager() {
        for (auto& pool : RenderCommandPools) {
            delete pool;
        }

        for (auto& pool : ComputeCommandPools) {
            delete pool;
        }

        for (auto& pool : TransferCommandPools) {
            delete pool;
        }
    
    }

    VulkanCommandPool* getCommandPool(CommandPoolType type) {
        auto& Pool = getPool(type);
        auto& Que = getQue(type);
        auto& Mtx = getMutex(type);

        VulkanCommandPool* res = nullptr;

        if (Que.pop(res)) return res;

        std::lock_guard<std::mutex> lock(Mtx);

        if (Que.pop(res)) return res;

        res = new VulkanCommandPool(mDevice, type);
        Pool.push_back(res);

        return res;
    }

    void reBackCommandPool(VulkanCommandPool* pool) {
        if (!pool) return;
        auto& Que = getQue(pool->mPoolType);
        Que.push(pool);
    }

private:
    std::vector<VulkanCommandPool*> RenderCommandPools, ComputeCommandPools, TransferCommandPools;
    std::mutex RenderCommandPoolsMTX, ComputeCommandPoolsMTX, TransferCommandPoolsMTX;
    LockFreeQue<VulkanCommandPool*> FreeRenderCommandPools, FreeComputeCommandPools, FreeTransferCommandPools;
    VulkanDevice* mDevice;
};





struct ThreadCommanPoolListener {
    ThreadCommanPoolListener(VulkanCommandPoolManager* manager, CommandPoolType type) {
        mManager = manager;
        mCommandPool = mManager->getCommandPool(type);
        if (mCommandPool) mCommandPool->UsedInThread.store(true, std::memory_order_release);
    }

    ~ThreadCommanPoolListener() {
        mCommandPool->UsedInThread.store(false, std::memory_order_release);
        mManager->reBackCommandPool(mCommandPool);
    }

    VulkanCommandPool* mCommandPool{ nullptr };
    VulkanCommandPoolManager* mManager {nullptr};
};

//Thread Context
struct ThreadContext {
    std::unique_ptr<ThreadCommanPoolListener> cmdRenderPoolListener{ nullptr };
    std::unique_ptr<ThreadCommanPoolListener> cmdComputePoolListener{ nullptr };
    std::unique_ptr<ThreadCommanPoolListener> cmdTransferPoolListener{ nullptr };

    std::unique_ptr<VulkanRenderContext> renderContext{ nullptr };
    std::unique_ptr<VulkanComputeContext> computeContext{ nullptr };
    std::unique_ptr<VulkanTransferContext> transferContext{ nullptr };

    void init(VulkanDevice* device, VulkanCommandPoolManager* manager, VulkanFencePool* fence) {
        VulkanCommandPool* cmdPool = nullptr;

        cmdRenderPoolListener.reset(new ThreadCommanPoolListener(manager, _Graphics_));
        renderContext.reset(new VulkanRenderContext(device, fence, cmdRenderPoolListener->mCommandPool));

        cmdComputePoolListener.reset(new ThreadCommanPoolListener(manager, _Compute_));
        computeContext.reset(new VulkanComputeContext(device, fence, cmdComputePoolListener->mCommandPool));

        cmdTransferPoolListener.reset(new ThreadCommanPoolListener(manager, _Transfer_));
        transferContext.reset(new VulkanTransferContext(device, fence, cmdTransferPoolListener->mCommandPool));

    }


    bool initialized() const {
        return renderContext && computeContext && transferContext;
    }

};
static thread_local std::unique_ptr<ThreadContext> TlsContext;

//Pending Release Command Buffers
struct PendingReleaseCBInfo {
    PendingReleaseCBInfo() {}
    PendingReleaseCBInfo(VulkanFence* f, std::vector<CBInfo>&& c) : fence(f), cbInfos(std::move(c)) {}
    PendingReleaseCBInfo(VulkanFence* f, const std::vector<CBInfo>& c) : fence(f), cbInfos(c) {}


    PendingReleaseCBInfo(PendingReleaseCBInfo&& other) noexcept
        : fence(other.fence), cbInfos(std::move(other.cbInfos)) {
        other.fence = nullptr;
    }

    PendingReleaseCBInfo& operator=(PendingReleaseCBInfo&& other) noexcept {
        if (this != &other) {
            fence = other.fence;
            cbInfos = std::move(other.cbInfos);
            other.fence = nullptr;
        }
        return *this;
    }

    VulkanFence* fence{ nullptr };
    std::vector<CBInfo> cbInfos{};
};
static LockFreeQue<PendingReleaseCBInfo> PendingReleaseCBs;
static std::vector<PendingReleaseCBInfo> PendingReleaseCBsInThread;

//Pending Upload Command Buffers
LockFreeQue<CBInfo> CmdBufferNeedUpload;
static std::mutex ContextCreateMutex;
std::thread RHIThread, RHIResourceThread;