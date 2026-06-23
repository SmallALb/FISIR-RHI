#pragma once

#include <cstdint>

namespace FISIR {
	class RHIFrameBuffer;
	class RHIFence;
	class RHISemaphore;
	class RHICommandListBase;
	class RHITexture;
	class RHIPipeline;

	constexpr int MAX_SWAPCHAIN_FRAME = 3;

	struct SwapChainGetImageInfo {
		RHIFence* frameFence {nullptr};
		RHISemaphore* avaliable{ nullptr };
		RHISemaphore* renderFinish{ nullptr };
		uint32_t imageIndex {0};
		bool inUse = false;
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
	};



}