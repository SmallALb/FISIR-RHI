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

	
	VulkanViewport::VulkanViewport(DynamicRHI* rhi, TextureCOLORType colorType, uint32_t iniWidth, uint32_t initHeight,
								   DisplayDeviceType displayType, void* deviceHandle,
								   uint32_t swapChainSlotCount) {
		mData = new __VKViewportData{};
		mData->Surface = VK_NULL_HANDLE;
		mData->Viewport.height = initHeight;
		mData->Viewport.width = iniWidth;
		mData->WindowHandle = nullptr;
		DisplayType = displayType;
		ImageColorType = colorType;
		SwapChainSlotCount = swapChainSlotCount ? swapChainSlotCount : DEFAULT_SWAPCHAIN_SLOT_COUNT;

		switch (displayType) {
		case DisplayDeviceType::Win32Window: {
			if (!deviceHandle) {
				Error("VulkanViewport: Win32Window 需要 Win32DisplayHandle{ hinstance, hwnd }，收到 nullptr");
				return;
			}
			auto* win32 = static_cast<FISIR::Win32DisplayHandle*>(deviceHandle);
			WindowHandle = { win32->hinstance, win32->hwnd };
			mData->WindowHandle = &WindowHandle;
			CreateWin32Surface();
			break;
		}
		case DisplayDeviceType::Headless:
			// 没有 surface ⇒ RHIGetSwapChain() 直接返回 nullptr（不建交换链），调用方渲染到离屏目标。
			// 这条路上 RHI 完全不依赖窗口系统，服务器/CI 也能跑。
			Info("[Vulkan] viewport 0x{:x} 是 Headless（无 surface / 无交换链），渲染目标由调用方自备",
				 (size_t)this);
			break;
		default:
			// 其余类型接口已定形、本后端尚未实现：给一个没有 surface 的视口，让调用方在
			// RHIGetSwapChain() 拿到 nullptr 时自己决定怎么退，而不是崩在空 surface 上。
			Error("[Vulkan] 呈现设备类型 {} 尚未在本后端实现（本视口没有 surface）",
				  DisplayDeviceTypeName(displayType));
			break;
		}
	}

	// 只为 Win32Window 设备建 surface。句柄布局 = Win32DisplayHandle{ hinstance, hwnd }
	//（即示例里原来的 win32Data 布局）。
	void VulkanViewport::CreateWin32Surface() {
	#ifdef _WIN32
		auto* winInstance = static_cast<Win32Data*>(mData->WindowHandle);
		vkWind32SurfaceCreate = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(GetGlobalInstance(), "vkCreateWin32SurfaceKHR"));
		if (!vkWind32SurfaceCreate) {
			Error("Vulkan Win32 Create Function create failed");
			return;
		}
		VkWin32SurfaceCreateInfoKHR win32SurfaceCreateInfo {
			.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			.hinstance = static_cast<HINSTANCE>(winInstance->hinstance),
			.hwnd = static_cast<HWND>(winInstance->hwnd)
		};

		if (vkWind32SurfaceCreate(GetGlobalInstance(), &win32SurfaceCreateInfo, nullptr, &mData->Surface) != VK_SUCCESS) {
			Error("Could Not Create Win32 Suraface");
			return;
		}

		Info("Create Win32 Vulkan Surface Successed!");
	#endif //  _WIN32
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

	bool VulkanViewport::hasSurface() const {
		return mData && mData->Surface != VK_NULL_HANDLE;
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
