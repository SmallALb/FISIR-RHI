// ─────────────────────────────────────────────────────────────────────────────
// 平台层 · Win32 实现（桌面）
// 与 Android 实现（PlatformAndroid.cpp）二选一编译，见 CMakeLists 的平台分支。
// ─────────────────────────────────────────────────────────────────────────────
#ifndef __ANDROID__

#include "Platform.h"

#include <cstdio>
#include <windows.h>

#include "Log/Logger.h"

namespace Platform {

	namespace {
		struct Win32Impl {
			HINSTANCE hInstance = nullptr;
			HWND      hwnd = nullptr;
			FISIR::Win32DisplayHandle handle{ nullptr, nullptr };
			bool      quit = false;
		};

		LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
			switch (uMsg) {
			case WM_DESTROY: PostQuitMessage(0); return 0;
			case WM_KEYDOWN: if (wParam == VK_ESCAPE) PostQuitMessage(0); return 0;
			default: return DefWindowProc(hwnd, uMsg, wParam, lParam);
			}
		}
	} // anonymous namespace

	bool Init(Window& window, bool hidden, const char* title) {
		auto* impl = new Win32Impl();
		window.impl = impl;

		const char CLASS_NAME[] = "FISIRExampleWindow";
		WNDCLASSA wc = { 0 };
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = GetModuleHandle(NULL);
		wc.lpszClassName = CLASS_NAME;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		RegisterClassA(&wc);

		impl->hInstance = wc.hInstance;
		impl->hwnd = CreateWindowExA(
			0, CLASS_NAME, title,
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			(int)window.width, (int)window.height,
			NULL, NULL, impl->hInstance, NULL);
		if (!impl->hwnd) {
			Error("CreateWindowExA failed ({})", GetLastError());
			return false;
		}
		if (!hidden) ShowWindow(impl->hwnd, SW_SHOW);

		// 窗口客户区尺寸可能与请求值不同（有边框/标题栏），以实际值为准。
		RECT rect{};
		if (GetClientRect(impl->hwnd, &rect)) {
			window.width = (uint32_t)(rect.right - rect.left);
			window.height = (uint32_t)(rect.bottom - rect.top);
			if (window.width == 0 || window.height == 0) { window.width = 800; window.height = 600; }
		}

		impl->handle = { impl->hInstance, impl->hwnd };
		window.type = FISIR::DisplayDeviceType::Win32Window;
		window.deviceHandle = &impl->handle;
		Info("Platform(Win32): hwnd=0x{:x} 可见={} 客户区={}x{}", (size_t)impl->hwnd, !hidden, window.width, window.height);
		return true;
	}

	bool PumpEvents(Window& window) {
		auto* impl = static_cast<Win32Impl*>(window.impl);
		MSG msg = { 0 };
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) { impl->quit = true; return false; }
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		return true;
	}

	bool LoadAsset(const char* path, std::vector<unsigned char>& out) {
		FILE* f = fopen(path, "rb");
		if (!f) { Warn("LoadAsset: 打不开 '{}'（工作目录对?）", path); return false; }
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size <= 0) { fclose(f); Warn("LoadAsset: '{}' 是空文件", path); return false; }
		out.resize((size_t)size);
		const size_t read = fread(out.data(), 1, out.size(), f);
		fclose(f);
		if (read != out.size()) { Warn("LoadAsset: '{}' 只读到 {}/{} 字节", path, read, out.size()); return false; }
		return true;
	}

	void SetTitle(Window& window, const char* title) {
		auto* impl = static_cast<Win32Impl*>(window.impl);
		if (impl && impl->hwnd) SetWindowTextA(impl->hwnd, title);
	}

	// 桌面没有触摸：ImGui 由 Win32 后端（ImGui_ImplFISIR_Win32_NewFrame）喂鼠标与键盘，
	// 这里只给出空事件，让示例侧那段安卓专属代码在桌面也能编译、且不做任何事。
	int PollTouchEvents(TouchEvent*, int, TouchState&) { return 0; }
	int PendingInputEvents() { return 0; }

	void Shutdown(Window& window) {
		auto* impl = static_cast<Win32Impl*>(window.impl);
		if (impl) {
			if (impl->hwnd) DestroyWindow(impl->hwnd);
			delete impl;
		}
		window.impl = nullptr;
		window.deviceHandle = nullptr;
	}

} // namespace Platform

#endif // !__ANDROID__
