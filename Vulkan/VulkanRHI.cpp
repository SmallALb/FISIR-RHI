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
#include "VulkanCommandContext.h"
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
    struct PendingReleaseCBInfo {
        PendingReleaseCBInfo() {}
        PendingReleaseCBInfo(RHIFence* f, std::vector<CBInfo>&& c, std::vector<RHIContext*>&& ctxs, std::vector<std::atomic_bool*>&& gpuDoneTags, bool infence = 0) :
            fence(f), 
            cbInfos(std::move(c)), 
            inputFence(infence),
            Ctxs(std::move(ctxs)),
            GpuDoneTags(std::move(gpuDoneTags))
            {}
        PendingReleaseCBInfo(RHIFence* f, const std::vector<CBInfo>& c) : fence(f), cbInfos(c) {}


        PendingReleaseCBInfo(PendingReleaseCBInfo&& other) noexcept
            : fence(other.fence), cbInfos(std::move(other.cbInfos)), Ctxs(std::move(other.Ctxs)), inputFence(other.inputFence), GpuDoneTags(std::move(other.GpuDoneTags)){
            other.fence = nullptr;
        }

        PendingReleaseCBInfo& operator=(PendingReleaseCBInfo&& other) noexcept {
            if (this != &other) {
                fence = other.fence;
                cbInfos = std::move(other.cbInfos);
                inputFence = other.inputFence;
                Ctxs = std::move(other.Ctxs);              
                GpuDoneTags = std::move(other.GpuDoneTags); 
                other.fence = nullptr;
            }
            return *this;
        }

        RHIFence* fence{ nullptr };
        std::vector<CBInfo> cbInfos{};
        bool inputFence {0};
        std::vector<RHIContext*> Ctxs;
        std::vector<std::atomic_bool*> GpuDoneTags;
    };


    static LockFreeQue<PendingReleaseCBInfo> PendingReleaseCBs;
    static std::vector<PendingReleaseCBInfo> PendingReleaseCBsInThread;

    //Pending Upload Command Buffers
    struct ExecuteTask {
        RHIContext* Ctx {nullptr};
        std::vector<RHISemaphore*> waitSeams {};
        std::vector<RHISemaphore*> singalSeams {};
        RHIFence* fence {nullptr};
        std::atomic_bool* submitTag {nullptr};
        std::atomic_bool* GpuDoneTag{ nullptr };

    };
    LockFreeQue<ExecuteTask> CmdListNeedExecute;
    static std::mutex ContextCreateMutex;
    std::thread RHIThread, RHIResourceThread;
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

      RHIThread.join();
      Debug("VulkanRHI Thread Join");

	  RHIResourceThread.join();
	  Debug("VulkanResource Thread Join");

      vkDeviceWaitIdle(mDevice->getLogicalDevice());

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
    if (!mDevice->Init(ViewPortsPool, ViewPortSwapChainCache))  {
		Error("Failed to initialize Vulkan Device!");
        return false;
    }
	mFencePool = new VulkanFencePool(mDevice);
	mSemaphorePool = new VulkanSemaphorePool(mDevice);
	mDescriptorPool = new VulkanDescriptorPool(mDevice);
    
    mCmdPoolManager = new VulkanCommandPoolManager(mDevice);

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

  static RHIContext* GetContext(VulkanRHI* rhi, VulkanDevice* device, CmdType type) {
      switch (type) {
      case CmdType::Render:
          return new VulkanRenderContext(rhi, device);
      case CmdType::Compute:
          return new VulkanComputeContext(rhi, device);
      case CmdType::Transfer:
          return new VulkanTransferContext(rhi, device);
      default:
          Error("Can Not Find the Type Of This Context!");
          return nullptr;
	  }
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

  void VulkanRHI::RHISubmitCommandList(RHICommandListBase* cmdList, RHIFence* fence, const std::vector<RHISemaphore*>& waitSemaphore, const std::vector<RHISemaphore*>& singalSemaphore, std::atomic_bool* submitTag, std::atomic_bool* gpuDoneTag) {
    if (cmdList->DontExecuteAndSubmit)  {
        if (auto ctx = cmdList->getContext()) {
            delete ctx;
            cmdList->setContext(nullptr);
        }
        Warn("CmdList Drop");
        return;
    }

    if (!cmdList->Executed) {
        if (!cmdList->getContext()) RHICreateContext(cmdList);
        cmdList->ExectueList();
    }
    Info("Submit Cmd To Queue");
    ExecuteTask task{cmdList->getContext(), waitSemaphore, singalSemaphore, fence, submitTag, gpuDoneTag};
    CmdListNeedExecute.push(task);
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

  void VulkanRHI::RHIFlushAndWaitAfterCommand(CmdType cmdtype) {
    RHICommandListBase* cmdList = nullptr;
    switch(cmdtype) {
        case CmdType::Render:
            cmdList = new RHIRenderCommandList(); break;
        case CmdType::Transfer:
            cmdList = new RHIRenderCommandList(); break;
        case CmdType::Compute:
            cmdList = new RHIRenderCommandList(); break;
    }

    if (!cmdList) return;
    std::atomic_bool tag = 0;
    RHICreateContext(cmdList);
    cmdList->ExectueList();
    ExecuteTask task {
        .Ctx = cmdList->getContext(),
        .GpuDoneTag = &tag,
    };
    CmdListNeedExecute.push(task);
        
    Warn("Waitting after Command Done...");
    
    
    while(!tag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    
    Warn("after Command had been Done");

    delete cmdList;
  }

  void VulkanRHI::RHICreateContext(RHICommandListBase* cmdlist) {
    cmdlist->setContext(GetContext(this, mDevice, cmdlist->getCommandListType()));
  }

  void VulkanRHI::RHIDestroyFence(RHIFence* fence) {
      mFencePool->release(static_cast<VulkanFence*>(fence));
  }

  ThreadCommanPoolListener* VulkanRHI::choiceCommandPool(CmdType type) {
      CommandPoolType typ;

      switch(type) {
        case CmdType::Render :typ = _Graphics_; break;
        case CmdType::Transfer:typ = _Transfer_; break;
        case CmdType::Compute:typ = _Compute_; break;
      }
      return new ThreadCommanPoolListener(mCmdPoolManager, typ);
  }

  ThreadCommanPoolListener* VulkanRHI::choiceCommandPool(uint32_t FamilyIndex) {
      return new ThreadCommanPoolListener(mCmdPoolManager, _Presnet_ , FamilyIndex);
  }

  

  template<class T_, class F_>
  bool TwoPointerSwapPop(std::vector<T_>& Target, F_ Judgefunc) {
      for (int l = 0, r = Target.size() - 1; l <= r; ) {
          if (Judgefunc(Target, r)) {
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
    Debug("RHI Thread ID: {}", std::this_thread::get_id());
    
    std::vector<VkCommandBuffer> RenderCMDs, TransferCMDs, ComputeCMDs;
    std::vector<CBInfo> RenderCMDInfos, TransferCMDInfos, ComputeCMDInfos;
    std::vector<RHIContext*> ctxs; std::vector<std::atomic_bool*> submitTags, gpuDoneTags;
    
    while (!stopTag || !CmdListNeedExecute.empty()) {
        RenderCMDs.clear(), TransferCMDs.clear(), ComputeCMDs.clear();
        RenderCMDInfos.clear(), TransferCMDInfos.clear(), ComputeCMDInfos.clear();
        ctxs.clear(); submitTags.clear(); gpuDoneTags.clear();
        while (!CmdListNeedExecute.empty()) {
            ExecuteTask task = {};
            if (!CmdListNeedExecute.pop(task)) continue;
            auto& [context, waitSems, singalSems, fence, submitTag, gpuDoneTag] = task;
            
            if (!context) {
                Error("Context null");
                continue;
            }
            auto vkctx = context->as<VulkanContextBase>();

            CBInfo cmdBuffer = vkctx->getBackCBInfo();
            

            switch (cmdBuffer.poolType) {
            case CommandPoolType::_Graphics_:
                if (!fence){
                    RenderCMDs.push_back(cmdBuffer.buffer);
                    RenderCMDInfos.push_back(std::move(cmdBuffer));
                    if (submitTag) submitTags.push_back(submitTag);
                    if (gpuDoneTag) gpuDoneTags.push_back(gpuDoneTag);
                    ctxs.push_back(context);
                }
                else {
                    mDevice->submitCommandBuffer(
                        { cmdBuffer.buffer }, 
                        CommandPoolType::_Graphics_, 
                        !singalSems.empty() ? singalSems : std::vector<RHISemaphore*>{},
                        !waitSems.empty() ? waitSems : std::vector<RHISemaphore*>{},
                        fence
                    );
                    Debug("Emplace to PendingReleaseCBs with fence: 0x{:x}, gpuDoneTag: 0x{:x}",
                        (size_t)fence, (size_t)gpuDoneTag);
                    PendingReleaseCBs.emplace(
                        fence, 
                        std::vector{ std::move(cmdBuffer) }, 
                        std::vector{ context },  
                        gpuDoneTag ? std::vector{gpuDoneTag} : std::vector<std::atomic_bool*>{},
                        1
                    );
                    if (submitTag) submitTag->store(1, std::memory_order_release);
                }
                break;
            case CommandPoolType::_Transfer_:
                TransferCMDs.push_back(cmdBuffer.buffer);
                TransferCMDInfos.push_back(std::move(cmdBuffer));
                break;
            case CommandPoolType::_Compute_:
                ComputeCMDs.push_back(cmdBuffer.buffer);
                ComputeCMDInfos.push_back(std::move(cmdBuffer));
                break;
            }
            
        }

        if (!RenderCMDs.empty()) {
			auto fence = mFencePool->createFence(false, "RenderFence");
            mDevice->submitCommandBuffer(RenderCMDs, CommandPoolType::_Graphics_, {}, {}, fence);
            Debug("Emplace to PendingReleaseCBs with fence: 0x{:x}, gpuDoneTags count: {}",
                (size_t)fence, gpuDoneTags.size());
            PendingReleaseCBs.emplace(fence, std::move(RenderCMDInfos), std::move(ctxs), std::move(gpuDoneTags));
            for (auto& tag : submitTags) tag->store(1, std::memory_order_release);
        }


	    //Create CBArray from CmdBufferNeedUpload


    }
  }


  void VulkanRHI::VulkanResourceLoop() {


    Debug("Resource Thread ID: {}", std::this_thread::get_id());
    while (!stopTag || !PendingReleaseCBs.empty() || !PendingReleaseCBsInThread.empty()) {
        static int shrink_counter = 0;
        if (++shrink_counter % 60 == 0 && PendingReleaseCBsInThread.empty()) {
            PendingReleaseCBsInThread.shrink_to_fit();
        }

        TwoPointerSwapPop(PendingReleaseCBsInThread, [this](std::vector<PendingReleaseCBInfo>& target, int idx)->bool {
            auto& [fence, cbs, fromframe, ctxs, gpuDoneTags] = target[idx];
            Debug("Thread Info: fence=0x{:x}, gpuDoneTags.size={}", (size_t)fence, gpuDoneTags.size());

            if (!fence || fence->isSignaled()) {
                for (auto& gpuDoneTag : gpuDoneTags) gpuDoneTag->store(1, std::memory_order_release), Debug("gpuDoneTag set to 1 In ResourceThreadVector");
                if (fence && !fromframe) mFencePool->release(static_cast<VulkanFence*>(fence));
                for (auto ctx : ctxs) delete ctx;
                for (auto& cb : cbs) {
                    cb.pool->releaseCommandBuffer(cb);
                    for (auto& [resource, change] : cb.QuoteResources) {
                        if (resource->getResourceType() == Type::Texture) {
                            static_cast<VulkanTexture*>(resource)->transitionLayout(change.layout);
                        }
                    }
                }
                cbs.clear();
                return true;
            }
            return stopTag;
        });
       

        PendingReleaseCBInfo Info;
        if (!PendingReleaseCBs.empty()) {
			std::vector<CBInfo> cbInfos;
            if (!PendingReleaseCBs.pop(Info)) {
                Warn("PendingReleaseCBs Pop Failed");
                continue;
            }
            auto& [fence, cbs, fromframe, ctxs, gpuDoneTags] = Info;
            
            if (!fence || fence->isSignaled()) {
                for (auto& gpuDoneTag : gpuDoneTags) gpuDoneTag->store(1, std::memory_order_release);
                for (auto& cb : cbs) {
                    cb.pool->releaseCommandBuffer(cb);
                    for (auto& [resource, change] : cb.QuoteResources) {
                        if (resource->getResourceType() == Type::Texture) {
                            static_cast<VulkanTexture*>(resource)->transitionLayout(change.layout);
                        }
                    }

                }
                if (fence && !fromframe) mFencePool->release(static_cast<VulkanFence*>(fence));
            }
            else {
                PendingReleaseCBsInThread.push_back(std::move(Info));
            }
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
