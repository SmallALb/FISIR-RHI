#include "VulkanViewport.h"
#include "VulkanDevice.h"
#include <vulkan/vulkan.h>
namespace FISIR {
	
	struct __VKViewportData {
		VkSurfaceKHR mSurface;
		VkViewport mViewport;
		VkSwapchainKHR mSwapChain;
	};

	VullkanViewport::VullkanViewport() {
		mData = new __VKViewportData;

	}

	VullkanViewport::~VullkanViewport() {
		delete mData;
	}

	void* VullkanViewport::getNativeSwapChain() const {
		return nullptr;
	}

	void* VullkanViewport::getNativeBackBufferTexture() const {
		return nullptr;
	}

	void* VullkanViewport::getNativeWindow(void** handle) const {
		return nullptr;
	}

	uint32_t VullkanViewport::getViewportWidth() const {
		return mData->mViewport.width;
	}

	uint32_t VullkanViewport::getViewportHeight() const {
		return mData->mViewport.height;
	}

	RHICustomPresent* VullkanViewport::getRHICustomPresent() const {
		return nullptr;
	}

	void VullkanViewport::setRHICustomPresent(RHICustomPresent* present) {
	}

	void VullkanViewport::tick(float deltatime) {

	}

	void VullkanViewport::waitForFrameEventCompletion() {

	}

	void VullkanViewport::IssueFrameEvent() {
	}

	const char* VullkanViewport::outPutString() const {
		return "Vulka Viewport";
	}

}