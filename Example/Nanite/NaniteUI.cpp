#include "NaniteUI.h"

#include <chrono>

#include "imgui.h"

#include "ClusterSelection.h"
#include "ImGui_Impl_FISIR.h"
#include "Log/Logger.h"

namespace NaniteUI {
	namespace {

		FISIR::DynamicRHI* g_Rhi = nullptr;
		HWND g_Window = nullptr;
		uint32_t g_DisplayWidth = NANITE_RT_WIDTH;
		uint32_t g_DisplayHeight = NANITE_RT_HEIGHT;

		bool g_ShowDemoWindow = false;
		// ClearColor 在 RenderParams 里是 R|G<<8|B<<16 的打包整数，面板用 float[3] 编辑后回写。
		float g_ClearColor[3] = { 0.0f, 0.0f, 0.0f };
		bool g_ClearColorInitialized = false;

		constexpr float kDefaultLodScale = 1000.0f;
		constexpr float kDefaultSwRasterThreshold = 0.02f;
		constexpr float kDefaultFarPlane = 20000.0f;
		constexpr uint32_t kDefaultColorBlock = 1;   // 与 ClisterSelection.cpp 的初值一致

		std::chrono::steady_clock::time_point g_LastFrameTime;
		float g_SmoothedFps = 0.0f;
		float g_SmoothedGpuMs = 0.0f;

		// 字体：ImGui 内置的 ProggyClean 只有 ASCII，中文标签会渲染成一排方点。
	// 优先加载系统里的中文字体（微软雅黑 → 黑体 → 宋体 → 等线），拿不到就退回内置字体。
	bool LoadFont(ImGuiIO& io) {
		static const char* kCandidates[] = {
			"C:/Windows/Fonts/msyh.ttc",     // 微软雅黑
			"C:/Windows/Fonts/simhei.ttf",   // 黑体
			"C:/Windows/Fonts/simsun.ttc",   // 宋体
			"C:/Windows/Fonts/Deng.ttf",     // 等线
		};
		for (const char* path : kCandidates) {
			if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
			// 简体中文常用字 ≈ 2500 个，图集比纯 ASCII 大得多（但仍是一次性上传）。
			if (io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr,
					io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
				Info("[NaniteUI] Font loaded: {}", path);
				return true;
			}
		}
		Warn("[NaniteUI] No CJK font found, falling back to the built-in ASCII font");
		io.Fonts->AddFontDefault();
		return false;
	}

	void HandleDemoWindow() {
			if (g_ShowDemoWindow) {
				// 完整的控件样例，用来验证后端对表格 / 树 / 图表 / 输入框等都正确渲染。
				ImGui::ShowDemoWindow(&g_ShowDemoWindow);
			}
		}

		void DrawPanel(CameraSettings& camera, const FrameStats& stats) {
			InputData& input = getInputData();
			RenderParams& params = getRenderParams();

			if (!g_ClearColorInitialized) {
				const uint32_t packed = params.ClearColor;
				g_ClearColor[0] = (float)(packed & 0xFF) / 255.0f;
				g_ClearColor[1] = (float)((packed >> 8) & 0xFF) / 255.0f;
				g_ClearColor[2] = (float)((packed >> 16) & 0xFF) / 255.0f;
				g_ClearColorInitialized = true;
			}

			ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
			ImGui::Begin("Nanite");

			ImGui::Text("%.1f FPS", g_SmoothedFps);
			ImGui::SameLine();
			ImGui::TextDisabled("|  GPU %.2f ms", g_SmoothedGpuMs);
			ImGui::Text("cam (%.1f, %.1f, %.1f)", stats.CameraX, stats.CameraY, stats.CameraZ);
			ImGui::Text("yaw %.1f  pitch %.1f", stats.YawDegrees, stats.PitchDegrees);

			// ── LOD selection ───────────────────────────────────────
			if (ImGui::CollapsingHeader("LOD Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::PushItemWidth(-110.0f);

				ImGui::SliderFloat("LodScale", &input.LodScale, 1.0f, 2000.0f, "%.0f",
					ImGuiSliderFlags_Logarithmic);
				if (ImGui::IsItemHovered()) {
					// 用后端助手而不是 ImGui::SetTooltip：悬停提示必须留在本面板所在的视口里，
					// 否则 ImGui 会给「装不下的 tooltip」单独建平台窗口（= 一条交换链 + 描述符堆），
					// 悬停次数一多就会和 RHI 线程互锁卡死。见 ImGui_Impl_FISIR.h 的说明。
					ImGui_ImplFISIR_SetTooltip("World-space error budget T = projScale / LodScale.\n"
						"A cluster is drawn when its own error <= T < its coarser level's error.\n"
						"Larger value = finer LOD (more clusters selected).");
				}

				ImGui::SliderFloat("SW raster threshold", &input.SwRasterThreshold, 0.0f, 10.0f, "%.3f");
				if (ImGui::IsItemHovered()) {
					ImGui_ImplFISIR_SetTooltip("Clusters whose angular size (~ radius / distance, radians) is below this\n"
						"are handed to the software rasterizer.\n"
						"Roughly 1800 x threshold pixels across: 0.08 ~ 144px, 0.02 ~ 36px.\n"
						"The SW raster does a per-pixel barycentric solve plus two global atomics,\n"
						"far costlier than hardware raster - feeding it 100px clusters drops the\n"
						"frame rate into the teens. Keep it <= 0.02. 0 = everything on hardware.");
				}

				ImGui::SliderFloat("Far plane", &params.FarPlane, 100.0f, 20000.0f, "%.0f");
				if (ImGui::IsItemHovered()) {
					ImGui_ImplFISIR_SetTooltip("Projection far plane; it directly sets the cluster projection scale.");
				}

				// ColorEdit3 自带的内建提示（ImGui::ColorTooltip）走的是 BeginTooltipEx，后端拦不到它；
				// 在 360 宽的面板里它十有八九会超出宿主视口 → ImGui 会给它单独建一个平台窗口
				// （= 一条交换链 + 一套描述符堆 + 一条管线），每次悬停建/销一轮。
				// 所以这里用 NoTooltip 关掉内建的，换成钉住视口的版本 —— 信息量不减（hex + RGB）。
				ImGui::ColorEdit3("Clear color", g_ClearColor, ImGuiColorEditFlags_NoTooltip);
				if (ImGui::IsItemHovered()) {
					ImGui_ImplFISIR_SetTooltip("Clear color  #%02X%02X%02X\nR %.3f   G %.3f   B %.3f",
						(uint32_t)(g_ClearColor[0] * 255.0f + 0.5f),
						(uint32_t)(g_ClearColor[1] * 255.0f + 0.5f),
						(uint32_t)(g_ClearColor[2] * 255.0f + 0.5f),
						g_ClearColor[0], g_ClearColor[1], g_ClearColor[2]);
				}
				params.ClearColor = (uint32_t)(g_ClearColor[0] * 255.0f + 0.5f)
					| ((uint32_t)(g_ClearColor[1] * 255.0f + 0.5f) << 8)
					| ((uint32_t)(g_ClearColor[2] * 255.0f + 0.5f) << 16);

				ImGui::PopItemWidth();

				if (ImGui::Button("Reset to defaults")) {
					input.LodScale = kDefaultLodScale;
					input.SwRasterThreshold = kDefaultSwRasterThreshold;
					params.FarPlane = kDefaultFarPlane;
					//params.ColorBlock = kDefaultColorBlock;
				}
			}

			// ── Camera ──────────────────────────────────────────────
			if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::PushItemWidth(-110.0f);
				ImGui::SliderFloat("FOV", &camera.FovDegrees, 20.0f, 120.0f, "%.0f");
				ImGui::SliderFloat("Move step", &camera.MoveStep, 0.01f, 20.0f, "%.2f");
				ImGui::SliderFloat("Mouse sensitivity", &camera.MouseSensitivity, 0.0005f, 0.02f, "%.4f");
				ImGui::PopItemWidth();
				ImGui::TextDisabled("RMB drag: look   WASD: move   Shift/Space: up-down");
			}

			// ── Shading (used by the resolve pass, see FrameBufferWrite.hlsl) ──
			if (ImGui::CollapsingHeader("Shading", ImGuiTreeNodeFlags_DefaultOpen)) {
				bool colorBlock = params.ColorBlock != 0;
				if (ImGui::Checkbox("Cluster color blocks", &colorBlock)) {
					params.ColorBlock = colorBlock ? 1u : 0u;
				}
				if (ImGui::IsItemHovered()) {
					ImGui_ImplFISIR_SetTooltip("On: one flat color per cluster - use it to spot overlaps\n"
						"(two colors in one area) or missing clusters (background showing through).\n"
						"Off: Lambert facet shading (grayscale).");
				}
			}

			// ── This frame's selection ──────────────────────────────
			if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
				const uint32_t totalClusters = stats.SoftwareClusters + stats.HardwareClusters;
				ImGui::Text("SW    : %u clusters / %u tris", stats.SoftwareClusters, stats.SoftwareTriangles);
				ImGui::Text("HW    : %u clusters", stats.HardwareClusters);
				ImGui::Text("Total : %u clusters", totalClusters);

				const float fraction = totalClusters > 0
					? (float)stats.SoftwareClusters / (float)totalClusters : 0.0f;
				ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), "SW cluster share");
			}

			ImGui::Separator();
			ImGui::Checkbox("ImGui demo window", &g_ShowDemoWindow);
			ImGui::TextDisabled("Double-click the title bar to collapse");

			ImGui::End();

			HandleDemoWindow();
		}

	} // namespace

	bool Init(FISIR::DynamicRHI* rhi, HWND hwnd) {
		if (!rhi || !hwnd) {
			Error("[NaniteUI] Init: rhi/hwnd is null");
			return false;
		}
		g_Rhi = rhi;
		g_Window = hwnd;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.IniFilename = nullptr;   // 不落地 imgui.ini：示例运行目录不该被写脏
		ImGui::StyleColorsDark();
		ImGui::GetStyle().WindowRounding = 6.0f;

		// 字体要在 ImGui_ImplFISIR_Init 之前准备好 —— 后者会立刻把图集上传成纹理。
		LoadFont(io);

		if (!ImGui_ImplFISIR_Init(rhi)) {
			ImGui::DestroyContext();
			return false;
		}

		// 冷启动一次平台层，把剪贴板等回调装上（之后每帧 NewFrame 会继续刷新）。
		ImGui_ImplFISIR_Win32_NewFrame(hwnd);

		g_LastFrameTime = std::chrono::steady_clock::now();
		Info("[NaniteUI] ImGui {} initialized", IMGUI_VERSION);
		return true;
	}

	void Shutdown() {
		if (!ImGui::GetCurrentContext()) return;
		ImGui_ImplFISIR_Shutdown();
		ImGui::DestroyContext();
		g_Rhi = nullptr;
		g_Window = nullptr;
	}

	bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		return ImGui_ImplFISIR_Win32_HandleMessage(hwnd, msg, wParam, lParam);
	}

	void NewFrame(CameraSettings& camera, const FrameStats& stats,
	              uint32_t displayWidth, uint32_t displayHeight) {
		if (!ImGui::GetCurrentContext()) return;

		g_DisplayWidth = displayWidth ? displayWidth : NANITE_RT_WIDTH;
		g_DisplayHeight = displayHeight ? displayHeight : NANITE_RT_HEIGHT;

		const auto now = std::chrono::steady_clock::now();
		const float deltaSeconds = std::chrono::duration<float>(now - g_LastFrameTime).count();
		g_LastFrameTime = now;

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)g_DisplayWidth, (float)g_DisplayHeight);
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);   // 直接渲到 swapchain，1:1
		io.DeltaTime = (deltaSeconds > 0.0f && deltaSeconds < 1.0f) ? deltaSeconds : (1.0f / 60.0f);

		// 指数平滑，避免数字每帧乱跳看不清
		if (deltaSeconds > 0.0f) {
			const float instantFps = 1.0f / deltaSeconds;
			g_SmoothedFps = (g_SmoothedFps > 0.0f) ? (g_SmoothedFps * 0.9f + instantFps * 0.1f) : instantFps;
		}
		if (g_Rhi) {
			const float gpuMs = (float)g_Rhi->getLastGPUTimeMs();
			g_SmoothedGpuMs = (g_SmoothedGpuMs > 0.0f) ? (g_SmoothedGpuMs * 0.9f + gpuMs * 0.1f) : gpuMs;
		}

		ImGui_ImplFISIR_Win32_NewFrame(g_Window);
		// 多视口：为上一帧新拖出来的平台窗口补建交换链。**必须在帧首**（上一帧 GPU 已完、
		// 还没开始录制）—— 建交换链是重活，放到帧中途的渲染回调里会和 RHI 工作线程互等而死锁。
		ImGui_ImplFISIR_PrepareViewportSwapChains();
		ImGui::NewFrame();
		// 整个主视口做成一个 DockSpace：面板可以停靠到四边、互相叠成 tab，也能拖出来变成
		// 自由浮动的 ImGui 窗口（仍画在主窗口里）。必须在任何 Begin() 之前调用。
		// 签名注：1.91.x 的 docking 分支是 DockSpaceOverViewport(dockspace_id, viewport, flags, ...)，
		// dockspace_id 传 0 让它自己生成。PassthruCentralNode = 中央区域透明，让 3D 场景透出来。
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
		DrawPanel(camera, stats);
		ImGui::Render();
	}

	void Render(FISIR::RHIRenderCommandList& cmdList, FISIR::RHIRenderPass* renderPass) {
		if (!ImGui::GetCurrentContext()) return;
		ImGui_ImplFISIR_RenderDrawData(cmdList, renderPass, ImGui::GetDrawData());
	}

	void UpdatePlatformWindows() {
		if (!ImGui::GetCurrentContext()) return;
		if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) return;
		// UpdatePlatformWindows 会按需建/销毁平台窗口并算好各窗口的 draw data；
		// RenderPlatformWindowsDefault 再逐个回调后端的 RenderWindow / SwapBuffers。
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}

	bool WantCaptureMouse() {
		return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
	}

	bool WantCaptureKeyboard() {
		return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard;
	}

}
