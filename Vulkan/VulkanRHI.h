#pragma once

#include "../DynamicRHI.h"
#include "VulkanViewport.h"

#include "../DLLheader.h"
#include <array>
#include <mutex>
#include "VulkanCommandPool.h"
#include "VulkanFencePool.h"
#include "VulkanSemaphorePool.h"
#include "VulkanDescriptorPool.h"
#include <unordered_set>
#include <thread>
struct VkQueryPool_T;
namespace FISIR {
  class VulkanPipeline;
  class VulkanDevice;
  class VulkanCommandPool;
  class CommandExecuteThreadPool;
  class VulkanViewport;
  class VulkanSwapChain;
  class VulkanShader;
  class VulkanSampler;
  class VulkanRHI : public DynamicRHI {
	public:
		VulkanRHI();

		~VulkanRHI();

		virtual bool Init() override;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) override;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) override;

		virtual RHIViewport* RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type,
											   DisplayDeviceType deviceType, void* deviceHandle,
											   uint32_t swapChainSlotCount) override;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) override;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const char* EntryPoint, const unsigned char* Data, size_t size) override;

		virtual RHIResourcePackResult RHICreateResourcePack(const std::vector<RHIResource*>& resources) override;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) override;

		virtual RHIFrameBuffer* RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) override;

		virtual RHISemaphore* RHICreateSemaphore(const char* name, FenceType typ) override;

		virtual RHISwapChain* RHIGetSwapChain(RHIViewport* viewport) override;

		virtual void RHIDestroySemaphore(RHISemaphore* semaphore) override;

		virtual RHISampler* RHICreateSampler(const SamplerInfo& info) override;

		virtual RHIFence* RHICreateFence(bool signaled, const char* name) override;

		virtual RingCommandPool::Page* RHIGetCommandPoolPage(CmdType cmdtype) override;

		virtual void RHIDestroyFence(RHIFence* fence) override;

		virtual void RHIDestroyTexture(RHITexture* texture) override;

		virtual void RHIDestroyBuffer(RHIBuffer* buffer) override;

		virtual void RHIDestroySampler(RHISampler* sampler) override;

		virtual void RHIDestroyResourcePack(RHIResourcePackResult& pack) override;
		
		virtual void RHIDestroyFrameBuffer(RHIFrameBuffer* frameBuffer) override;

		virtual double getLastGPUTimeMs() const override;

		// ── 交换链注册表（取图 / 呈现全归 RHI 线程）──────────────────
		// 注册：RHIGetSwapChain（视口创建时，主线程）；注销：~VulkanSwapChain。
		// RHI 线程每轮循环开头对每个已登记交换链调 tryAcquire()，因此
		// vkAcquireNextImageKHR / vkQueuePresentKHR / 交换链重建 全程只由该线程执行。
		static constexpr size_t MaxSwapChainCount = 64;
		static void RegisterSwapChain(VulkanSwapChain* swapchain);
		static void UnregisterSwapChain(VulkanSwapChain* swapchain);
	private:
		

		void VulkanRHILoop();

		void VulkanResourceLoop();
	private:
		VulkanDevice* mDevice;
		VulkanFencePool* mFencePool;
		VulkanSemaphorePool* mSemaphorePool;
		VulkanDescriptorPool* mDescriptorPool;
		VulkanCommandPoolManager* mCmdPoolManager;
		std::atomic_bool stopTag {0};
		// 页面「录制序号」发号器：分配页面时自增，RHI 线程据此按录制顺序提交页面。
		std::atomic<uint64_t> mRecordSequence {0};
		RingCommandPool CmdMemoryPool[3];
		CommandExecuteThreadPool* ThreadPool;

		// GPU 时间戳查询：在每帧一级命令缓冲的首尾写时间戳，资源线程在围栏置位后读回，
		// 换算成 GPU 帧耗时，供上层估算 GPU 占用率。
		VkQueryPool_T* mTimestampQueryPool{ nullptr };
		float mTimestampPeriod{ 0.0f };
		std::atomic<uint64_t> mLastGpuTimeNs{ 0 };
		std::atomic<uint32_t> mTimestampRing{ 0 };
  };


}

extern "C" {
	EXPORTDLL FISIR::DynamicRHI* RHICreate();
	EXPORTDLL void RHIDestroy(FISIR::DynamicRHI* rhi);
}

