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

	// ── 触摸输入（只有 Android 有；桌面恒为 available=false）────────────────────
	// 目的是让 Android 上的 ImGui 面板可交互（例如垂直同步开关）—— 面板按钮需要指针状态，
	// 而安卓没有系统鼠标：把 AMotionEvent 的主触点映射成 ImGui 的鼠标左键即可。
	// 坐标是**窗口/表面坐标**（与交换链 extent 同一套，即 ImGui 的 DisplaySize 空间）。
	struct TouchState {
		float x = 0.0f;
		float y = 0.0f;
		bool  down = false;
		bool  available = false;   // 该平台本次是否有触摸信息（桌面恒 false）
	};

	// 一次触摸事件（按发生顺序）。关键在于**按事件**而不是按帧喂给 ImGui：
	// 一次快速点击（DOWN/UP 间隔几十毫秒）很可能落在同一帧里，只取「当前状态」会把
	// 按下压成抬起，ImGui 就永远收不到点击（实测 adb input tap 无反应就是这个原因）。
	struct TouchEvent {
		float x = 0.0f;
		float y = 0.0f;
		bool  down = false;        // true=按下/移动，false=抬起/取消
	};

	// 取走自上次调用以来累积的触摸事件（安卓：AMotionEvent 主触点；桌面：恒返回 0）。
	// 返回写入 out 的事件个数（最多 maxCount），同时把 state 填成最新状态。
	int PollTouchEvents(TouchEvent* out, int maxCount, TouchState& state);

	// 输入队列里还积压着多少事件（安卓：AInputQueue_hasEvents；桌面恒 0）。
	// 用来区分「事件根本没进队列」（= 系统没往我们窗口派发）与「进了队列但没被取走」
	// （= 我们的抽取路径有问题）—— 面板上会显示它，排查触摸不响应时是决定性的判据。
	int PendingInputEvents();

	// 设置窗口标题（Win32 有用；Android 是空实现）。
	void SetTitle(Window& window, const char* title);

	void Shutdown(Window& window);

} // namespace Platform
