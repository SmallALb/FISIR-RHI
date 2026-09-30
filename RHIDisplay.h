#pragma once

// 呈现设备（display device）抽象 —— 「像素最终上到哪儿」这一层的最小描述。
//
// 它只回答两件事：**用哪一类设备**（DisplayDeviceType）和**对应的句柄长什么样**（各 *DisplayHandle）。
// 句柄一律用 void* 表达，RHI 头因此保持平台无关；真身在注释里写清楚。布局一经发布就不再改：
// RHI 是运行时加载的 DLL（RHIVK.dll），这个枚举与结构体等于跨模块 ABI。
//
// 与「渲染目标」的区别：本文件只管**上屏/上设备**那条路（RHIViewport + RHISwapChain）。
// 画到纹理上与设备无关（RHITexture + RHIFrameBuffer + RHIRenderPass），不需要这里的任何东西。

#include <cstdint>

namespace FISIR {

	enum class DisplayDeviceType : uint32_t {
		// ── 已实现（本机 Windows/NVIDIA 可跑）────────────────────────────────
		Win32Window = 0,   // 句柄 = Win32DisplayHandle   { hinstance, hwnd }        → vkCreateWin32SurfaceKHR
		Headless,          // 句柄 = nullptr（忽略）：无 surface、无交换链，只渲染到离屏目标

		// ── 接口已定形、按平台实现（本机用不了，原因见 .claude/Results）──────
		WinRtCoreWindow,   // 句柄 = WinRtDisplayHandle   { coreWindow }            → VK_KHR_winrt_surface
		XlibWindow,        // 句柄 = XlibDisplayHandle    { display, window }       → VK_KHR_xlib_surface
		WaylandWindow,     // 句柄 = WaylandDisplayHandle { display, surface }      → VK_KHR_wayland_surface
		AndroidWindow,     // 句柄 = AndroidDisplayHandle { nativeWindow }          → VK_KHR_android_surface
		MetalLayer,        // 句柄 = MetalDisplayHandle   { layer }                  → VK_EXT_metal_surface
		DisplayPlane,      // 句柄 = DisplayPlaneHandle   { displayIndex, planeIndex, modeIndex }
		                   //        → VK_KHR_display + vkCreateDisplayPlaneSurfaceKHR，
		                   //          还必须带 device 扩展 VK_KHR_display_swapchain（Windows 上不导出）

		Count,
	};

	// 日志/诊断用。未实现的类型也会在这里给出名字，方便报错时能直接读。
	inline const char* DisplayDeviceTypeName(DisplayDeviceType type) {
		switch (type) {
		case DisplayDeviceType::Win32Window:     return "Win32Window";
		case DisplayDeviceType::Headless:        return "Headless";
		case DisplayDeviceType::WinRtCoreWindow: return "WinRtCoreWindow";
		case DisplayDeviceType::XlibWindow:      return "XlibWindow";
		case DisplayDeviceType::WaylandWindow:   return "WaylandWindow";
		case DisplayDeviceType::AndroidWindow:   return "AndroidWindow";
		case DisplayDeviceType::MetalLayer:      return "MetalLayer";
		case DisplayDeviceType::DisplayPlane:    return "DisplayPlane";
		default:                                 return "Unknown";
		}
	}

	// ── 句柄布局 ────────────────────────────────────────────────────────────
	// 约定：RHICreateViewport 的 deviceHandle 必须指向与 deviceType 匹配的那个结构体；
	// 只有 Headless 传 nullptr。字段用 void* 是为了不把 windows.h / Xlib.h 拉进 RHI 头，
	// 语义与真身（HINSTANCE/HWND、Display*/Window…）完全一致，布局等价可直接别名。

	struct Win32DisplayHandle {
		void* hinstance;   // HINSTANCE
		void* hwnd;        // HWND
	};

	struct WinRtDisplayHandle {
		void* coreWindow;  // IUnknown*（CoreWindow）
	};

	struct XlibDisplayHandle {
		void* display;     // Display*
		void* window;      // Window (XID)
	};

	struct WaylandDisplayHandle {
		void* display;     // wl_display*
		void* surface;     // wl_surface*
	};

	struct AndroidDisplayHandle {
		void* nativeWindow;   // ANativeWindow*
	};

	struct MetalDisplayHandle {
		void* layer;       // CAMetalLayer*
	};

	struct DisplayPlaneHandle {
		uint32_t displayIndex;   // vkGetPhysicalDeviceDisplayPropertiesKHR 的下标
		uint32_t planeIndex;     // vkGetPhysicalDeviceDisplayPlanePropertiesKHR 的下标
		uint32_t modeIndex;      // vkGetDisplayModePropertiesKHR 的下标
	};

}
