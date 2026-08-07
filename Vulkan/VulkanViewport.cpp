#include "VulkanViewport.h"

#include <algorithm>

#include <vulkan/vulkan.h>

#include "../Log/Logger.h"
#include "../RHICommandList.h"
#include "../ShaderComplier.h"
#include "ChangeImageFlagsToVulkanFlags.h"
#include "VulkanDevice.h"
#include "VulkanFrameBuffer.h"
#include "VulkanRenderPass.h"
#include "VulkanRHI.h"
#include "VulkanShader.h"
#include "VulkanTexture.h"
namespace FISIR {
	
	extern VkInstance GetGlobalInstance();

	struct __VKViewportData {
		VkSurfaceKHR Surface;
		VkViewport Viewport;
		void * WindowHandle;
	};
#ifdef _WIN32
	static PFN_vkCreateWin32SurfaceKHR vkWind32SurfaceCreate = nullptr;
#endif //  _WIN32

	
	VulkanViewport::VulkanViewport(DynamicRHI* rhi, TextureCOLORType colorType, uint32_t iniWidth, uint32_t initHeight, void* WindowHandle) {
		mData = new __VKViewportData{};
		mData->Surface = VK_NULL_HANDLE;
		mData->Viewport.height = initHeight;
		mData->Viewport.width = iniWidth;
		mData->WindowHandle = WindowHandle;
		ImageColorType = colorType;

	//Create Win32 WindowSurface
	#ifdef _WIN32
		struct WindowHandles {
			HINSTANCE hInstance;
			HWND hwnd;
		};
		auto winInstance = static_cast<WindowHandles*>(mData->WindowHandle);
		vkWind32SurfaceCreate = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(GetGlobalInstance(), "vkCreateWin32SurfaceKHR"));
		if (!vkWind32SurfaceCreate) {
			Error("Vulkan Win32 Create Function create failed");
			return;
		}
		VkWin32SurfaceCreateInfoKHR win32SurfaceCreateInfo {
			.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			.hinstance = winInstance->hInstance,
			.hwnd = winInstance->hwnd
		};

		if (vkWind32SurfaceCreate(GetGlobalInstance(), &win32SurfaceCreateInfo, nullptr, &mData->Surface) != VK_SUCCESS) {
			Error("Could Not Create Win32 Suraface");
			return;
		}

		Info("Create Win32 Vulkan Surface Successed!");
	#endif // _WIN32


	}

	VulkanViewport::~VulkanViewport() {
		if (mData && mData->Surface) {
			vkDestroySurfaceKHR(GetGlobalInstance(), mData->Surface, nullptr);
		}
		delete mData;
	}

	void* VulkanViewport::getNativeWindow(void** handle) const {
		return mData->WindowHandle;
	}

	uint32_t VulkanViewport::getViewportWidth() const {
		return mData->Viewport.width;
	}

	uint32_t VulkanViewport::getViewportHeight() const {
		return mData->Viewport.height;
	}

	void VulkanViewport::setViewportResize(uint32_t height, uint32_t width) {
		mData->Viewport.width = height;
		mData->Viewport.height = width;

	}

		VkSurfaceKHR_T* VulkanViewport::getVkSurface() const {
		if (!mData) {
			Error("VulkanViewport::getVkSurface: mData is nullptr!");
			return nullptr;
		}
		return mData->Surface;
	}

	uint32_t VulkanViewport::getVulkanColorFormat() const {
		return getVulkanFormat(ImageColorType);
	}




}