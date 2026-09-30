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
#include <android/looper.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <cstring>

#include "Log/Logger.h"

// Main.cpp 里的渲染主体（桌面版是 main(argc, argv)）
extern int RunTextureCube();

namespace {

	struct AndroidImpl {
		android_app* app = nullptr;
		FISIR::AndroidDisplayHandle handle{ nullptr };
		bool windowReady = false;
		bool windowLost = false;
		bool destroyRequested = false;
	};

	AndroidImpl* g_Impl = nullptr;

	void HandleAppCmd(android_app* app, int32_t cmd) {
		AndroidImpl* impl = static_cast<AndroidImpl*>(app->userData);
		if (!impl) return;
		switch (cmd) {
		case APP_CMD_INIT_WINDOW:
			impl->windowReady = (app->window != nullptr);
			impl->windowLost = false;
			Info("[Android] APP_CMD_INIT_WINDOW: ANativeWindow=0x{:x} {}x{}",
				 (size_t)app->window,
				 app->window ? ANativeWindow_getWidth(app->window) : 0,
				 app->window ? ANativeWindow_getHeight(app->window) : 0);
			break;
		case APP_CMD_TERM_WINDOW:
			impl->windowReady = false;
			impl->windowLost = true;
			Warn("[Android] APP_CMD_TERM_WINDOW: 窗口被系统回收（本版直接结束，见文件头说明）");
			break;
		case APP_CMD_WINDOW_RESIZED:
			Info("[Android] APP_CMD_WINDOW_RESIZED: {}x{}",
				 app->window ? ANativeWindow_getWidth(app->window) : 0,
				 app->window ? ANativeWindow_getHeight(app->window) : 0);
			break;
		case APP_CMD_GAINED_FOCUS:
		case APP_CMD_LOST_FOCUS:
			break;
		case APP_CMD_DESTROY:
			impl->destroyRequested = true;
			break;
		default:
			break;
		}
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

	// SPIR-V 磁盘缓存目录：Android 上唯一稳定可写的是应用的私有数据目录
	// （APK 里的 assets 只读、TMPDIR 不保证）。命中缓存时连 Slang 运行时都不会加载，
	// 手机上启动会快很多（见 ShaderComplier.h 的说明）。
	if (app->activity && app->activity->internalDataPath) {
		setenv("FISIR_SPV_CACHE_DIR", app->activity->internalDataPath, 1);
		Info("[Android] SPIR-V 缓存目录 = {}", app->activity->internalDataPath);
	}

	Info("[Android] android_main 进入：assetManager=0x{:x}", (size_t)app->activity->assetManager);

	// 窗口会随「息屏 / 切后台 / 分屏」被系统回收，回来时给的是**新的** ANativeWindow。
	// RHI 目前没有「换 surface / 重建交换链」的接口（只有 RHICreateViewport），所以这一版的处理是：
	// 窗口没了就结束渲染主体并 finish Activity（进程退出）。
	//
	// 试过「进程留住、窗口回来重新初始化」的写法（android_main 里循环重跑 RunTextureCube），
	// 但实测在**拆 RHI 的时候会 native 崩**：Thread-6（RHI 的常驻工作线程之一）在
	// destroyRenderInterface 期间 SIGSEGV，fault addr 0x28 —— 说明析构与工作线程的收尾有竞态，
	// 同一进程里二次初始化也不干净。要支持「切后台再回来」得先把这条收尾链路修干净，
	// 并补 RHIDestroyViewport / surface 重建接口（见 .claude/Results 里的 TODO）。
	RunTextureCube();   // ← 与桌面共用的渲染主体

	Info("[Android] 渲染主体结束，请求结束 Activity");
	if (app->activity) ANativeActivity_finish(app->activity);
}

namespace Platform {

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
		if (impl.destroyRequested) { Info("[Android] destroyRequested：退出主循环"); return false; }
		if (impl.windowLost || !impl.windowReady) {
			Warn("[Android] 窗口已失效：退出主循环（换 surface 需要 RHI 支持重建，见文件头说明）");
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
