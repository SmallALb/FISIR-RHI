#pragma once
#include <atomic>
#include "../RHISwapChain.h"
namespace FISIR {
	class VulkanDevice;
	class VulkanViewport;
	class VulkanFrameBuffer;
	class VulkanTexture;
	class VulkanRenderPass;
	class VulkanPipeline;
	class VulkanQueue;
	class DynamicRHI;

	class __VkSwapChainData;

	constexpr uint32_t SWAPCHAIN_SLOT_COUNT = 5;


	class VulkanSwapChain : public RHISwapChain{
	public:
		VulkanSwapChain(VulkanViewport* viewport, uint32_t QueFamilyIndex);

		bool init(VulkanDevice* device, DynamicRHI* rhi);

		~VulkanSwapChain();

		virtual uint32_t acquireGetImageInfoID() override;

		virtual void present(uint32_t infoid) override;

		virtual RHITexture* getSwapChainFrameTexture(uint32_t imageindex) const override;

		virtual uint32_t getImageCount() const override;

		virtual SwapChainGetImageInfo getSwapChainGetImageInfo(uint32_t id) override;

		virtual RHIFrameBuffer* getSwapChainFrameBuffer(uint32_t imageindex) override;

		virtual RHIPipeline* getSwapChainRenderPipeline() const override;

		virtual void enableTextureInput(RHITexture* texture, RHISampler* sampler) override;

		virtual void enableBufferInput(uint32_t width, uint32_t height, RHIBuffer* frameBuffer) override;

		virtual RHIResourcePackResult getSwapchainResourcePack() const override;

	private:
		bool recreateSwapChain();

		bool createSwapChian();

		bool createPipelineandRenderPass();

		// 重建呈现资源包：若已存在则先销毁再创建。binding0/1/2 三槽缺省时用内部占位资源填充，
		// binding3 始终是 BufferToOutPutData。包内资源顺序 = 管线 describeInfo 顺序（0..3）。
		void RebuildPresentPack();

		RHIBuffer*  EnsureFallbackBuffer();
		RHITexture* EnsureFallbackTexture();
		RHISampler* EnsureFallbackSampler();

		void UpdateOutputData(uint32_t width, uint32_t height, uint32_t bufferEnable);

		SwapChainGetImageInfo SwapChainFrameInfos[SWAPCHAIN_SLOT_COUNT];
		// Per-image present semaphores: indexed by swapchain image, not slot.
		RHISemaphore* ImageRenderFinish[MAX_SWAPCHAIN_FRAME]{};
		// Per-image present fences (VK_KHR_swapchain_maintenance1):
		// signaled by vkQueuePresentKHR when the image leaves the display engine.
		RHIFence*     PresentFence[MAX_SWAPCHAIN_FRAME]{};
		VulkanTexture* SwapChainTextures[MAX_SWAPCHAIN_FRAME];
		VulkanFrameBuffer* SwapChainFrameBuffers[MAX_SWAPCHAIN_FRAME];

		uint32_t MaxSwapChianFramCount{0};
		uint32_t CurrentFrameID{0};

		VulkanDevice* mDevice;

		VulkanRenderPass* VulkanSwapChainRednerPass;
		VulkanPipeline* VulkanViewportPipeline;

		// swapchain 自带的 BufferToOutPutData cbuffer 后备缓冲（UniformBuffer, b3）。
		// 默认 BufferEnable=0；enableBufferInput 置 viewport 并开启。
		RHIBuffer* BufferToOutPutData {nullptr};

		// ── 呈现输入（仅记录指针，不拥有）────────────
		RHIBuffer*  PresentBuffer  {nullptr};   // binding1 帧缓冲（buffer 模式）
		RHITexture* PresentTexture {nullptr};   // binding0 纹理（texture 模式）
		RHISampler* PresentSampler {nullptr};   // binding2 采样器
		bool        BufferEnabled  {false};     // 当前 cbuffer BufferEnable 状态

		// 当前呈现资源包（enable* 时销毁重建）。
		RHIResourcePackResult PresentResourcePack;

		// ── 内部占位资源（槽位缺省时使用，随 swapchain 销毁）──
		RHIBuffer*  FallbackBuffer  {nullptr};
		RHITexture* FallbackTexture {nullptr};
		RHISampler* FallbackSampler {nullptr};

		VulkanViewport* Surfaceviewport;

		uint32_t mPresentQueFamilyIndex{0};

		VulkanQueue* PresentQueue {nullptr};

		DynamicRHI* usingRHI;

		__VkSwapChainData* mData;

		std::atomic_bool needReBuildSwapChain {0};
	};


}