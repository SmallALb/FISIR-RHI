#pragma once
#include <atomic>
#include <vector>
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

	class VulkanSwapChain : public RHISwapChain{
		// presentNow / tryAcquire 只给 RHI 线程用（VkSwapchainKHR 必须单线程触碰）。
		friend class VulkanRHI;
	public:
		// 复位「呈现通道着色器」这组进程级静态（VulkanSwapChian.cpp 里的 VShader/FShader）。
		// 设备销毁时必须调用：否则窗口被回收后重建时，这里会复用上一代已被销毁的
		// RHIShader*，管线创建时解引用野指针直接崩（Android 切后台回来就是这个栈）。
		static void ResetPresentShaders();

		// slotCount：用户指定的槽位数（帧在飞数）。createSwapChian() 会把它夹取到
		// 「≥1 且 ≤ 交换链图像数」——超出图像数会重现「帧在飞数 > 图像数」的信号量复用
		// 竞态（曾导致渲染卡死，见 RHISwapChain.h 中 DEFAULT_SWAPCHAIN_SLOT_COUNT 的说明）。
		VulkanSwapChain(VulkanViewport* viewport, uint32_t QueFamilyIndex,
						uint32_t slotCount = DEFAULT_SWAPCHAIN_SLOT_COUNT);

		bool init(VulkanDevice* device, DynamicRHI* rhi);

		~VulkanSwapChain();

		// 主线程：置位取图请求并自旋等结果；RHI 线程取回 ImageIndex 前不返回（见 .cpp）。
		virtual uint32_t acquireGetImageInfoID() override;

		// RHI 线程：每轮轮询。只有主线程置位了请求才动交换链；vkAcquireNextImageKHR 用
		// timeout=0 绝不等待，取不到就留到下一轮。
		virtual void tryAcquire() override;

		virtual RHITexture* getSwapChainFrameTexture(uint32_t imageindex) const override;

		virtual uint32_t getImageCount() const override;

		// 有效槽位数（帧在飞数）：由构造参数夹取「≤ 图像数」后的结果，
		// acquireGetImageInfoID() 返回的 id 恒 < 该值。
		virtual uint32_t getSlotCount() const override { return SlotCount; }

		// 垂直同步：true → FIFO 呈现模式，false → 优先 MAILBOX / IMMEDIATE。
		// 呈现模式在交换链创建时固定，故这里登记目标模式并请求重建，重建在下一次
		// acquireGetImageInfoID() 里完成（先 vkDeviceWaitIdle）。
		virtual void sync(bool enable = true) override;

		virtual bool isSyncEnabled() const override { return SyncEnabled.load(std::memory_order_acquire); }

		virtual SwapChainGetImageInfo getSwapChainGetImageInfo(uint32_t id) override;

		virtual RHIFrameBuffer* getSwapChainFrameBuffer(uint32_t imageindex) override;

		virtual RHIPipeline* getSwapChainRenderPipeline() const override;

		virtual void enableTextureInput(RHITexture* texture, RHISampler* sampler) override;

		virtual void enableBufferInput(uint32_t width, uint32_t height, RHIBuffer* frameBuffer) override;

		virtual RHIResourcePackResult getSwapchainResourcePack() const override;

	private:
		// 真正执行 vkQueuePresentKHR。**只能由 RHI 线程调用**（friend VulkanRHI）——
		// 它与 vkQueueSubmit / vkAcquireNextImageKHR / 交换链重建共用同一线程。
		void presentNow(uint32_t infoid);

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

		// 槽位信息环：大小 == SlotCount（每次重建时按夹取后的槽位数调整）。
		std::vector<SwapChainGetImageInfo> SwapChainFrameInfos;
		// 每图像资源：大小 == MaxSwapChianFramCount（交换链实际图像数，与槽位数无关）。
		// 图像索引来自 acquire，present 等待的正是这里的每图像信号量。
		std::vector<RHISemaphore*>       ImageRenderFinish;
		// 每图像 present 围栏（VK_KHR_swapchain_maintenance1）：
		// 由 vkQueuePresentKHR 在图像离开显示引擎时置位。
		std::vector<RHIFence*>           PresentFence;
		std::vector<VulkanTexture*>      SwapChainTextures;
		std::vector<VulkanFrameBuffer*>  SwapChainFrameBuffers;

		uint32_t SlotCount{DEFAULT_SWAPCHAIN_SLOT_COUNT};            // 有效槽位数（夹取后）
		uint32_t RequestedSlotCount{DEFAULT_SWAPCHAIN_SLOT_COUNT};   // 构造时用户请求的槽位数
		std::atomic_bool SyncEnabled{false};                         // 垂直同步请求（FIFO）

		uint32_t MaxSwapChianFramCount{0};
		uint32_t CurrentFrameID{0};

		// ── 取图握手（主线程 ↔ RHI 线程）────────────────────────────
		// 0 = 空闲；1 = 主线程已请求；2 = RHI 已取回（mAcquiredFrameID 有效，含 FAILEID）。
		// 主线程 0→1 置位后用**原子等待**（std::atomic::wait）睡在这里，RHI 线程 1→2 后
		// notify 唤醒：不加锁、不烧 CPU（自旋实现每帧能空转上百毫秒，上层拿到 FAILEID 就
		// continue，整帧的 ImGui::NewFrame / UpdatePlatformWindows 都不跑 —— 拖拽缩放直接失效）。
		// mAcquireDeadlineNs：请求的兜底时刻，由主线程写入。原子等待没有超时版本，于是把
		// 「不能死等」放到 RHI 侧：轮询时发现已过该时刻就回一个 FAILEID，让上层继续跑
		//（最小化窗口、驱动一直 VK_TIMEOUT 这类情况都会走到）。
		std::atomic<int>         mAcquire{0};
		uint32_t                 mAcquiredFrameID{RHISwapChain::FAILEID};   // 仅在 mAcquire==2 时有效
		std::atomic<uint64_t>    mAcquireDeadlineNs{0};
		static constexpr uint64_t kAcquireGiveUpNs = 1'000'000'000ull;      // 1s（兜底窗口，可调）

		// RHI 线程专用：置取图结果并唤醒主线程（tryAcquire 的所有 1→2 都走这里）。
		void publishAcquire(uint32_t frameID);

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