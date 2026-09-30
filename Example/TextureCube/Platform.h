#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 示例的平台层：桌面（Win32）与 Android 的差异全部收敛在这里。
//
// 设计意图：`Main.cpp` 的渲染主体（建管线、录命令、跑帧循环）两边**一字不改**地共用，
// 只有三件事按平台不同：
//   1. 窗口 / 呈现设备：Win32 建窗口拿 HWND；Android 从 NativeActivity 拿 ANativeWindow；
//   2. 事件循环：Win32 PeekMessage；Android ALooper_pollOnce + APP_CMD_* 生命周期；
//   3. 资源读取：Win32 走相对路径 fopen；Android 走 APK 里的 assets（AAssetManager）。
//
// 句柄布局一律用 RHIDisplay.h 里那套 void* 结构（RHI 头因此不依赖任何平台头）。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <vector>

#include "RHIDisplay.h"

namespace Platform {

	struct Window {
		FISIR::DisplayDeviceType type = FISIR::DisplayDeviceType::Win32Window;
		void* deviceHandle = nullptr;   // 指向 Win32DisplayHandle 或 AndroidDisplayHandle
		uint32_t width = 800;
		uint32_t height = 600;
		void* impl = nullptr;           // 平台私有数据（本文件之外别碰）
	};

	// 建窗口（Win32）/ 等 NativeActivity 给出 ANativeWindow（Android）。
	// 返回 false 表示拿不到可用的呈现目标，调用方直接失败退出。
	bool Init(Window& window, bool hidden, const char* title);

	// 泵一次事件。返回 false = 该退出主循环（Win32: WM_QUIT；Android: 系统要求结束或窗口没了）。
	bool PumpEvents(Window& window);

	// 读一个资源文件。Win32 按相对路径读磁盘；Android 从 APK 的 assets 读。
	bool LoadAsset(const char* path, std::vector<unsigned char>& out);

	// 设置窗口标题（Win32 有用；Android 是空实现）。
	void SetTitle(Window& window, const char* title);

	void Shutdown(Window& window);

} // namespace Platform
