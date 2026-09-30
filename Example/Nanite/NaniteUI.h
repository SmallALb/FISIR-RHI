#pragma once

// Nanite 示例的 ImGui 调试面板 + Win32 输入接线。
//
// 面板直接编辑 InputData（LodScale / SwRasterThreshold）与 RenderParams（FarPlane / ClearColor）
// 这两块主机可见缓冲，所以这里不需要把值再传回主循环；只有相机相关的几个参数由调用方持有
// （CameraSettings），每帧读一次。

#include <cstdint>
#include <windows.h>

namespace FISIR {
	class DynamicRHI;
	class RHIRenderCommandList;
	class RHIRenderPass;
}

namespace NaniteUI {

	// 面板里可调、但由 Main.cpp 每帧使用的相机 / 循环参数。
	struct CameraSettings {
		float MoveStep = 0.3f;            // WASD/QE 每帧位移
		float MouseSensitivity = 0.003f;  // 右键拖拽灵敏度（弧度/像素）
		float FovDegrees = 60.0f;         // 垂直 FOV
	};

	// 面板要显示的每帧统计量（FPS / GPU 耗时由本模块自己测）。
	struct FrameStats {
		uint32_t SoftwareClusters = 0;
		uint32_t SoftwareTriangles = 0;
		uint32_t HardwareClusters = 0;
		float CameraX = 0.0f, CameraY = 0.0f, CameraZ = 0.0f;
		float YawDegrees = 0.0f, PitchDegrees = 0.0f;
	};

	// 创建 ImGui context 并初始化 RHI 后端（要求 rhi 已完成 Init()）。
	bool Init(FISIR::DynamicRHI* rhi, HWND hwnd);

	void Shutdown();

	// 窗口过程转发。返回 true = 消息已被后端处理，不要再交给 DefWindowProc。
	bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	// 开新帧 → 建面板 → 结束帧。必须在上一帧 GPU 工作已完成之后调用
	// （示例里放在 WaitFrameGPUIdle() 之后），因为后端顶点缓冲是单份复用的。
	// camera 非 const：面板里的 FOV / 步长 / 灵敏度滑条直接改它。
	// displayWidth/Height = 渲染目标像素尺寸（= 交换链实际 extent），用作 io.DisplaySize。
	void NewFrame(CameraSettings& camera, const FrameStats& stats,
	              uint32_t displayWidth, uint32_t displayHeight);

	// 在当前 render pass 内录制 ImGui。必须夹在 BeginRenderPass/EndRenderPass 之间，
	// 且排在场景绘制之后（ImGui 不做深度，靠提交顺序覆盖）。
	void Render(FISIR::RHIRenderCommandList& cmdList, FISIR::RHIRenderPass* renderPass);

	// 多视口：把被拖出主窗口的那些独立 OS 窗口更新/渲染出来。
	// 必须在主窗口 present 之后、每帧调用一次（后端会为每个视口 acquire/渲染/present 自己的交换链）。
	void UpdatePlatformWindows();

	// 相机输入是否要让位给 UI（鼠标落在面板上 / 正在输入文本）。
	bool WantCaptureMouse();
	bool WantCaptureKeyboard();

}
