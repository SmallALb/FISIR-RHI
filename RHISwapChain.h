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

		virtual uint32_t acquireGetImageInfoID() = 0;

		virtual void present(uint32_t infoid) = 0;

		virtual RHITexture* getSwapChainFrameTexture(uint32_t imageindex) const = 0;

		virtual uint32_t getImageCount() const = 0;

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
