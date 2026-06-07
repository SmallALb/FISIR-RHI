#include "VulkanRHI.h"
#include <vector>
#include <cstdio>
#include <algorithm>
#include <vulkan/vulkan.h>
#include <string>
#include "../../Log/Logger.h"
#include "../../DataBase/LockFreeQue.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanCommandContext.h"
#include "VulkanShader.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanRenderPass.h"
namespace FISIR {
    #include "ChangeImageFlagsToVulkanFlags.h"

   /*
       __VulkanData_AND_FUNC__
   */


	/*

		CommandPool Manager and Thread Context

    */
    #include "VulkanThreadCommandContext.h"
    static VulkanCommandPoolManager* CommandPoolManager;

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
      return new VulkanShader(mDevice, Data, size);
  }

  void VulkanRHI::RHISubmitCommandList(RHICommandListBase* cmdList) {
    auto ctx = cmdList->getContext();
    auto VkCtxptr = dynamic_cast<VulkanContextBase*>(ctx);
    CBInfo commandbufferinfo = VkCtxptr->getCommandBuffer();
	CmdBufferNeedUpload.push(commandbufferinfo);

  }

  RHIResourcePack* VulkanRHI::RHICreateResourcePack(Type restyp,std::initializer_list<RHIResource*> resources) {
      return mDescriptorPool->createResourcePack(restyp, resources);
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
                    RenderCMDInfos.push_back(cmdBuffer);
                    break;
                case CommandPoolType::_Transfer_:
                    TransferCMDs.push_back(cmdBuffer.buffer);
                    TransferCMDInfos.push_back(cmdBuffer);
                    break;
                case CommandPoolType::_Compute_:
                    ComputeCMDs.push_back(cmdBuffer.buffer);
                    ComputeCMDInfos.push_back(cmdBuffer);
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

  void VulkanRHI::VulkanResourceLoop() {
    Debug("Resource Thread ID: {}", std::this_thread::get_id());
    while (!stopTag || !PendingReleaseCBs.empty() || !PendingReleaseCBsInThread.empty()) {
        //快排思维清理
		//停止标志位停止后，强制清理所有命令缓冲区，不管fence状态
        if (!PendingReleaseCBsInThread.empty()) {
            for (int l=0, r=PendingReleaseCBsInThread.size()-1; l <= r; ) {
				auto& [fence, cbs] = PendingReleaseCBsInThread[r];
                bool isFree = 0;
                if (!fence || fence->isSignaled()) {
                    for (int cl = 0, cr = cbs.size() - 1; cl <= cr; ) {
                        if (!cbs[cr].pool->isPoolUsed() || stopTag) {
                            cbs[cr].pool->releaseCommandBuffer(cbs[cr]);
							cbs.pop_back();
                            cr--;
                        }
                        else if (cl < cr) {
							std::swap(cbs[cl], cbs[cr]);
                            cl++;
                        }
                        else break;
                    }
					isFree = cbs.empty();
                    if (fence) mFencePool->release(fence);
                }
                if (isFree || stopTag) {
                    PendingReleaseCBsInThread.pop_back();
                    r--;
                }
                else if (l < r) {
					std::swap(PendingReleaseCBsInThread[l], PendingReleaseCBsInThread[r]);
                    l++;
                }
                else break;
            }
        }

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
                        cbInfos.push_back(cb);
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
