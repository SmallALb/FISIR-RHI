#pragma once

#include <cstdint>

#include "RHITypes.h"

namespace FISIR {
	class RHIFrameBuffer;
	class RHIFence;
	class RHISemaphore;
	class RHICommandListBase;
	class RHITexture;
	class RHIPipeline;
	class RHIBuffer;
	class RHISampler;

	constexpr int MAX_SWAPCHAIN_FRAME = 3;

	// 默认槽位数（帧在飞数）。槽位由创建 viewport 时指定（RHICreateViewport 的最后一个参数），
	// 运行期实际生效值用 getSlotCount() 查询（可能因「不得超过交换链图像数」被夹取）。
	// 硬约束：槽位数 ≤ 交换链图像数 —— 否则槽复用周期短于图像复用周期，会出现
	// 「同一 present 信号量在上一帧 present 尚未消费时被再次 signal」的竞态（曾导致渲染卡死），
	// 见 .claude/Results/2026-08-30-bunnypbr-卡死-帧在飞数超图像数-信号量复用竞态-fix.md。
	constexpr uint32_t DEFAULT_SWAPCHAIN_SLOT_COUNT = 3;

	struct SwapChainGetImageInfo {
		RHISemaphore* avaliable{ nullptr };
		RHISemaphore* renderFinish{ nullptr };
		RHIFence* finishFence{ nullptr };
		uint32_t imageIndex { UINT32_MAX };
	};


	class RHISwapChain {
	public:
		inline static uint32_t FAILEID = UINT32_MAX;

		virtual ~RHISwapChain() {}

		// ── 取图 / 呈现：全归 RHI 线程 ────────────────────────────────
		// VkSwapchainKHR 是外部同步对象：vkAcquireNextImageKHR、vkQueuePresentKHR 与交换链重建
		// 必须始终在同一线程上串行。因此这里只留「取图」能力，且拆成两半：
		//   主线程：置位请求，自旋等 RHI 取回 ImageIndex（**只有主线程阻塞**，且有自旋上限）；
		//   RHI 线程：非阻塞轮询，只有被请求时才 vkAcquireNextImageKHR(timeout=0)，
		//             取到就把 ImageIndex 交给主线程（见 VulkanSwapChain::tryAcquire）。
		// 呈现不再对外暴露：录制期用 RHICommandListBase::Present(swapchain, frameID) 录成指令，
		// 由 RHI 线程在本页 vkQueueSubmit 之后执行 vkQueuePresentKHR。
		virtual uint32_t acquireGetImageInfoID() = 0;

		virtual void tryAcquire() = 0;

		virtual RHITexture* getSwapChainFrameTexture(uint32_t imageindex) const = 0;

		virtual uint32_t getImageCount() const = 0;

		// 本次交换链实际启用的槽位数（帧在飞数）：acquireGetImageInfoID() 返回的 id 恒 < 该值，
		// 应用侧所有「每槽一份」的资源（状态缓冲 / 信号量 / 资源包 …）都应按它来分配。
		virtual uint32_t getSlotCount() const = 0;

		// ── 垂直同步 ────────────────────────────────────────────────
		// sync(true) ：切到 FIFO 呈现模式（垂直同步，帧率由显示器刷新率锁定）；
		// sync(false)：切回无同步模式（优先 MAILBOX，其次 IMMEDIATE，都不可用时退回 FIFO）。
		// 呈现模式在 vkCreateSwapchainKHR 时固定，无法原地修改，故本调用只登记目标模式并
		// 请求重建交换链，真正的重建发生在下一次 acquireGetImageInfoID()（重建前 vkDeviceWaitIdle），
		// 因此可以在帧与帧之间安全调用（无需停止录制/提交）。
		virtual void sync(bool enable = true) = 0;

		// 当前请求的垂直同步状态（true = 目标为 FIFO）。注意：若设备不支持 MAILBOX/IMMEDIATE，
		// sync(false) 也会退回 FIFO，实际模式以日志中的 present mode 为准。
		virtual bool isSyncEnabled() const = 0;

		virtual SwapChainGetImageInfo getSwapChainGetImageInfo(uint32_t id) = 0;

		virtual RHIFrameBuffer* getSwapChainFrameBuffer(uint32_t imageindex) = 0;

		virtual RHIPipeline* getSwapChainRenderPipeline() const = 0;

		// ── 呈现输入 ────────────────────────────────────────────────
		// swapchain PS 的固定 4 绑定（与描述符顺序一致）：
		// binding0=SamplerImage(g_OffscreenTexture, t0)、binding1=RBuffer(g_FrameBuffer, t1)、
		// binding2=Sampler(g_LinearSampler, s2)、binding3=UniformBuffer(BufferToOutPutData, b3)。
		// 这两函数都只做一件事：重建呈现资源包（若先前已有则销毁再创建），并同步 cbuffer 的 BufferEnable。

		// 纹理呈现模式：绑定离屏纹理，BufferEnable=0（PS 采样纹理）。binding1 槽位用内部占位缓冲填充。
		virtual void enableTextureInput(RHITexture* texture, RHISampler* sampler) = 0;

		// 缓冲呈现模式：把 frameBuffer 绑到 binding1 读取，BufferEnable=1（PS 读缓冲而非纹理），
		// 同时把 cbuffer 的 viewport 置为 (width, height)。binding0/binding2 缺省时用内部占位纹理/采样器。
		virtual void enableBufferInput(uint32_t width, uint32_t height, RHIBuffer* frameBuffer) = 0;

		// 当前呈现资源包（由 enableTextureInput / enableBufferInput 生成），供 SetResourcePack 绑定。
		virtual RHIResourcePackResult getSwapchainResourcePack() const = 0;

	};



}
