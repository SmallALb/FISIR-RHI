#include "VulkanRHI.h"
#include <vector>
#include <cstdio>
#include <algorithm>
#include <vulkan/vulkan.h>
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
#include "../RHICommandList.h"
namespace FISIR {
    #include "ChangeImageFlagsToVulkanFlags.h"

	/*

		CommandPool Manager and Thread Context

    */
    #include "VulkanThreadCommandContext.h"
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

    static VulkanCommandPoolManager* CommandPoolManager;

    static LockFreeQue<PendingReleaseCBInfo> PendingReleaseCBs;
    static std::vector<PendingReleaseCBInfo> PendingReleaseCBsInThread;

    //Pending Upload Command Buffers
    LockFreeQue<CBInfo> CmdBufferNeedUpload;
    static std::mutex ContextCreateMutex;
    std::thread RHIThread, RHIResourceThread;

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

      for (auto& [name, pipeline] : PipelineMap) delete pipeline;

      for (auto& [info, renderPass] : RenderPassCache) delete renderPass;

	  Debug("Destroy Fence and Semaphore Pool");
	  mFencePool->destroyPool();
	  delete mFencePool;
	  delete mSemaphorePool;
      delete CommandPoolManager;

	  Debug("Destroy Device");
      mDevice->Destory();
      Debug("Vulkan RHI Destroyed!");

      DestroyDebugReportCallback();
  }

  bool VulkanRHI::Init() {
    if (!MakeVkInstance()) return false;
    Info("vulkan Instance Create Success!");
    if (!MakeDebugReportCallback()) return false;
    mDevice = new VulkanDevice(SelectDevice(gInstance));
    if (!mDevice->Init())  {
		Error("Failed to initialize Vulkan Device!");
        return false;
    }
	mFencePool = new VulkanFencePool(mDevice);
	mSemaphorePool = new VulkanSemaphorePool(mDevice);
	mDescriptorPool = new VulkanDescriptorPool(mDevice);
    CommandPoolManager = new VulkanCommandPoolManager(mDevice);

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

  RHIViewport* VulkanRHI::RHICreateViewport() {
      return nullptr;
  }

  RHIPipeline* VulkanRHI::RHICreatePipeline(const RHIPipelineState& PipelineState) {
      if (!PipelineCacheMap.contains(PipelineState)) {
          PipelineCacheMap[PipelineState] = new VulkanPipeline(mDevice, mDescriptorPool, PipelineState);
      }
      return PipelineCacheMap[PipelineState];
  }

  RHIContext* VulkanRHI::RHIGetContext(CmdType type) {
	  if (!TlsContext) {
		  std::lock_guard<std::mutex> lock(ContextCreateMutex);
          TlsContext = std::make_unique<ThreadContext>();
          TlsContext->init(mDevice, CommandPoolManager, mFencePool);
      }

      switch (type) {
      case CmdType::Render:
          return TlsContext->renderContext.get();
      case CmdType::Compute:
          return TlsContext->computeContext.get();
      case CmdType::Transfer:
          return TlsContext->transferContext.get();
      default:
          return nullptr;
	  }
  }


  RHIShader* VulkanRHI::RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) {
      Debug("Create Vulkan Shader");
      if (!Data) {
        Error("Shader Data is Empty!");
        return nullptr;
      }
      return new VulkanShader(mDevice, Data, size);
  }

  void VulkanRHI::RHISubmitCommandList(RHICommandListBase* cmdList) {
    auto ctx = cmdList->getContext();
    auto VkCtxptr = dynamic_cast<VulkanContextBase*>(ctx);
    CBInfo commandbufferinfo = VkCtxptr->getCommandBuffer();
	CmdBufferNeedUpload.push(commandbufferinfo);

  }

  RHIResourcePack* VulkanRHI::RHICreateResourcePack(Type restyp, const std::vector<RHIResource*>& resources) {
      return mDescriptorPool->createResourcePack(restyp, resources);
  }

  RHIRenderPass* VulkanRHI::RHICreateRenderPass(const RHIRenderPassInfo& info) {
      Debug("Getting Render Pass");
      if (RenderPassCache.find(info) != RenderPassCache.end()) {
          Debug("Using cached Render Pass");
          return RenderPassCache[info];
      }
      Debug("Creating new Render Pass");
      auto res = new VulkanRenderPass(mDevice, info);
      RenderPassCache[info] = res;
      return res;
  }

  void VulkanRHI::VulkanRHILoop() {
    Debug("RHI Thread ID: {}", std::this_thread::get_id());
    while (!stopTag || !CmdBufferNeedUpload.empty()) {
        std::vector<VkCommandBuffer> RenderCMDs, TransferCMDs, ComputeCMDs;
        std::vector<CBInfo> RenderCMDInfos, TransferCMDInfos, ComputeCMDInfos;

        if (!CmdBufferNeedUpload.empty()) {
            Debug("Buffers Input!");
            while (!CmdBufferNeedUpload.empty()) {
                CBInfo cmdBuffer;
                CmdBufferNeedUpload.pop(cmdBuffer);
                switch (cmdBuffer.poolType) {
                case CommandPoolType::_Graphics_:
                    RenderCMDs.push_back(cmdBuffer.buffer);
                    RenderCMDInfos.push_back(std::move(cmdBuffer));
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
        }              
        

        if (!RenderCMDs.empty()) {
			auto fence = mFencePool->createFence(false, "RenderFence");
            mDevice->submitCommandBuffer(RenderCMDs, CommandPoolType::_Graphics_, {}, {}, fence);
            PendingReleaseCBs.emplace(fence, std::move(RenderCMDInfos));
        }

	    //Create CBArray from CmdBufferNeedUpload


		CurrentFrame.store((CurrentFrame.load() + 1) % 3);
    }
  }

  //双指针移除算法
  //一个类似快排的算法，将符合条件的放在右边然后弹出
  //不符合的将其交换到最左边，然后左指针前移
  template<class T_, class F_> 
  bool TwoPointerSwapPop(std::vector<T_>& Target, F_ Judgefunc) {
    for (int l=0, r=Target.size()-1; l<=r; ) {
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

  void VulkanRHI::VulkanResourceLoop() {


    Debug("Resource Thread ID: {}", std::this_thread::get_id());
    while (!stopTag || !PendingReleaseCBs.empty() || !PendingReleaseCBsInThread.empty()) {


        TwoPointerSwapPop(PendingReleaseCBsInThread, [this](std::vector<PendingReleaseCBInfo>& target, int idx)->bool {
            auto& [fence, cbs] = target[idx];
            if (!fence || fence->isSignaled()) {
                if (fence) mFencePool->release(fence);
                return TwoPointerSwapPop(cbs, [this](std::vector<CBInfo>& infos, int idx2) -> bool {
                    if (!infos[idx2].QuoteResources.empty()) {
                        for (auto [resource, newVal] : infos[idx2].QuoteResources) {
                            if (resource->getResourceType() == Type::Texture) static_cast<VulkanTexture*>(resource)->transitionLayout(newVal.layout);
                        }
                        infos[idx2].QuoteResources.clear();
                    }
                    bool isUsed= !infos[idx2].pool->isPoolUsed() || stopTag;
                    if (isUsed) infos[idx2].pool->releaseCommandBuffer(infos[idx2]);
                    return isUsed;

                }) || stopTag;
            }
            return stopTag;
        });
       

        PendingReleaseCBInfo Info;
        if (!PendingReleaseCBs.empty()) {
			std::vector<CBInfo> cbInfos;
            PendingReleaseCBs.pop(Info);
            auto& [fence, cbs] = Info;
            if (!fence || fence->isSignaled()) {
                for (auto& cb : cbs) {
                    if (!cb.pool->isPoolUsed()) {
                        cb.pool->releaseCommandBuffer(cb);
                    }
                    else {
                        cbInfos.push_back(std::move(cb));
                    }
                }
                if (fence) mFencePool->release(fence);
                PendingReleaseCBsInThread.emplace_back(nullptr, std::move(cbInfos));
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
