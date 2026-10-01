// ─────────────────────────────────────────────────────────────────────────────
// 平台层 · Android 实现
//
// 入口链：系统加载 libTextureCube.so → native_app_glue 的 ANativeActivity_onCreate
//        → android_main(app) → RunTextureCube()（Main.cpp 里与桌面共用的渲染主体）
//
// 这一版刻意做「最简可用」：
//   · 窗口（ANativeWindow）在 APP_CMD_INIT_WINDOW 时拿到，直接交给 RHI 建
//     VkSurfaceKHR（DisplayDeviceType::AndroidWindow，见 RHIDisplay.h / VulkanViewport.cpp）；
//   · 窗口被销毁（APP_CMD_TERM_WINDOW，比如切到后台）→ 主循环结束、应用退出。
//     原因：RHI 目前没有「换 surface / 重建交换链」的接口（只有 RHICreateViewport），
//     要支持「切后台再回来」得先补 RHIDestroyViewport 或 RHIRecreateSurface —— 那是下一步的事。
//   · 资源从 APK 的 assets 读（AAssetManager）。
// ─────────────────────────────────────────────────────────────────────────────
#ifdef __ANDROID__

#include "Platform.h"

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <cstring>
#include <string>

#include "Log/Logger.h"

// Main.cpp 里的渲染主体（桌面版是 main(argc, argv)）
extern int RunTextureCube();

namespace {

	// 触摸事件队列容量：宿主每帧都会取走，正常远远用不满；给个上限只是防止宿主卡住时无界增长。
	constexpr int kMaxPendingTouch = 32;

	struct AndroidImpl {
		android_app* app = nullptr;
		FISIR::AndroidDisplayHandle handle{ nullptr };
		bool windowReady = false;
		bool windowLost = false;
		bool destroyRequested = false;
		bool paused = false;   // PAUSE/STOP：不可见（切后台、任务被划走、挂起过渡）→ 停止渲染
		Platform::TouchState touch;   // 主触点当前状态（仅用于「触摸时画指示点」）
		// 触摸事件队列：**必须按事件喂 ImGui**，不能只取当前状态 —— 快速点击的 DOWN/UP
		// 常常落在同一帧，按状态采样会把按下压成抬起，控件永远收不到点击。
		Platform::TouchEvent pendingTouch[kMaxPendingTouch];
		int pendingTouchCount = 0;
		int32_t width = 0;
		int32_t height = 0;
	};

	AndroidImpl* g_Impl = nullptr;

	void UpdateWindowSize(AndroidImpl& impl) {
		if (impl.app && impl.app->window) {
			impl.width = ANativeWindow_getWidth(impl.app->window);
			impl.height = ANativeWindow_getHeight(impl.app->window);
		}
	}

	void HandleAppCmd(android_app* app, int32_t cmd) {
		AndroidImpl* impl = static_cast<AndroidImpl*>(app->userData);
		if (!impl) return;
		switch (cmd) {
		case APP_CMD_INIT_WINDOW:
			UpdateWindowSize(*impl);
			impl->windowReady = (app->window != nullptr);
			impl->windowLost = false;
			Info("[Android] APP_CMD_INIT_WINDOW: ANativeWindow=0x{:x} {}x{}",
				 (size_t)app->window, impl->width, impl->height);
			break;
		case APP_CMD_TERM_WINDOW:
			// 切后台 / 锁屏 / 分屏都会走到这里：窗口被系统回收。
			// 这时**只停渲染**，不要结束 Activity（旧版直接 finish，导致系统恢复同一个
			// Activity 时已经没有渲染线程，表现为卡死）。
			impl->windowReady = false;
			impl->windowLost = true;
			Warn("[Android] APP_CMD_TERM_WINDOW: 窗口被回收 → 停渲染并等新窗口（进程保留）");
			break;
		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONFIG_CHANGED:
			// 旋转/分屏/折叠屏都会改窗口尺寸。交换链在下一次 acquire 时按 surface 的
			// 最新 capabilities 重建（见 VulkanSwapChain::acquireGetImageInfoID 的 OUT_OF_DATE 处理），
			// 这里只要把新尺寸记下来、并在需要时触发重建即可。
			UpdateWindowSize(*impl);
			Info("[Android] 窗口尺寸变化: {}x{}", impl->width, impl->height);
			break;
		case APP_CMD_PAUSE:
		case APP_CMD_STOP:
			// 不可见（切后台、任务被划走、进入挂起过渡）：**停止渲染**。
			// 这个主循环是 present-bound 的：系统一旦不再按显示节奏节流，它会以 1000+ fps 空转
			//（渲染只占 0.02ms，帧率完全由 acquire 决定），纯粹烧 CPU/带宽/电。
			// 注意这里**不拆 RHI**：窗口还在，恢复时应当立刻接着画（拆 RHI 是 TERM_WINDOW 的事）。
			impl->paused = true;
			Info("[Android] APP_CMD_{}: 不可见 → 暂停渲染（等 RESUME/START）",
				 cmd == APP_CMD_PAUSE ? "PAUSE" : "STOP");
			break;
		case APP_CMD_RESUME:
		case APP_CMD_START:
			impl->paused = false;
			Info("[Android] APP_CMD_{}: 恢复可见 → 继续渲染", cmd == APP_CMD_RESUME ? "RESUME" : "START");
			break;
		case APP_CMD_GAINED_FOCUS:
		case APP_CMD_LOST_FOCUS:
			// 失焦 ≠ 不可见：分屏/小窗里仍然看得见，照常渲染；真正的「停」由 PAUSE/STOP 决定。
			break;
		case APP_CMD_DESTROY:
			impl->destroyRequested = true;
			break;
		default:
			break;
		}
	}

	// 触摸 → ImGui 的「鼠标」：安卓没有系统指针，把主触点当作左键即可让面板上的控件可用。
	// 坐标就是窗口/表面坐标（与交换链 extent 同一套，也正是 ImGui 的 DisplaySize 空间）。
	// 返回 0：不消费事件，交给系统继续处理（否则会吃掉边缘返回手势等系统手势）。
	int32_t HandleInput(android_app* app, AInputEvent* event) {
		AndroidImpl* impl = static_cast<AndroidImpl*>(app->userData);
		if (!impl || !event) return 0;

		const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
		// 只取主触点：面板按钮不需要多指
		const float tx = AMotionEvent_getX(event, 0);
		const float ty = AMotionEvent_getY(event, 0);
		impl->touch.x = tx;
		impl->touch.y = ty;
		impl->touch.available = true;

		bool down = impl->touch.down;
		bool push = true;
		switch (action) {
		case AMOTION_EVENT_ACTION_DOWN:
		case AMOTION_EVENT_ACTION_MOVE:
			down = true;
			break;
		case AMOTION_EVENT_ACTION_UP:
		case AMOTION_EVENT_ACTION_CANCEL:
			down = false;
			break;
		case AMOTION_EVENT_ACTION_POINTER_DOWN:
		case AMOTION_EVENT_ACTION_POINTER_UP:
		default:
			push = false;   // 多指变化 / 其它动作：不动主触点
			break;
		}
		impl->touch.down = down;
		if (push) {
			if (impl->pendingTouchCount < kMaxPendingTouch) {
				impl->pendingTouch[impl->pendingTouchCount++] = Platform::TouchEvent{ tx, ty, down };
			} else {
				// 队列满（宿主卡住或事件洪水）：丢掉最旧的一条，保住最新状态，避免积压爆掉。
				for (int i = 1; i < kMaxPendingTouch; ++i) impl->pendingTouch[i - 1] = impl->pendingTouch[i];
				impl->pendingTouch[kMaxPendingTouch - 1] = Platform::TouchEvent{ tx, ty, down };
			}
		}
		return 0;
	}

	// 非阻塞地抽干 looper 队列（用 ALooper_pollOnce：r27 起 ALooper_pollAll 已废弃）
	void DrainLooper(AndroidImpl& impl) {
		android_app* app = impl.app;
		while (true) {
			int events = 0;
			android_poll_source* source = nullptr;
			const int ident = ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source));
			if (source) source->process(app, source);
			if (app->destroyRequested) { impl.destroyRequested = true; return; }
			if (ident == ALOOPER_POLL_TIMEOUT) break;
		}
	}

} // anonymous namespace

void android_main(struct android_app* app) {
	AndroidImpl impl;
	impl.app = app;
	g_Impl = &impl;
	app->userData = &impl;
	app->onAppCmd = HandleAppCmd;
	app->onInputEvent = HandleInput;   // 触摸 → ImGui（面板上的开关按钮要用）
	app->onInputEvent = nullptr;   // 不处理输入事件（ImGui 里只用 io.DisplaySize 与 io.DeltaTime）

	// SPIR-V 磁盘缓存目录：Android 上唯一稳定可写的是应用的私有数据目录
	// （APK 里的 assets 只读、TMPDIR 不保证）。命中缓存时连 Slang 运行时都不会加载，
	// 手机上启动会快很多（见 ShaderComplier.h 的说明）。
	if (app->activity && app->activity->internalDataPath) {
		// 缓存与日志都放**应用外部私有目录**（/sdcard/Android/data/<包名>/files/）：
		// adb 能直接读，排查「手机上编出来的 SPIR-V 到底对不对」时可以把 .spv 拉回 PC 校验
		// （spirv-val），不需要 debuggable、不需要存储权限。
		const char* dataDir = app->activity->externalDataPath ? app->activity->externalDataPath
		                                                    : app->activity->internalDataPath;
		setenv("FISIR_SPV_CACHE_DIR", dataDir, 1);
		Info("[Android] SPIR-V 缓存目录 = {}", dataDir);

		// 日志旁路落文件：日志线程是异步的，原生崩溃会把队列里没写出去的消息带走 ——
		// 手机上排查「启动一会儿就 SIGSEGV」时丢的恰好是关键尾巴。
		std::string logPath = std::string(dataDir) + "/fisir.log";
		setenv("FISIR_LOG_FILE", logPath.c_str(), 1);
		Info("[Android] 日志同步落盘 = {}", logPath);
	}

	Info("[Android] android_main 进入：assetManager=0x{:x}", (size_t)app->activity->assetManager);

	// ── 生命周期：窗口会反复被回收/重建，进程要活下来 ──────────────────────────
	// 症状对照（旧版为什么会卡死）：
	//   · 锁屏/切后台 → TERM_WINDOW → 旧版直接 finish Activity 并让 android_main 返回；
	//     而系统往往恢复的是**同一个 Activity 实例**，不会再次调用 ANativeActivity_onCreate，
	//     于是窗口还在、渲染线程没了 ⇒ 永远停在最后一帧 = 卡死；
	//   · 从桌面回来同理（没有「窗口回来 → 重建」的路径）；
	//   · 旋转虽然改了窗口尺寸，但交换链没重建 ⇒ 看起来既没横屏也没画面。
	// 现在：窗口丢了就结束**本轮**渲染主体（它会把 RHI 干净拆掉），等下一个窗口到手再重建一遍。
	// 这要求 RHI 的进程级静态缓存必须清干净（VulkanDevice::Destory / ~VulkanRHI 里已修）。
	while (!app->destroyRequested) {
		const int ret = RunTextureCube();   // ← 与桌面共用的渲染主体
		Info("[Android] 渲染主体结束（ret={}）", ret);
		if (ret != 0) break;                // 初始化失败：别死循环重试
		if (app->destroyRequested) break;

		// 等新窗口。阻塞在 looper 上，不空转 CPU。
		while (!impl.windowReady && !app->destroyRequested) {
			int events = 0;
			android_poll_source* source = nullptr;
			ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source));
			if (source) source->process(app, source);
		}
		if (app->destroyRequested) break;
		impl.windowLost = false;
		Info("[Android] 新窗口已就绪（{}x{}），重建渲染", impl.width, impl.height);
	}

	Info("[Android] 收到结束请求，finish Activity");
	if (app->activity) ANativeActivity_finish(app->activity);
}

namespace Platform {

	int PollTouchEvents(TouchEvent* out, int maxCount, TouchState& state) {
		if (!g_Impl) return 0;
		AndroidImpl& impl = *g_Impl;
		state = impl.touch;
		int n = 0;
		while (n < maxCount && impl.pendingTouchCount > 0) {
			out[n++] = impl.pendingTouch[0];
			for (int i = 1; i < impl.pendingTouchCount; ++i) impl.pendingTouch[i - 1] = impl.pendingTouch[i];
			--impl.pendingTouchCount;
		}
		return n;
	}

	bool Init(Window& window, bool hidden, const char* title) {
		if (!g_Impl) { Error("Platform::Init 必须在 android_main 里调用"); return false; }
		AndroidImpl& impl = *g_Impl;

		// 等系统把窗口交给我们（INIT_WINDOW）。正常情况下这一等很短；若用户在启动前就退出，
		// 这里会拿到 destroyRequested 并干净失败。
		while (!impl.windowReady && !impl.destroyRequested) {
			int events = 0;
			android_poll_source* source = nullptr;
			const int ident = ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source));
			if (source) source->process(impl.app, source);
			if (impl.app->destroyRequested) { impl.destroyRequested = true; break; }
			if (ident == ALOOPER_POLL_TIMEOUT && !impl.windowReady) {
				// -1 理论上会阻塞到有事件；真超时了就再转一圈（不忙等，交给 looper）
				continue;
			}
		}
		if (!impl.windowReady || !impl.app->window) {
			Error("Platform::Init: 没等到 ANativeWindow（应用可能启动即退出）");
			return false;
		}

		impl.handle.nativeWindow = impl.app->window;
		window.type = FISIR::DisplayDeviceType::AndroidWindow;
		window.deviceHandle = &impl.handle;
		window.width = (uint32_t)ANativeWindow_getWidth(impl.app->window);
		window.height = (uint32_t)ANativeWindow_getHeight(impl.app->window);
		window.impl = &impl;
		Info("Platform(Android): ANativeWindow=0x{:x} {}x{}", (size_t)impl.handle.nativeWindow, window.width, window.height);
		return true;
	}

	bool PumpEvents(Window& window) {
		AndroidImpl& impl = *static_cast<AndroidImpl*>(window.impl);
		DrainLooper(impl);

		// ── 直接抽干输入队列 ────────────────────────────────────────────────────
		// native_app_glue 的正规路径是把事件通过 looper 的 process_input 交给我们注册的
		// onInputEvent。但实测在某些设备/窗口布局下这些事件根本到不了回调（日志里能看到队列非空、
		// 却一次 HandleInput 都没触发），面板就永远点不动。这里在同一线程（就是持有该 looper 的
		// 线程）上自己抽一次队列是安全的，也绕开了那条不投递的路径。
		if (impl.app && impl.app->inputQueue) {
			AInputEvent* ev = nullptr;
			while (AInputQueue_getEvent(impl.app->inputQueue, &ev) >= 0) {
				if (AInputQueue_preDispatchEvent(impl.app->inputQueue, ev)) {
					// 已被系统预派发消费（手势/输入法窗口等）：按文档不能再 finishEvent
					continue;
				}
				int32_t handled = 0;
				if (impl.app->onInputEvent) handled = impl.app->onInputEvent(impl.app, ev);
				AInputQueue_finishEvent(impl.app->inputQueue, ev, handled);
			}
		}
		if (impl.destroyRequested) { Info("[Android] destroyRequested：退出主循环"); return false; }
		// 暂停期间阻塞在 looper 上：主线程 0 占用；一旦 RESUME/START（或被系统回收窗口、
		// 或应用要退出）立刻醒来。这样「快要挂起 / 变成小窗」时不会继续满速空转。
		while (impl.paused && !impl.destroyRequested && !impl.windowLost) {
			int events = 0;
			android_poll_source* source = nullptr;
			ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source));
			if (source) source->process(impl.app, source);
			if (impl.app->destroyRequested) { impl.destroyRequested = true; }
		}
		if (impl.windowLost || !impl.windowReady) {
			// 窗口被系统回收：结束**这一轮**渲染主体（它会干净拆掉 RHI），
			// 由 android_main 等待下一个窗口后整体重建。进程不退出。
			Warn("[Android] 窗口已失效：结束本轮渲染，等新窗口重建");
			return false;
		}
		return true;
	}

	bool LoadAsset(const char* path, std::vector<unsigned char>& out) {
		if (!g_Impl || !g_Impl->app || !g_Impl->app->activity) return false;
		AAssetManager* manager = g_Impl->app->activity->assetManager;
		if (!manager) { Error("LoadAsset: 没有 assetManager"); return false; }

		AAsset* asset = AAssetManager_open(manager, path, AASSET_MODE_BUFFER);
		if (!asset) { Error("LoadAsset: assets 里没有 '{}'", path); return false; }
		const off_t length = AAsset_getLength(asset);
		if (length <= 0) { AAsset_close(asset); Error("LoadAsset: '{}' 长度为 0", path); return false; }
		out.resize((size_t)length);
		const int read = AAsset_read(asset, out.data(), out.size());
		AAsset_close(asset);
		if (read != (int)out.size()) { Error("LoadAsset: '{}' 只读到 {}/{}", path, read, out.size()); return false; }
		Info("LoadAsset: '{}' {} 字节", path, out.size());
		return true;
	}

	void SetTitle(Window& window, const char* title) {
		(void)window; (void)title;   // Android 的标题由 Activity/Manifest 决定
	}

	void Shutdown(Window& window) {
		window.impl = nullptr;
		window.deviceHandle = nullptr;
	}

} // namespace Platform

#endif // __ANDROID__
