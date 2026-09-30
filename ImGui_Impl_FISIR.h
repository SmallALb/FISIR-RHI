#pragma once

// ImGui → FISIR RHI 渲染后端（+ Win32 平台输入）。
//
// 这是「RHI 层」的后端：它只依赖 DynamicRHI / RHICommandList / RHIPipeline 这些公共接口，
// 不碰任何 Vulkan 类型，因此换后端（DX12）时无需改动。
//
// 用法（与 Dear ImGui 官方后端一致的三段式）：
//
//   IMGUI_CHECKVERSION();
//   ImGui::CreateContext();
//   ImGui::StyleColorsDark();
//   ImGui_ImplFISIR_Init(rhi);              // 字体图集 / 采样器 / 顶点索引缓冲
//
//   主循环：
//     ImGui_ImplFISIR_Win32_NewFrame(hwnd); // 刷新鼠标位置与焦点
//     ImGui::NewFrame();
//     ... 建 UI ...                          // 相机输入前查 io.WantCaptureMouse 让位给 UI
//     ImGui::Render();
//
//     cmdList.BeginRenderPass(frameBuf, 0, clear);
//     ... 场景绘制 ...
//     ImGui_ImplFISIR_RenderDrawData(cmdList, frameBuf->getFrameRenderPass(), ImGui::GetDrawData());
//     cmdList.EndRenderPass();
//
//   ImGui_ImplFISIR_Shutdown();
//
// 设计要点（为什么这么做）：
//  · 管线**惰性创建**：ImGui 必须和场景共用同一个 render pass（交换链的 render pass 是
//    loadOp=Clear，另开一个 pass 会把刚画好的画面再清一遍），而交换链在第一次 acquire 之前
//    还没有 framebuffer，所以只能在第一次 RenderDrawData 时才拿得到 render pass。
//  · 顶点直接上传 ImGui 的原始 ImDrawVert（stride 20 = pos(8) + uv(8) + col(4)），
//    靠 RHI 新增的 _UByte4Norm 顶点格式表达打包色，CPU 侧不做逐顶点转换。
//  · 顶点/索引缓冲只有一份、没做按帧在飞数环形分配 —— 调用方必须在**上一帧 GPU 工作已完成**
//    之后再调用 RenderDrawData（Nanite 示例放在 WaitFrameGPUIdle() 之后即可）。

#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#endif

struct ImDrawData;

namespace FISIR {
	class DynamicRHI;
	class RHIRenderCommandList;
	class RHIRenderPass;
}

// 创建 GPU 资源并上传字体图集。要求 rhi 已完成 Init()，且 ImGui context 已创建。
bool ImGui_ImplFISIR_Init(FISIR::DynamicRHI* rhi);

// 释放全部 GPU 资源。不销毁 ImGui context。
void ImGui_ImplFISIR_Shutdown();

// 在当前 render pass 内录制 ImGui 的全部 draw call（会自行设置 scissor / 管线 / 顶点缓冲）。
// renderPass 必须与 BeginRenderPass 用的是同一个 —— 管线在第一次调用时按它惰性创建。
// drawData 为 nullptr 或为空时什么都不做。
void ImGui_ImplFISIR_RenderDrawData(FISIR::RHIRenderCommandList& cmdList,
                                    FISIR::RHIRenderPass* renderPass,
                                    const ImDrawData* drawData);

// 悬停提示：与 ImGui::SetTooltip 同义，但把 tooltip **钉在当前窗口所在的视口**上。
//
// 为什么需要这个替代品（不是风格问题，是 Release 卡死的成因）：
//   ImGui 的 tooltip 是按鼠标位置摆放的，一旦它的矩形超出宿主视口，imgui.cpp 的
//   "Late create viewport if we don't fit within our current host viewport" 就会**迟到地**
//   给它新建一个平台窗口（`ViewportAllowPlatformMonitorExtend >= 0` 那段）。
//   本后端给每个平台视口配一条独立交换链 + 一套描述符堆 + 一条 ImGui 管线，于是
//   「鼠标每扫过一个控件」都要在主线程上建/销一个 OS 窗口和这些 GPU 资源，并与 RHI 线程
//   在同族队列上的 submit/present 互锁 —— 悬停几十次之后就卡死（实测 trace：
//   hwnd 复用、size=(379,32)/(413,64)/(452,64)… 全是 tooltip）。
//   先用 SetNextWindowViewport() 指定视口，imgui.cpp 里的 lock_viewport 分支就会跳过那段
//   「迟到建视口」，tooltip 留在宿主窗口里（与不开多视口时一致：超出部分被夹进窗口）。
//
// vendor/imgui **保持官方原样**（换一份官方 ImGui 就能直接编译链接），所以这条约束由本助手
// 在**调用点**保证：本工程里所有自绘 tooltip 都走这里；控件自带的内建提示（如 ColorEdit3 的
// ImGui::ColorTooltip，它走 BeginTooltipEx，后端拦不到）则在调用处关掉、换成本助手，见 NaniteUI.cpp。
//
// 调用时机与 SetTooltip 完全相同：IsItemHovered()（或 always-hover 条件）之后。
void ImGui_ImplFISIR_SetTooltip(const char* fmt, ...);

// 多视口：为「上一帧新拖出来的平台窗口」补建 RHI 视口/交换链。
//
// **必须在帧首调用**（上一帧 GPU 已完、尚未开始录制），例如紧跟在 WaitFrameGPUIdle() 之后。
// 原因：建交换链会走 VulkanSwapChain::init() —— 编译内嵌着色器、建交换链/纹理/帧缓冲/呈现管线，
// 还要分配命令页。RHI 的 Init() 是在那三个工作线程启动**之前**做这件事的；若改到帧中途的
// ImGui 渲染回调里做，就会和正在跑的工作线程互等 → 一拖出窗口立刻死锁（实测）。
// 所以 Platform_SwapBuffers 只把新视口登记下来并跳过本帧，由本函数在安全的时点补建。
void ImGui_ImplFISIR_PrepareViewportSwapChains();

#if defined(_WIN32)
// 每帧调用一次（在 ImGui::NewFrame() 之前）：同步鼠标位置 / 焦点 / 光标形状。
void ImGui_ImplFISIR_Win32_NewFrame(HWND hwnd);

// 窗口过程里转发消息。返回值与官方后端同义：true 表示「该消息已被后端处理，不必再传给
// DefWindowProc」；它**不代表** ImGui 想独占输入 —— 要判断是否让位给 UI，请查
// ImGui::GetIO().WantCaptureMouse / WantCaptureKeyboard。
bool ImGui_ImplFISIR_Win32_HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
