#pragma once 

#include "RHIResource.h"

namespace FISIR {

	class RHICommandContext;

	class RHICustomPresent : public RHIResource {
	public:
		virtual void onBackCustomPresent() = 0;

		virtual bool needsNativePresent() = 0;

		virtual bool needsAdvanceBackbuffer() {return false;}

		virtual void beginDrawing() {}

		virtual bool pressent(RHICommandContext& RHICmdContext, int32_t& SyncInterval) {return true;}

		virtual void postPressent() {};

		virtual void onAcquireThreadOwnership() {}

		virtual void onReleaseThreadOwnership() {}
	};


	class RHIVierport : public RHIResource {
	public:
		
		virtual void* getNativeSwapChain() const {return nullptr;}

		virtual void* getNativeBackBufferTexture() const {return nullptr;}

		virtual void* getNativeWindow(void** handle) const {return nullptr;}

		virtual uint32_t getViewportWidth() const {return 0;}

		virtual uint32_t getViewportHeight() const {return 0;}

		virtual RHICustomPresent* getRHICustomPresent() const {return nullptr;}

		virtual void setRHICustomPresent(RHICustomPresent* present) {}

		virtual void tick(float deltatime) {}

		virtual void waitForFrameEventCompletion() {}

		virtual void IssueFrameEvent() {}
	

	};

}