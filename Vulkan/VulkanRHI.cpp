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
    struct PendingReleaseCBInfo {
        PendingReleaseCBInfo() {}
        PendingReleaseCBInfo(VulkanFence* f, std::vector<CBInfo>&& c, bool infence = 0) :
            fence(f), 
            cbInfos(std::move(c)), 
            inputFence(infence)
            {}
        PendingReleaseCBInfo(VulkanFence* f, const std::vector<CBInfo>& c) : fence(f), cbInfos(c) {}


        PendingReleaseCBInfo(PendingReleaseCBInfo&& other) noexcept
            : fence(other.fence), cbInfos(std::move(other.cbInfos)), inputFence(other.inputFence) {
            other.fence = nullptr;
        }

        PendingReleaseCBInfo& operator=(PendingReleaseCBInfo&& other) noexcept {
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
        bool inputFence {0};
    };

 
    static LockFreeQue<PendingReleaseCBInfo> PendingReleaseCBs;
    static std::vector<PendingReleaseCBInfo> PendingReleaseCBsInThread;
    static LockFreeQue<RingCommandPool::Page*> NeedUsingPages;

    //Pending Upload Command Buffers
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

    for (int i=0; i<3; i++) {
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
      return CmdMemoryPool[static_cast<int>(cmdtype)-1].acquireQue();
  }

  
  void VulkanRHI::RHIFlushAndWaitAfterCommand(CmdType cmdtype) {
    
  }

  void VulkanRHI::RHIDestroyFence(RHIFence* fence) {
      mFencePool->release(static_cast<VulkanFence*>(fence));
  }

  void VulkanRHI::RHISubmitPage(RingCommandPool::Page* page, RHIFence* fence, const std::vector<RHISemaphore*>& SignalSemaphores, const std::vector<RHISemaphore*>& WaitSemaphores) {
  
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
    Debug("RHI Thread ID: {}", std::this_thread::get_id());
    
    std::vector<SubmitPageTask> PendingReadDonePages;
    std::vector<CBInfo> Cbs;
    while(!stopTag) {
		TwoPointerSwapPop(PendingReadDonePages, [](SubmitPageTask& task)->bool {
			if () {
				return false;
			}
			else {
				SubmitPageTasks.push(task);
				return true;
			}
		}); 


		SubmitPageTask task;
        if (!SubmitPageTasks.pop(task)) {
			std::this_thread::yield();
            continue;
        }

        if (task.page->Flags.load(std::memory_order_acquire) & RingCommandPool::IsReading) { 
			PendingReadDonePages.push_back(task);
            std::this_thread::yield();
            continue;
        }

    }

    
  }


  void VulkanRHI::VulkanResourceLoop() {


    Debug("Resource Thread ID: {}", std::this_thread::get_id());
    while (!stopTag || !PendingReleaseCBs.empty() || !PendingReleaseCBsInThread.empty()) {
        static int shrink_counter = 0;
        if (++shrink_counter % 60 == 0 && PendingReleaseCBsInThread.empty()) {
            PendingReleaseCBsInThread.shrink_to_fit();
        }

        TwoPointerSwapPop(PendingReleaseCBsInThread, [this](PendingReleaseCBInfo& value)->bool {
            auto& [fence, cbs, inputFence] = value;

            if (!fence || fence->isSignaled()) {
                if (fence && !inputFence) mFencePool->release(static_cast<VulkanFence*>(fence));
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
            auto& [fence, cbs, fromframe] = Info;
            
            if (!fence || fence->isSignaled()) {
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
