#pragma once

#include "../RHIViewport.h"

namespace FISIR {

	class VulkanDevice;
	struct __VKViewportData;
	
	class VullkanViewport : public RHIViewport {
	public:
		VullkanViewport();

		~VullkanViewport();

		virtual void* getNativeSwapChain() const override;

		virtual void* getNativeBackBufferTexture() const override;

		virtual void* getNativeWindow(void** handle) const override;

		virtual uint32_t getViewportWidth() const override;

		virtual uint32_t getViewportHeight() const override;

		virtual RHICustomPresent* getRHICustomPresent() const override;

		virtual void setRHICustomPresent(RHICustomPresent* present) override;

		virtual void tick(float deltatime) override;

		virtual void waitForFrameEventCompletion() override;

		virtual void IssueFrameEvent() override;

		virtual const char* outPutString() const override;

		VulkanDevice* mDevice{nullptr};
		__VKViewportData* mData {nullptr};
	};


}