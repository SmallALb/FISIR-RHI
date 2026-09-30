#include "ImGui_Impl_FISIR.h"

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#include "imgui.h"

#include "Log/Logger.h"
#include "RHIBuffer.h"
#include "RHICommandList.h"
#include "RHIFence.h"
#include "RHIFrameBuffer.h"
#include "RHIPipeline.h"
#include "RHIRenderPass.h"
#include "RHISampler.h"
#include "RHIShader.h"
#include "RHISwapChain.h"
#include "RHITexture.h"
#include "RHIViewport.h"
#include "ShaderComplier.h"

// ════════════════════════════════════════════════════════════════════════════
// 内嵌着色器
//
// 故意写成字符串而不是放 Shader/ 目录按文件加载：后端属于 RHI 层，示例只要链接它就能用，
// 不需要再为它部署一份 shader 资源（也就不必给每个示例改 CMake 拷贝规则）。
//
// 顶点布局必须与 pipeline 里的 RHIVertexInputInfo 严格一致：
//   location 0 = pos  (R32G32_SFLOAT)     ← 对应 _Fvec2
//   location 1 = uv   (R32G32_SFLOAT)     ← 对应 _Fvec2
//   location 2 = col  (R8G8B8A8_UNORM)    ← 对应 _UByte4Norm
// 语义名用带编号的 TEXCOORD0/1/2：这样「按声明顺序编号」与「按语义序号编号」两种
// DXC 定位规则结果一致，不会因实现差异把属性错位。
// ════════════════════════════════════════════════════════════════════════════
static const wchar_t* ImGuiVertexShader = LR"(
[[vk::binding(0, 0)]] cbuffer ImGuiParams  : register(b0) {
    // row_major：C++ 侧按行主序填 16 个 float（见 BuildProjectionMatrix），
    // 配合 mul(行向量, M) 的约定，两边都不需要转置。
    row_major float4x4 ProjMatrix;
};

struct VSInput {
    float2 pos : TEXCOORD0;
    float2 uv  : TEXCOORD1;
    float4 col : TEXCOORD2;
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = mul(float4(input.pos, 0.0, 1.0), ProjMatrix);
    output.uv  = input.uv;
    output.col = input.col;
    return output;
}
)";

static const wchar_t* ImGuiPixelShader = LR"(
[[vk::binding(1, 0)]] Texture2D    g_FontTexture  : register(t1);
[[vk::binding(2, 0)]] SamplerState g_FontSampler  : register(s2);

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
};

float4 main(PSInput input) : SV_TARGET {
    // 字体图集是「白色 RGB + 覆盖率在 A」，顶点色直乘即可；
    // 配合的混合是直通 alpha（src=SrcAlpha, dst=OneMinusSrcAlpha），不是预乘。
    return input.col * g_FontTexture.Sample(g_FontSampler, input.uv);
}
)";

// ── 内部状态 ────────────────────────────────────────────────────────────────
namespace {

    // 提到最前面：下面的多视口回调要用它
    FISIR::DynamicRHI* g_Rhi = nullptr;

    // ════════════════════════════════════════════════════════════════════════
    // 多视口（multi-viewport）：把面板拖出主窗口 → 独立的 OS 窗口，由本后端自己渲染
    //
    // ImGui 提供两件事，缺一不可：
    //   · ImGuiConfigFlags_ViewportsEnable  —— 允许一个窗口脱离主视口，成为独立平台窗口；
    //   · ImGuiPlatformIO 的一组回调        —— 由后端负责建/管/渲染那些 OS 窗口。
    // 少了后端这半边，拖出去的面板会因为「没有窗口渲染它」而直接消失。
    //
    // 每个平台窗口 = 一个 Win32 窗口(HWND) + 一套独立的 RHI 视口/交换链 + 一次自己的
    // BeginRenderPass/Draw/End/present。主视口不在这里渲染（仍由宿主 Main.cpp 负责）。
    //
    // 视口对象池：RHI 只暴露 RHICreateViewport / RHIGetSwapChain，没有销毁接口，而交换链的
    // VkSurfaceKHR 绑在 HWND 上（销毁窗口会让 surface 失效）。所以这里**回收复用**：
    // ImGui 关掉一个平台窗口时只隐藏它并把 (HWND, 视口, 交换链) 放回池子，下次新建优先复用
    // 同一个 HWND —— 于是永远不需要销毁 surface，也不会留下悬挂的窗口句柄。
    // ════════════════════════════════════════════════════════════════════════

    const wchar_t* kViewportWindowClass = L"ImGuiFisirViewportWindow";

    struct FisirViewportData {
        HWND                 Hwnd        = nullptr;
        FISIR::RHIViewport*  RhiViewport = nullptr;
        FISIR::RHISwapChain* SwapChain   = nullptr;
        uint32_t             Width       = 0;
        uint32_t             Height      = 0;
        // RHI 要的不是裸 HWND，而是 RHIDisplay.h 里定义的那种句柄布局（与 Main.cpp 建主视口时一致），
        // 所以每个视口自己持有一份，生命周期跟着视口走。
        FISIR::Win32DisplayHandle win32Data{};
        // Renderer_RenderWindow 里 acquire 到的帧 id，交给 Platform_SwapBuffers 呈现
        uint32_t             PendingFrameID = UINT32_MAX;

        // ── 本视口最近用过的槽围栏 ────────────────────────────────────────
        // 释放本视口的渲染资源（顶点/索引/参数缓冲 + 描述符堆）之前必须确认 GPU 已经把
        // 「用过这些资源的那些帧」跑完。主视口的 WaitFrameGPUIdle() 覆盖不到独立视口，
        // 提前释放会让描述符堆的宿主缓冲地址被随后新建的另一种堆复用 —— 校验层报
        // VUID-11236/11228（预留区必须在别的命令缓冲里完全一致且同堆类型），随后设备丢失。
        // 这里只记围栏对象本身（每个槽一份、随交换链存活），不去读交换链内部的槽表：
        // 那张表会被 RHI 线程在重建交换链时整表换新，非握手窗口里读它会和重建赛跑。
        static constexpr int kMaxRecentSlotFences = 8;
        FISIR::RHIFence*     RecentSlotFences[kMaxRecentSlotFences]{};
        int                  RecentSlotFenceCount = 0;
    };

    // ── 每个视口一套渲染资源（挂在 ImGuiViewport::RendererUserData 上）────────────
    // 官方后端就是这么做的（如 ImGui_ImplDX11_ViewportData）：**顶点/索引/参数缓冲每视口一份**。
    // 本后端原先只有一份全局缓冲，前提是「调用方保证上一帧 GPU 已完成」；一旦多视口同帧渲染
    // （主窗口 + 拖出去的独立窗口），后者的上传会覆盖前者尚未被 GPU 消费的数据 —— 表现为 UI 错乱/空白。
    // 所以按视口分开，资源包也随参数缓冲各建一份（资源包绑定的就是该视口的参数缓冲）。
    struct FisirRendererData {
        FISIR::RHIBuffer* VertexBuffer = nullptr;
        FISIR::RHIBuffer* IndexBuffer  = nullptr;
        FISIR::RHIBuffer* ParamsBuffer = nullptr;
        FISIR::RHIResourcePackResult ResourcePack;   // { ParamsBuffer, FontTexture, FontSampler }
        uint32_t VertexCapacity = 0;
        uint32_t IndexCapacity  = 0;
    };

    std::vector<FisirViewportData*>* g_ViewportPool = nullptr;
    bool g_ViewportClassRegistered = false;

    // ══════════════════════════════════════════════════════════════════════════
    // 交互追踪（**临时诊断设施**，问题定位后整段删除）
    //
    // 独立窗口的点击/拖动问题没法靠"看画面"定位，而只有使用者能在真机上操作鼠标。
    // 所以把关键事件落到 exe 同目录的 imgui_viewport_trace.log：
    // 使用者跑一轮 → 关掉程序 → 开发侧读这个文件就能看到"消息有没有到、坐标是多少、
    // 我们喂给 ImGui 的是什么、ImGui 当时认为鼠标在哪个视口"。
    // 每 64 行 flush 一次 + 关键事件即时 flush，保证崩溃时也能留下大部分内容。
    // ══════════════════════════════════════════════════════════════════════════
    FILE* g_TraceFile = nullptr;
    int   g_TraceLines = 0;

    void TraceOpen() {
        if (g_TraceFile) return;
        char exePath[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
            std::string path(exePath);
            const size_t slash = path.find_last_of("\\/");
            path = (slash == std::string::npos) ? std::string("imgui_viewport_trace.log")
                                                : path.substr(0, slash + 1) + "imgui_viewport_trace.log";
            // _SH_DENYWR：只禁止别的进程写，**允许边跑边读** —— 诊断时要在程序还开着的时候
            // 读这个文件（fopen 的默认共享模式在这里会把读者挡在外面）。
            g_TraceFile = _fsopen(path.c_str(), "w", _SH_DENYWR);
        }
        if (!g_TraceFile) g_TraceFile = _fsopen("imgui_viewport_trace.log", "w", _SH_DENYWR);
    }

    void Trace(const char* fmt, ...) {
        if (!g_TraceFile || g_TraceLines > 40000) return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_TraceFile, fmt, ap);
        va_end(ap);
        fputc('\n', g_TraceFile);
        fflush(g_TraceFile);   // 出问题时要保证内容已经落盘
        ++g_TraceLines;
    }
    // 每视口渲染资源（定义在文件后部的 FisirRendererData 那一段）；渲染回调要用，先声明。
    FisirRendererData* GetOrCreateRendererData(ImGuiViewport* vp);
    void DestroyRendererData(ImGuiViewport* vp);
    // 上一帧新拖出来、还没建交换链的平台窗口。由 Platform_SwapBuffers 登记、
    // 由 ImGui_ImplFISIR_PrepareViewportSwapChains() 在帧首补建（见头文件里的原因说明）。
    std::vector<ImGuiViewport*>* g_PendingViewports = nullptr;

    // 已关闭视口、待释放的渲染资源：帧中途不能销毁（GPU 可能还在用），帧首统一回收。
    // 登记项带上「该视口最近用过的槽围栏」，回收前逐个等它们落定 —— 只靠「帧首」这个
    // 时间点是**不够**的：主视口链的 WaitFrameGPUIdle() 覆盖不到独立视口，而临时 tooltip
    // 视口会频繁建/销，资源一提前释放就会踩描述符堆预留区别名（VUID-11236/11228 → 设备丢失）。
    struct PendingRendererDestroy {
        FisirRendererData* rd{ nullptr };
        FISIR::RHIFence*   fences[FisirViewportData::kMaxRecentSlotFences]{};
        int                fenceCount{ 0 };
    };
    std::vector<PendingRendererDestroy>* g_PendingDestroyRendererData = nullptr;

    // 记录某视口本帧用过的槽围栏（去重；槽位数远小于容量）。
    void RecordRecentSlotFence(FisirViewportData* vd, FISIR::RHIFence* fence) {
        if (!vd || !fence) return;
        for (int i = 0; i < vd->RecentSlotFenceCount; ++i)
            if (vd->RecentSlotFences[i] == fence) return;
        if (vd->RecentSlotFenceCount >= FisirViewportData::kMaxRecentSlotFences) return;   // 防御：满了就不再记
        vd->RecentSlotFences[vd->RecentSlotFenceCount++] = fence;
    }

    // 等这批围栏对应的 GPU 工作全部结束。未提交过的围栏直接跳过：那说明没有在飞的 GPU 工作
    //（若也 wait()，会等一个永远不会被 signal 的围栏上）。
    void WaitFencesGPUIdle(FISIR::RHIFence* const* fences, int count) {
        for (int i = 0; i < count; ++i) {
            FISIR::RHIFence* fence = fences[i];
            if (fence && fence->isSubmited()) fence->wait();
        }
    }

    bool EnsureViewportWindowClass() {
        if (g_ViewportClassRegistered) return true;
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = DefWindowProcW;   // 消息由 ImGui 的 WndProc 钩子转发，这里用默认即可
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kViewportWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Error("[ImGui] RegisterClassExW failed (err {})", GetLastError());
            return false;
        }
        g_ViewportClassRegistered = true;
        return true;
    }

    BOOL CALLBACK CollectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
        ImVector<ImGuiPlatformMonitor>* monitors = (ImVector<ImGuiPlatformMonitor>*)data;
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &mi)) return TRUE;
        ImGuiPlatformMonitor mon;
        mon.MainPos  = ImVec2((float)mi.rcMonitor.left, (float)mi.rcMonitor.top);
        mon.MainSize = ImVec2((float)(mi.rcMonitor.right - mi.rcMonitor.left),
                              (float)(mi.rcMonitor.bottom - mi.rcMonitor.top));
        mon.WorkPos  = ImVec2((float)mi.rcWork.left, (float)mi.rcWork.top);
        mon.WorkSize = ImVec2((float)(mi.rcWork.right - mi.rcWork.left),
                              (float)(mi.rcWork.bottom - mi.rcWork.top));
        mon.DpiScale = 1.0f;   // 本示例不做 DPI 缩放
        monitors->push_back(mon);
        return TRUE;
    }

    // 填 ImGuiPlatformIO::Monitors。**开了 ViewportsEnable 就必须有**：ImGui 在
    // imgui.cpp 里断言 "Platform init didn't setup Monitors list?"（平台窗口要靠它
    // 知道各显示器的工作区来做贴边/最大化）。官方 Win32 后端同样有这么一步。
    void UpdateMonitors() {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        pio.Monitors.resize(0);
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, (LPARAM)&pio.Monitors);
        if (pio.Monitors.Size == 0) {   // 兜底：一台都没有会给一个默认的，避免断言
            ImGuiPlatformMonitor mon;
            mon.MainPos  = ImVec2(0.0f, 0.0f);
            mon.MainSize = ImVec2((float)GetSystemMetrics(SM_CXSCREEN), (float)GetSystemMetrics(SM_CYSCREEN));
            mon.WorkPos  = mon.MainPos;
            mon.WorkSize = mon.MainSize;
            mon.DpiScale = 1.0f;
            pio.Monitors.push_back(mon);
        }
    }

    // 视口坐标一律用**桌面绝对坐标**（与 ImGui 的约定一致）。
    // 曾经折中成"相对主窗口客户区原点"，那是为了让 ScreenToClient 得到的鼠标坐标能对上；
    // 代价是平台窗口位置被多加了一次原点、被摆到屏幕外（实测独立窗口只剩 8px 露在边上）。
    // 正解是两边都用桌面坐标：视口位置用 ClientToScreen，鼠标也喂桌面坐标（见 Win32_NewFrame）。
    HWND g_MainHwnd = nullptr;

    // 视口的 HWND：独立视口存在 PlatformUserData 里；主视口只有 PlatformHandle（宿主建窗口）
    HWND HwndOf(ImGuiViewport* vp) {
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        return vd ? vd->Hwnd : (HWND)vp->PlatformHandle;
    }

    // 平台窗口的消息过程：把消息转给**同一个** ImGui handler（官方后端的做法 —— 一个 handler，
    // 由应用装在每个窗口上）。不这么做的话：主窗口的 WndProc 才转发消息，独立窗口里的
    // 点击/键盘事件根本到不了 ImGui（鼠标位置是全局轮询的，所以只有"位置对、点不动"这个现象）。
    LRESULT CALLBACK ViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        // ── 这两条是"把面板拖回主窗口"的前提，必须在通用 handler 之前处理 ──
        // 官方 Win32 后端的平台窗口过程里也是这么写的（imgui_impl_win32.cpp 的
        // WndProcHandler_PlatformWindow），通用 handler 返回 bool、没法返回 HTTRANSPARENT。
        if (msg == WM_NCHITTEST || msg == WM_MOUSEACTIVATE) {
            if (ImGuiViewport* vp = ImGui::FindViewportByPlatformHandle((void*)hwnd)) {
                if (msg == WM_NCHITTEST) {
                    // 拖动一个平台窗口时 ImGui 会给它打上 NoInputs。这时对它回 HTTRANSPARENT，
                    // Windows 就把命中测试和鼠标消息交给它**后面**的那个窗口（同一线程的窗口
                    // 才会这样穿透，我们的主窗口和所有视口窗口都是同一线程建的）。
                    // 于是 backend 才能正确报出"鼠标其实在主窗口上"（见 Win32_NewFrame），
                    // ImGui 松手时才会把窗口合并回主视口 —— UpdateTryMergeWindowIntoHostViewport()
                    // 第一行就是 if (window->Viewport == viewport) return false;，
                    // 也就是要求那一刻 MouseViewport **不是**被拖窗口自己的视口。
                    // 少了这一条，被拖的窗口永远压在最上面 ⇒ 拖出去就再也拖不回来。
                    if (vp->Flags & ImGuiViewportFlags_NoInputs) return HTTRANSPARENT;
                }
                else if (vp->Flags & ImGuiViewportFlags_NoFocusOnClick) {
                    return MA_NOACTIVATE;   // ImGui 明确要求这个视口被点击时不要抢焦点
                }
            }
        }
        if (ImGui_ImplFISIR_Win32_HandleMessage(hwnd, msg, wParam, lParam)) return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void Platform_CreateWindow(ImGuiViewport* vp) {        if (vp->PlatformUserData) return;
        if (!EnsureViewportWindowClass()) return;

        FisirViewportData* vd = nullptr;
        if (g_ViewportPool && !g_ViewportPool->empty()) {   // 优先复用池子里的（复用 HWND ⇒ 复用 surface）
            vd = g_ViewportPool->back();
            g_ViewportPool->pop_back();
        }
        if (!vd) {
            vd = new FisirViewportData();
            // 无边框（WS_POPUP）：ImGui 的平台窗口本来就是无边框的（靠面板标题栏拖动），
            // 而且这样「窗口尺寸 == 客户区尺寸」—— 带边框时 SetWindowPos 设的是含边框的尺寸，
            // 客户区会比 vp->Size 小一圈，现象就是右边/下边被裁掉一小条且拉不自适应。
            // WS_EX_TOOLWINDOW：不占任务栏。
            vd->Hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kViewportWindowClass, L"ImGui Viewport",
                WS_POPUP, (int)vp->Pos.x, (int)vp->Pos.y, (int)vp->Size.x, (int)vp->Size.y,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!vd->Hwnd) { Error("[ImGui] CreateWindowExW failed (err {})", GetLastError()); delete vd; return; }
            // 子类化：让这个窗口的消息也进 ImGui（见 ViewportWndProc 的注释）
            SetWindowLongPtrW(vd->Hwnd, GWLP_USERDATA, (LONG_PTR)vd);
            SetWindowLongPtrW(vd->Hwnd, GWLP_WNDPROC, (LONG_PTR)ViewportWndProc);
        }

        vp->PlatformHandle   = vd->Hwnd;
        vp->PlatformUserData = vd;
    }

    void Platform_DestroyWindow(ImGuiViewport* vp) {
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        if (!vd) return;
        // 捕获必须转移（官方后端在 DestroyWindow 里做同一件事）：我们只是把窗口藏起来放进
        // 对象池，而隐藏的窗口收不到消息 —— 捕获留在它身上就等于 WM_LBUTTONUP 永远收不到。
        if (GetCapture() == vd->Hwnd) {
            ReleaseCapture();
            if (g_MainHwnd) SetCapture(g_MainHwnd);
        }
        ShowWindow(vd->Hwnd, SW_HIDE);          // 只隐藏：见上面的「视口对象池」说明
        if (g_ViewportPool) g_ViewportPool->push_back(vd);
        vp->PlatformUserData = nullptr;
        vp->PlatformHandle   = nullptr;
    }

    void Platform_ShowWindow(ImGuiViewport* vp) {
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        if (vd) ShowWindow(vd->Hwnd, SW_SHOWNA);
    }

    void Platform_SetWindowPos(ImGuiViewport* vp, ImVec2 pos) {
        HWND hwnd = HwndOf(vp);
        if (!hwnd) return;
        Trace("SetWindowPos  vp=0x%llx hwnd=0x%llx pos=(%.0f,%.0f)",
              (unsigned long long)(uintptr_t)vp, (unsigned long long)(uintptr_t)hwnd, pos.x, pos.y);
        SetWindowPos(hwnd, nullptr, (int)pos.x, (int)pos.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);   // pos 已是桌面坐标
    }

    ImVec2 Platform_GetWindowPos(ImGuiViewport* vp) {
        HWND hwnd = HwndOf(vp);
        if (!hwnd) return ImVec2(0, 0);
        POINT p{ 0, 0 };
        ClientToScreen(hwnd, &p);                                   // 桌面坐标，与 ImGui 约定一致
        return ImVec2((float)p.x, (float)p.y);
    }

    void Platform_SetWindowSize(ImGuiViewport* vp, ImVec2 size) {
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        if (!vd) return;
        Trace("SetWindowSize vp=0x%llx hwnd=0x%llx size=(%.0f,%.0f)",
              (unsigned long long)(uintptr_t)vp, (unsigned long long)(uintptr_t)vd->Hwnd, size.x, size.y);
        SetWindowPos(vd->Hwnd, nullptr, 0, 0, (int)size.x, (int)size.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ImVec2 Platform_GetWindowSize(ImGuiViewport* vp) {
        HWND hwnd = HwndOf(vp);
        if (!hwnd) return ImVec2(0, 0);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        return ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    }

    void Platform_SetWindowFocus(ImGuiViewport* vp)     { SetForegroundWindow((HWND)vp->PlatformHandle); }
    bool Platform_GetWindowFocus(ImGuiViewport* vp)     { return GetForegroundWindow() == (HWND)vp->PlatformHandle; }
    bool Platform_GetWindowMinimized(ImGuiViewport* vp) { return IsIconic((HWND)vp->PlatformHandle) != 0; }
    void Platform_SetWindowTitle(ImGuiViewport* vp, const char* title) { SetWindowTextA((HWND)vp->PlatformHandle, title); }
    void Platform_RenderWindow(ImGuiViewport*, void*) {
    }   // 真正渲染放在 Renderer_RenderWindow

    // 取/建该视口的交换链；尺寸变化时只调 setViewportResize（RHI 会自行重建交换链）
    FISIR::RHISwapChain* AcquireViewportSwapChain(FisirViewportData* vd, uint32_t w, uint32_t h) {
        if (w == 0 || h == 0 || !g_Rhi) return nullptr;
        if (!vd->RhiViewport) {
            vd->win32Data = { GetModuleHandleW(nullptr), vd->Hwnd };
            // 拖出去的独立窗口 = 又一个 Win32Window 呈现设备（句柄布局见 RHIDisplay.h）
            vd->RhiViewport = g_Rhi->RHICreateViewport(w, h, FISIR::TextureCOLORType::RGBA_8,
                                                       FISIR::DisplayDeviceType::Win32Window,
                                                       (void*)&vd->win32Data);
            if (!vd->RhiViewport) { Error("[ImGui] RHICreateViewport failed for a detached viewport"); return nullptr; }
            vd->SwapChain = g_Rhi->RHIGetSwapChain(vd->RhiViewport);
            vd->Width = w; vd->Height = h;
        }
        else if (vd->Width != w || vd->Height != h) {
            vd->RhiViewport->setViewportResize(h, w);   // 注意参数序是 (height, width)
            vd->Width = w; vd->Height = h;
        }
        return vd->SwapChain;
    }

    // ── 渲染侧回调（与官方后端同构：Renderer_* 负责画，Platform_SwapBuffers 只负责呈现）──
    //   Renderer_CreateWindow  —— 空实现（资源一律留到帧首建，见函数内注释）
    //   Renderer_DestroyWindow —— 释放
    //   Renderer_RenderWindow  —— acquire + 录制 + 提交（帧首已确保交换链/管线/资源就绪）
    // 这样拆分后：Platform_SwapBuffers 只剩 present，同步逻辑一目了然；
    // 而且渲染资源与平台窗口的生命周期一一对应，不会互相串用。
    void Renderer_CreateWindow(ImGuiViewport* vp) {
        // **故意什么都不做**：这里的 vp 是刚建出来的平台视口，下面 Renderer_RenderWindow 会发现
        // 「没有交换链」而把它登记进 g_PendingViewports，真正的资源（交换链 / ImGui 管线 /
        // 顶点·索引·参数缓冲 / 资源包=两个描述符堆）统一由 ImGui_ImplFISIR_PrepareViewportSwapChains()
        // 在**帧首**建。
        // 原先在这里调 GetOrCreateRendererData() 会在帧中途（UpdatePlatformWindows 里）new 描述符堆，
        // 与 RHI 头文件里「GPU 资源只在帧首建」的既有结论相矛盾 —— 那时 RHI 工作线程正在翻译/提交
        // 本帧的页面，两边会去抢描述符堆的宿主缓冲（历史上正是 VUID-11236/11228 → 设备丢失的入口）。
        (void)vp;
    }

    void Renderer_DestroyWindow(ImGuiViewport* vp) {
        // **不能在这里立刻释放**：这是帧中途（UpdatePlatformWindows 里），GPU 很可能还在用这些
        // 缓冲/资源包 —— 立刻销毁正是「关闭独立窗口就崩」的原因。挂到待释放表，帧首由
        // PrepareViewportSwapChains 统一回收，**并且回收前会等本视口最近用过的槽围栏落定**。
        if (!vp) return;
        if (vp->RendererUserData && g_PendingDestroyRendererData) {
            PendingRendererDestroy entry;
            entry.rd = (FisirRendererData*)vp->RendererUserData;
            if (FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData) {
                entry.fenceCount = vd->RecentSlotFenceCount;
                for (int i = 0; i < entry.fenceCount; ++i) entry.fences[i] = vd->RecentSlotFences[i];
            }
            g_PendingDestroyRendererData->push_back(entry);
        }
        vp->RendererUserData = nullptr;
        if (g_PendingViewports)   // 该视口若还挂在待建表里也一并摘掉，别为已关闭的窗口建资源
            for (size_t i = 0; i < g_PendingViewports->size(); ) {
                if ((*g_PendingViewports)[i] == vp) (*g_PendingViewports)[i] = g_PendingViewports->back(), g_PendingViewports->pop_back();
                else ++i;
            }
    }

    void Renderer_RenderWindow(ImGuiViewport* vp, void*) {
        if (!g_Rhi || !vp->DrawData) return;
        if (vp == ImGui::GetMainViewport()) return;      // 主视口由宿主在它自己的 render pass 里画
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        if (!vd) return;

        // 本帧还没准备好：新建的平台窗口（还没有交换链），或本视口的窗口尺寸刚被拖动改变过。
        // 两种情况都只登记、跳过本帧，交给下一帧帧首的 ImGui_ImplFISIR_PrepareViewportSwapChains()
        // 统一处理 —— 「平台视口的交换链/GPU 资源只在帧首碰」这条规则必须真的成立：
        // 帧中途改尺寸会让 RHI 线程去 recreateSwapChain()（vkDeviceWaitIdle + 重建整条交换链），
        // 而此刻主线程就在同一个回调里取图，两者在 NVIDIA 驱动内部互锁（Release 下表现为卡死）。
        const uint32_t sizeW = (uint32_t)vp->Size.x;
        const uint32_t sizeH = (uint32_t)vp->Size.y;
        if (!vd->SwapChain || vd->Width != sizeW || vd->Height != sizeH) {
            if (g_PendingViewports) {
                bool queued = false;
                for (ImGuiViewport* p : *g_PendingViewports) if (p == vp) { queued = true; break; }
                if (!queued) g_PendingViewports->push_back(vp);
            }
            return;
        }

        // 帧首已把本视口备好（交换链尺寸就是上面比对的尺寸），这里只读、不再碰 RHI 的可变状态。
        FISIR::RHISwapChain* swapchain = vd->SwapChain;

        const uint32_t frameID = swapchain->acquireGetImageInfoID();
        if (frameID == FISIR::RHISwapChain::FAILEID) { vd->PendingFrameID = UINT32_MAX; return; }
        const FISIR::SwapChainGetImageInfo info = swapchain->getSwapChainGetImageInfo(frameID);
        // 记下本帧用的槽围栏：Renderer_DestroyWindow 之后回收本视口资源前要等它落定。
        // 这里读交换链是安全的（取图握手之后，RHI 线程不会同时重建交换链）。
        RecordRecentSlotFence(vd, info.finishFence);
        FISIR::RHIFrameBuffer* frameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);
        if (!frameBuf) { vd->PendingFrameID = UINT32_MAX; return; }

        FISIR::ClearValue clear{};   // 默认成员初值就是「清色 + 清深度」
        FISIR::RHIRenderCommandList cmdList(g_Rhi);
        cmdList.BeginRenderPass(frameBuf, 0, clear);
        // viewport/scissor 是动态状态，**每个 render pass 都必须显式设一次**：
        // 少了这两行校验层会报 vkCmdDrawIndexed "Dynamic viewport(s) ... were not provided
        // via calls to vkCmdSetViewport()"，而且会按残留的视口去画 —— 表现为 UI 被放大/错位。
        // 尺寸用本视口的（即该交换链的 extent），不是主窗口的。
        cmdList.SetViewPort(0.0f, 0.0f, (float)vp->Size.x, (float)vp->Size.y, 1.0f, 0.0f);
        cmdList.SetScissor((uint32_t)vp->Size.x, (uint32_t)vp->Size.y);
        ImGui_ImplFISIR_RenderDrawData(cmdList, frameBuf->getFrameRenderPass(), vp->DrawData);
        cmdList.EndRenderPass();
        // 呈现录成指令（必须在 End 之前）：由 RHI 线程在本页提交之后执行 vkQueuePresentKHR。
        // 绝不能在回调里自己 present —— 交换链是外部同步对象，acquire / present / 重建
        // 全在 RHI 线程，跨线程碰就会报 THREADING ERROR 并最终设备丢失。
        cmdList.Present(swapchain, frameID);
        cmdList.End(info.finishFence, { info.avaliable }, { info.renderFinish });
        vd->PendingFrameID = frameID;   // 登记本帧已提交（SwapBuffers 侧只负责清账）
    }

    void Platform_SwapBuffers(ImGuiViewport* vp, void*) {
        if (!g_Rhi) return;
        if (vp == ImGui::GetMainViewport()) return;      // 主视口由宿主呈现（见 Main.cpp）
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        if (!vd || !vd->SwapChain) return;
        if (vd->PendingFrameID == UINT32_MAX) return;    // 本帧没渲染成功

        // Renderer_RenderWindow 已经把这一帧（含 Present 指令）提交出去了；呈现由 RHI 线程
        // 在提交之后执行。这里只剩节流与清账。
        const FISIR::SwapChainGetImageInfo info = vd->SwapChain->getSwapChainGetImageInfo(vd->PendingFrameID);
        if (info.finishFence) info.finishFence->waitFenceSubmited();
        vd->PendingFrameID = UINT32_MAX;
    }


    // 所以 imconfig.h 必须把 ImDrawIdx 定成 32 位。用 16 位不会编译报错、也不会运行报错，
    // 只会因为 GPU 按 4 字节跨度读 2 字节数据而画出垃圾 —— 这里用 static_assert 把它变成编译期错误。
    static_assert(sizeof(ImDrawIdx) == 4,
        "ImDrawIdx must be 32-bit: FISIR RHI always binds the index buffer as VK_INDEX_TYPE_UINT32. "
        "Define 'ImDrawIdx unsigned int' in vendor/imgui/imconfig.h.");

    // 与 HLSL 的 row_major float4x4 对齐（64 字节）
    struct ImGuiParams {
        float ProjMatrix[16];
    };

    constexpr uint32_t kInitialVertexCount = 32768;
    constexpr uint32_t kInitialIndexCount = 65536;

    FISIR::RHIShader* g_VertexShader = nullptr;
    FISIR::RHIShader* g_PixelShader = nullptr;
    FISIR::RHITexture* g_FontTexture = nullptr;
    FISIR::RHISampler* g_FontSampler = nullptr;
    FISIR::RHIBuffer* g_FontUploadBuffer = nullptr;  // 字体上传中转（存活到 Shutdown）

    FISIR::RHIPipeline* g_Pipeline = nullptr;
    FISIR::RHIRenderPass* g_PipelineRenderPass = nullptr;  // 管线照哪个 render pass 建的

    // 像素坐标 → Vulkan NDC（x 右、y 下、z ∈ [0,1]）。
    // ImGui 也是 y 向下，故 y 不翻转；行主序写入，配 HLSL 的 row_major + mul(v, M)。
    void BuildProjectionMatrix(const ImDrawData* drawData, float out[16]) {
        const float L = drawData->DisplayPos.x;
        const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
        const float T = drawData->DisplayPos.y;
        const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;

        out[0] = 2.0f / (R - L);     out[1] = 0.0f;               out[2] = 0.0f;  out[3] = 0.0f;
        out[4] = 0.0f;               out[5] = 2.0f / (B - T);     out[6] = 0.0f;  out[7] = 0.0f;
        out[8] = 0.0f;               out[9] = 0.0f;               out[10] = 0.5f; out[11] = 0.0f;
        // y 平移必须是负数：Vulkan NDC 的 +y 朝下，而 Dear ImGui 官方后端的矩阵是按 D3D
        // 的「+y 朝上」写的（那里是 (T+B)/(B-T)，正好反号）。符号写错不会报任何错，
        // 只会让整个 UI 落到屏幕外（y_ndc 恒 > 1，实测 [1.11, 2.02]）—— 已踩过。
        out[12] = (R + L) / (L - R); out[13] = (T + B) / (T - B); out[14] = 0.5f; out[15] = 1.0f;
    }

    bool CreateVertexIndexBuffers(FisirRendererData* rd, uint32_t vertexCount, uint32_t indexCount) {
        FISIR::BufferInfo vertexInfo{
            .data_CPU = nullptr,
            .size = (uint64_t)vertexCount * sizeof(ImDrawVert),
            .bufferlayout = FISIR::VertexBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
        };
        FISIR::BufferInfo indexInfo{
            .data_CPU = nullptr,
            .size = (uint64_t)indexCount * sizeof(ImDrawIdx),
            .bufferlayout = FISIR::IndexBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
        };

        rd->VertexBuffer = g_Rhi->RHICreateBuffer(vertexInfo);
        rd->IndexBuffer = g_Rhi->RHICreateBuffer(indexInfo);
        if (!rd->VertexBuffer || !rd->IndexBuffer) {
            Error("[ImGui] Failed to create vertex/index buffer");
            return false;
        }
        rd->VertexCapacity = vertexCount;
        rd->IndexCapacity = indexCount;
        return true;
    }

    // 容量不够就整体重建（每视口一份缓冲，无环形分配）。
    bool EnsureVertexIndexCapacity(FisirRendererData* rd, uint32_t vertexCount, uint32_t indexCount) {
        if (vertexCount <= rd->VertexCapacity && indexCount <= rd->IndexCapacity) return true;

        const uint32_t newVertexCount = (vertexCount > rd->VertexCapacity) ? vertexCount : rd->VertexCapacity;
        const uint32_t newIndexCount = (indexCount > rd->IndexCapacity) ? indexCount : rd->IndexCapacity;

        if (rd->VertexBuffer) { g_Rhi->RHIDestroyBuffer(rd->VertexBuffer); rd->VertexBuffer = nullptr; }
        if (rd->IndexBuffer) { g_Rhi->RHIDestroyBuffer(rd->IndexBuffer); rd->IndexBuffer = nullptr; }
        rd->VertexCapacity = rd->IndexCapacity = 0;
        Debug("[ImGui] viewport draw data outgrew buffers -> rebuild to {} verts / {} indices", newVertexCount, newIndexCount);
        return CreateVertexIndexBuffers(rd, newVertexCount, newIndexCount);
    }

    // 把各 DrawList 顺序追加进大缓冲。
    // 索引在这里按 DrawCmd 重定基：RHI 的 DrawIndex 不带 vertexOffset（vkCmdDrawIndexed 的
    // vertexOffset 恒为 0），所以要把「本段在合并后顶点缓冲里的绝对基址」加进每个索引。
    // 用 ImDrawCmd::VtxOffset 而不是 0 是因为 ImGui 在 16 位索引下会把超过 64K 顶点的
    // DrawList 切成多段，段内索引相对段首。
    void UploadDrawData(FisirRendererData* rd, const ImDrawData* drawData) {
        ImDrawVert* vertexDst = static_cast<ImDrawVert*>(rd->VertexBuffer->getBufferData());
        ImDrawIdx* indexDst = static_cast<ImDrawIdx*>(rd->IndexBuffer->getBufferData());

        uint32_t vertexBase = 0;
        uint32_t indexBase = 0;
        for (int n = 0; n < drawData->CmdListsCount; ++n) {
            const ImDrawList* list = drawData->CmdLists[n];
            const uint32_t vertexCount = (uint32_t)list->VtxBuffer.Size;

            memcpy(vertexDst + vertexBase, list->VtxBuffer.Data, (size_t)vertexCount * sizeof(ImDrawVert));

            for (int c = 0; c < list->CmdBuffer.Size; ++c) {
                const ImDrawCmd& cmd = list->CmdBuffer[c];
                if (cmd.UserCallback != nullptr) continue;   // 回调不产生几何
                const uint32_t cmdVertexBase = vertexBase + cmd.VtxOffset;
                for (uint32_t i = 0; i < cmd.ElemCount; ++i)
                    indexDst[indexBase + cmd.IdxOffset + i] =
                        (ImDrawIdx)(list->IdxBuffer.Data[cmd.IdxOffset + i] + cmdVertexBase);
            }

            vertexBase += vertexCount;
            indexBase += (uint32_t)list->IdxBuffer.Size;
        }
    }

    bool CreatePipeline(FISIR::RHIRenderPass* renderPass) {
        // 绑定顺序必须与资源包列表顺序一致（堆式描述符按声明顺序排布偏移）：
        //   b0 ImGuiParams -> t1 字体图集 -> s2 采样器
        FISIR::RHIPipelineDescribeInfo describeInfo{
            {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage},
            {1, 1, FISIR::RHIDescriptorTyp::SamplerImage,  FISIR::RHIUsingStage::FragmentShaderStage},
            {2, 1, FISIR::RHIDescriptorTyp::Sampler,       FISIR::RHIUsingStage::FragmentShaderStage},
        };

        FISIR::RHIPipelineState state{
            .describeInfo = describeInfo,
            .vertexInfo = FISIR::RHIVertexInputInfo({ FISIR::_Fvec2, FISIR::_Fvec2, FISIR::_UByte4Norm }),
            .topologyType = FISIR::TopologyType::Triangle,
            .rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
            .multiSampleState = { .SamplerBit = 1 },
            // ImGui 按提交顺序覆盖绘制，不需要深度；交换链 render pass 本身也没有深度附件。
            .depthStencilState = { false, false, false, 0.0f, 1.0f, FISIR::_Always_ },
            .colorblendState = {
                .ColorBlenEnable = true,
                .UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_ | FISIR::_A_PASS),
                .SrcColorBlend = FISIR::BlendFactor::SrcAlpha,
                .DstColorBlend = FISIR::BlendFactor::OneMinusSrcAlpha,
                .ColorBlendOp = FISIR::BlendOp::Add,
                .SrcAlphaBlend = FISIR::BlendFactor::One,
                .DstAlphaBlend = FISIR::BlendFactor::OneMinusSrcAlpha,
                .AlphaBlendOp = FISIR::BlendOp::Add,
            },
            .renderpass = renderPass,
        };
        state.Shaders[FISIR::__VERTEXSHADER__] = g_VertexShader;
        state.Shaders[FISIR::__FRAGMENTSHADER__] = g_PixelShader;

        g_Pipeline = g_Rhi->RHICreatePipeline(state);
        if (!g_Pipeline) {
            Error("[ImGui] Pipeline creation FAILED (render pass 0x{:x})", (size_t)renderPass);
            return false;
        }
        // VulkanPipeline 内部不检查 vkCreateGraphicsPipelines 的返回值：失败时对象仍非空，
        // 只是 handle 为 VK_NULL_HANDLE，绑定它不会报错、只会什么都不画。这里兜住这种情况。
        if (!g_Pipeline->getPipelineHandle()) {
            Error("[ImGui] vkCreateGraphicsPipelines returned a NULL handle (render pass 0x{:x})", (size_t)renderPass);
            return false;
        }
        g_PipelineRenderPass = renderPass;
        Info("[ImGui] Pipeline ready (handle 0x{:x})", (size_t)g_Pipeline->getPipelineHandle());
        return true;
    }

    // 每个 render pass 一条 ImGui 管线。多视口下每个独立窗口的交换链都有自己的 render pass，
    // 而管线与 render pass 是绑定的（RHICreatePipeline 的缓存键里就含它），所以必须分开建、分开缓存。
    std::vector<std::pair<FISIR::RHIRenderPass*, FISIR::RHIPipeline*>> g_PipelinePerPass;

    FISIR::RHIPipeline* GetPipelineFor(FISIR::RHIRenderPass* renderPass) {
        for (auto& entry : g_PipelinePerPass)
            if (entry.first == renderPass) return entry.second;
        if (!CreatePipeline(renderPass)) return nullptr;
        g_PipelinePerPass.push_back({ renderPass, g_Pipeline });
        return g_Pipeline;
    }

    // 参数缓冲（投影矩阵）+ 资源包：每个视口一份（资源包绑定的就是这个视口的参数缓冲）
    bool CreateParamsAndPack(FisirRendererData* rd) {
        FISIR::BufferInfo paramsInfo{
            .data_CPU = nullptr,
            .size = sizeof(ImGuiParams),
            .bufferlayout = FISIR::UniformBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
        };
        rd->ParamsBuffer = g_Rhi->RHICreateBuffer(paramsInfo);
        if (!rd->ParamsBuffer) { Error("[ImGui] Params buffer creation FAILED"); return false; }
        rd->ResourcePack = g_Rhi->RHICreateResourcePack({ rd->ParamsBuffer, g_FontTexture, g_FontSampler });
        return true;
    }

    // 取该视口的渲染资源；没有就现建（主视口与平台窗口共用这条路径）。
    FisirRendererData* GetOrCreateRendererData(ImGuiViewport* vp) {
        if (!vp || !g_Rhi) return nullptr;
        FisirRendererData* rd = (FisirRendererData*)vp->RendererUserData;
        if (rd && rd->ParamsBuffer) return rd;
        if (!rd) { rd = new FisirRendererData(); vp->RendererUserData = rd; }
        if (!CreateParamsAndPack(rd)) return nullptr;
        if (!EnsureVertexIndexCapacity(rd, kInitialVertexCount, kInitialIndexCount)) return nullptr;
        Debug("[ImGui] renderer data ready for viewport 0x{:x}", (size_t)vp);
        return rd;
    }

    void DestroyRendererDataRaw(FisirRendererData* rd) {
        if (!rd) return;
        if (rd->ResourcePack.ResourcePack || rd->ResourcePack.SamplerPack) g_Rhi->RHIDestroyResourcePack(rd->ResourcePack);
        if (rd->VertexBuffer) g_Rhi->RHIDestroyBuffer(rd->VertexBuffer);
        if (rd->IndexBuffer)  g_Rhi->RHIDestroyBuffer(rd->IndexBuffer);
        if (rd->ParamsBuffer) g_Rhi->RHIDestroyBuffer(rd->ParamsBuffer);
        delete rd;
    }

    void DestroyRendererData(ImGuiViewport* vp) {
        if (!vp) return;
        DestroyRendererDataRaw((FisirRendererData*)vp->RendererUserData);
        vp->RendererUserData = nullptr;
    }

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// 渲染后端
// ════════════════════════════════════════════════════════════════════════════

bool ImGui_ImplFISIR_Init(FISIR::DynamicRHI* rhi) {
    if (!rhi) { Error("[ImGui] Init: rhi is null"); return false; }
    if (!ImGui::GetCurrentContext()) { Error("[ImGui] Init: ImGui context not created"); return false; }
    if (g_Rhi) { Warn("[ImGui] Init called twice, ignored"); return true; }
    g_Rhi = rhi;

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_fisir";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    // 允许窗口停靠 / 拖出成浮动窗口（docking 分支才有这两个 flag）。
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // ── 多视口：把面板拖出主窗口，变成系统里一个**独立的 OS 窗口** ──
    // 需要三样：ViewportsEnable + 后端声明支持平台视口 + 下面这组回调。
    // 少任何一样，拖出去的面板都会因为「没有窗口渲染它」而消失。
    io.ConfigFlags  |= ImGuiConfigFlags_ViewportsEnable;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
    // 我们**主动**告诉 ImGui「鼠标此刻压在哪个平台窗口上」（见 Win32_NewFrame 里的
    // AddMouseViewportEvent）。不声明这个能力，ImGui 就只能靠"最后聚焦时间戳"去猜视口：
    // 见 imgui.cpp 的 FindHoveredViewportFromPlatformWindowStack()。
    io.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.Platform_CreateWindow       = Platform_CreateWindow;
    platformIO.Platform_DestroyWindow      = Platform_DestroyWindow;
    platformIO.Platform_ShowWindow         = Platform_ShowWindow;
    platformIO.Platform_SetWindowPos       = Platform_SetWindowPos;
    platformIO.Platform_GetWindowPos       = Platform_GetWindowPos;
    platformIO.Platform_SetWindowSize      = Platform_SetWindowSize;
    platformIO.Platform_GetWindowSize      = Platform_GetWindowSize;
    platformIO.Platform_SetWindowFocus     = Platform_SetWindowFocus;
    platformIO.Platform_GetWindowFocus     = Platform_GetWindowFocus;
    platformIO.Platform_GetWindowMinimized = Platform_GetWindowMinimized;
    platformIO.Platform_SetWindowTitle     = Platform_SetWindowTitle;
    platformIO.Platform_RenderWindow       = Platform_RenderWindow;
    platformIO.Platform_SwapBuffers        = Platform_SwapBuffers;
    // 渲染侧回调（官方后端的同构拆分）：Renderer_* 负责建/画/销毁该视口的渲染资源，
    // Platform_SwapBuffers 只负责呈现。**这三个必须挂上**，否则 ImGui 不会调 Renderer_RenderWindow，
    // 而 SwapBuffers 里已经没有渲染逻辑了 → 独立窗口会一直空白。
    platformIO.Renderer_CreateWindow       = Renderer_CreateWindow;
    platformIO.Renderer_DestroyWindow      = Renderer_DestroyWindow;
    platformIO.Renderer_RenderWindow       = Renderer_RenderWindow;
    if (!g_ViewportPool) g_ViewportPool = new std::vector<FisirViewportData*>();
    // 必须一起分配：Platform_SwapBuffers 靠它登记"待建交换链"的新视口，
    // 漏了的话那句 if (g_PendingViewports) 永远是 false —— 视口永远拿不到交换链，
    // 独立窗口就一直空白（实测踩过：gate 日志里 hasSwapChain 恒为 0）。
    if (!g_PendingViewports) g_PendingViewports = new std::vector<ImGuiViewport*>();
    // 待释放的每视口渲染资源（Renderer_DestroyWindow 只登记围栏，帧首等 GPU 完成后再回收）
    if (!g_PendingDestroyRendererData) g_PendingDestroyRendererData = new std::vector<PendingRendererDestroy>();
    TraceOpen();   // 交互追踪文件（临时诊断，见 TraceOpen 的注释）
    Trace("=== ImGui FISIR backend init (multi-viewport interaction trace) ===");
    UpdateMonitors();   // ViewportsEnable 的硬性前提，见 UpdateMonitors 的注释

    // ── 字体图集 → 纹理 ──
    unsigned char* pixels = nullptr;
    int texWidth = 0, texHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texWidth, &texHeight);
    if (!pixels || texWidth <= 0 || texHeight <= 0) {
        Error("[ImGui] Font atlas is empty");
        return false;
    }

    FISIR::TextureInfo fontTextureInfo{
        .size = { (uint32_t)texHeight, (uint32_t)texWidth, 1 },   // TextureSize 字段序 {height, width, depth}
        .colorType = FISIR::TextureCOLORType::RGBA_8,
        .type = FISIR::TextureType::TEXTURE2D,
        .useFor = FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForTransferDst,
        .mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,       // 0 = 非 MSAA（本 RHI 约定）
    };
    g_FontTexture = rhi->RHICreateTexture(fontTextureInfo);
    if (!g_FontTexture) { Error("[ImGui] Font texture creation FAILED"); return false; }

    FISIR::BufferInfo uploadInfo{
        .data_CPU = pixels,
        .size = (uint64_t)texWidth * texHeight * 4,
        .bufferlayout = FISIR::TransferSrcBuffer,
        .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    g_FontUploadBuffer = rhi->RHICreateBuffer(uploadInfo);

    {
        // 一次性上传提交：Undefined → TransferDstOptimal → 拷贝 → ShaderReadOnlyOptimal。
        // 必须赶在建资源包之前完成 —— 采样图像描述符里的 imageLayout 取的是纹理当前布局。
        FISIR::RHIRenderCommandList uploadList(rhi);
        FISIR::RHITexture* textureArray[] = { g_FontTexture };
        uploadList.TransitionTextures(textureArray, 1,
            FISIR::ResourceAccess::Undefined, FISIR::ResourceAccess::TransferDst,
            FISIR::TextureLayout::Undefined, FISIR::TextureLayout::TransferDstOptimal,
            FISIR::RHIUsingStage::NoneStage, FISIR::RHIUsingStage::PipelineTransferStage);
        uploadList.CopyToTexture(g_FontUploadBuffer, g_FontTexture, 0, 0, 1, 0, { 0, 0, 0 }, fontTextureInfo.size);
        uploadList.TransitionTextures(textureArray, 1,
            FISIR::ResourceAccess::TransferDst, FISIR::ResourceAccess::ShaderReadOnly,
            FISIR::TextureLayout::TransferDstOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
            FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);

        FISIR::RHIFence* fence = rhi->RHICreateFence();
        uploadList.End(fence);
        fence->wait();
        rhi->RHIDestroyFence(fence);
    }
    // 后端恒用字体图集，这里只是避免 TexID 为空触发 ImGui 断言；自定义贴图未支持。
    io.Fonts->SetTexID((ImTextureID)(uintptr_t)1);

    // ── 采样器：字体是二值位图，线性过滤 + CLAMP 防止相邻字形渗色 ──
    FISIR::SamplerInfo samplerInfo{};
    samplerInfo.enlagerFilter = FISIR::SamplerFilter::LINEAR;
    samplerInfo.minFilter = FISIR::SamplerFilter::LINEAR;
    samplerInfo.mipMapMode = FISIR::SamplerFilter::NEAREST;
    samplerInfo.u = samplerInfo.v = samplerInfo.w = FISIR::SamplerOverFoundMode::CLAMP_TO_EDGE;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLop = 0.0f;
    samplerInfo.maxLop = 0.0f;
    g_FontSampler = rhi->RHICreateSampler(samplerInfo);
    if (!g_FontSampler) { Error("[ImGui] Font sampler creation FAILED"); return false; }

    // ── 每个视口一套渲染资源：顶点/索引/参数缓冲都在 GetOrCreateRendererData 里按视口创建
    //    （主视口在第一次 RenderDrawData 时、平台窗口在帧首的 PrepareViewportSwapChains 里），
    //    这里不再建全局的。 ──

    // ── 着色器 ──
    FISIR::ShaderComplier vertexCompiler;
    vertexCompiler.compileShader(ImGuiVertexShader, wcslen(ImGuiVertexShader) * sizeof(wchar_t), L"main", L"vs_6_0");
    g_VertexShader = rhi->RHICreateShader(FISIR::ShaderTYP::__VERTEXSHADER__, "main",
        vertexCompiler.getShaderData(), vertexCompiler.getShaderDataSize());
    if (!g_VertexShader) { Error("[ImGui] Vertex shader creation FAILED"); return false; }

    FISIR::ShaderComplier pixelCompiler;
    pixelCompiler.compileShader(ImGuiPixelShader, wcslen(ImGuiPixelShader) * sizeof(wchar_t), L"main", L"ps_6_0");
    g_PixelShader = rhi->RHICreateShader(FISIR::ShaderTYP::__FRAGMENTSHADER__, "main",
        pixelCompiler.getShaderData(), pixelCompiler.getShaderDataSize());
    if (!g_PixelShader) { Error("[ImGui] Pixel shader creation FAILED"); return false; }

    // 管线/资源包留到第一次 RenderDrawData —— 那时才拿得到交换链的 render pass。
    Info("[ImGui] Backend ready (font atlas {}x{}, {} verts / {} indices capacity)",
        texWidth, texHeight, kInitialVertexCount, kInitialIndexCount);
    return true;
}

void ImGui_ImplFISIR_SetTooltip(const char* fmt, ...) {
    if (!ImGui::GetCurrentContext()) return;

    // 把 tooltip 钉在当前窗口所在的视口上。**必须先于 SetTooltipV 调用**：SetNextWindowViewport
    // 置的是 ImGuiNextWindowDataFlags_HasViewport，tooltip 的 Begin() 会消费它并走 lock_viewport
    // 分支 —— 该分支同时把 ViewportAllowPlatformMonitorExtend 留在 -1，于是 imgui.cpp:7758 那段
    // 「装不下就新建视口」被跳过。缺了这一步，悬停就会造平台窗口（见头文件里的因果链）。
    if (ImGuiViewport* viewport = ImGui::GetWindowViewport())
        ImGui::SetNextWindowViewport(viewport->ID);

    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

void ImGui_ImplFISIR_PrepareViewportSwapChains() {
    // 帧首是安全期（上一帧 GPU 已完）：先把上一帧登记要释放的每视口渲染资源收掉。
    if (g_PendingDestroyRendererData && !g_PendingDestroyRendererData->empty()) {
        for (const PendingRendererDestroy& entry : *g_PendingDestroyRendererData) {
            // 关键：**先等这个视口最近用过的槽围栏落定**，再释放它的缓冲/描述符堆。
            // 只凭「帧首」是不够的 —— 独立视口（尤其是 hover 时反复建/销的临时 tooltip 视口）
            // 不在主视口 WaitFrameGPUIdle() 的覆盖范围内，资源提前释放会让堆缓冲地址被
            // 随后新建的另一种堆复用（VUID-11236/11228 预留区冲突）并最终设备丢失。
            WaitFencesGPUIdle(entry.fences, entry.fenceCount);
            DestroyRendererDataRaw(entry.rd);
        }
        g_PendingDestroyRendererData->clear();
    }
    if (!g_PendingViewports || g_PendingViewports->empty()) return;

    // 先把列表换出来再处理：补建过程中可能又触发登记（例如尺寸还没算好、本帧先跳过）
    std::vector<ImGuiViewport*> pending;
    pending.swap(*g_PendingViewports);

    for (ImGuiViewport* vp : pending) {
        FisirViewportData* vd = (FisirViewportData*)vp->PlatformUserData;
        if (!vd) continue;
        const uint32_t w = (uint32_t)vp->Size.x;
        const uint32_t h = (uint32_t)vp->Size.y;
        if (w == 0 || h == 0) { g_PendingViewports->push_back(vp); continue; }   // 尺寸未定，下一帧再来
        if (vd->SwapChain && vd->Width == w && vd->Height == h) continue;        // 已就绪（重复登记，或本帧又变回原尺寸）
        FISIR::RHISwapChain* swapchain = AcquireViewportSwapChain(vd, w, h);     // 首建或改尺寸都在这里
        if (!swapchain) continue;

        // 顺带把该视口的 ImGui 管线也在这里建好。独立视口的交换链有**自己的 render pass**，
        // 需要单独一条 ImGui 管线；而建管线会连带创建描述符堆/布局，放在帧中途的渲染回调里
        // 做既会和在飞的命令缓冲抢描述符预留区，也是之前那次死锁的同源问题 —— 统一挪到帧首。
        if (FISIR::RHIFrameBuffer* fb = swapchain->getSwapChainFrameBuffer(0)) {
            FISIR::RHIRenderPass* rp = fb->getFrameRenderPass();
            GetPipelineFor(rp);
            // 渲染资源（顶点/索引/参数缓冲 + 资源包）也在这里建：帧首是安全期，
            // 避免在帧中途的回调里创建缓冲/描述符堆。
            GetOrCreateRendererData(vp);
        }
    }
}

void ImGui_ImplFISIR_Shutdown() {
    if (!g_Rhi) return;

    // 每个视口的渲染资源（顶点/索引/参数缓冲 + 资源包）都是挂在 RendererUserData 上的，逐个释放。
    // 主视口也在 ImGui 的 Viewports[0] 里，所以这一圈就覆盖全了。
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    for (int i = 0; i < pio.Viewports.Size; ++i) DestroyRendererData(pio.Viewports[i]);

    // 还挂在待释放表里的（已关闭的独立视口）：同样要等 GPU 完成再释放，否则销毁 RHI 时会
    // 带着在飞的引用释放资源。本函数正是在销毁 RHI 之前调用的，时机合适。
    if (g_PendingDestroyRendererData && !g_PendingDestroyRendererData->empty()) {
        for (const PendingRendererDestroy& entry : *g_PendingDestroyRendererData) {
            WaitFencesGPUIdle(entry.fences, entry.fenceCount);
            DestroyRendererDataRaw(entry.rd);
        }
        g_PendingDestroyRendererData->clear();
    }

    // 管线/着色器/render pass 没有对应的 RHIDestroy* 接口（RHI 有意不在示例侧析构它们），
    // 这里只丢指针，资源随进程结束回收 —— 与其它示例（TextureCube / BunnyPBR）一致。
    g_Pipeline = nullptr;
    g_PipelineRenderPass = nullptr;
    g_VertexShader = nullptr;
    g_PixelShader = nullptr;

    if (g_FontUploadBuffer) { g_Rhi->RHIDestroyBuffer(g_FontUploadBuffer); g_FontUploadBuffer = nullptr; }
    if (g_FontSampler) { g_Rhi->RHIDestroySampler(g_FontSampler); g_FontSampler = nullptr; }
    if (g_FontTexture) { g_Rhi->RHIDestroyTexture(g_FontTexture); g_FontTexture = nullptr; }

    Trace("=== shutdown ===");
    if (g_TraceFile) { fclose(g_TraceFile); g_TraceFile = nullptr; }

    g_Rhi = nullptr;
}

void ImGui_ImplFISIR_RenderDrawData(FISIR::RHIRenderCommandList& cmdList,
                                    FISIR::RHIRenderPass* renderPass,
                                    const ImDrawData* drawData) {
    if (!g_Rhi || !renderPass || !drawData || drawData->CmdListsCount <= 0) return;
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) return;

    if (!g_Pipeline) {
        if (!CreatePipeline(renderPass)) return;
    }

    // 取本 render pass 对应的管线（主窗口一条，每个独立视口各一条）
    FISIR::RHIPipeline* pipeline = GetPipelineFor(renderPass);
    if (!pipeline) return;

    // 取**本视口**的渲染资源（顶点/索引/参数缓冲 + 资源包）。
    // OwnerViewport 是 ImGui 给的「这段 draw data 属于哪个视口」，官方后端也是靠它定位目标。
    ImGuiViewport* vp = drawData->OwnerViewport ? drawData->OwnerViewport : ImGui::GetMainViewport();
    FisirRendererData* rd = GetOrCreateRendererData(vp);
    if (!rd) return;

    if (!EnsureVertexIndexCapacity(rd, (uint32_t)drawData->TotalVtxCount, (uint32_t)drawData->TotalIdxCount)) return;

    UploadDrawData(rd, drawData);

    ImGuiParams params{};
    BuildProjectionMatrix(drawData, params.ProjMatrix);
    memcpy(rd->ParamsBuffer->getBufferData(), &params, sizeof(params));

    cmdList.SetPipelineState(pipeline);
    cmdList.SetResourcePack(rd->ResourcePack);
    cmdList.SetVertexBuffer(rd->VertexBuffer, 0, 0);
    cmdList.SetIndexBuffer(rd->IndexBuffer, 0);

    // 每个 DrawCmd 自带 ClipRect，用带偏移的 scissor 表达（只给 w/h 的 SetScissor 做不到）。
    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    const float framebufferWidth = drawData->DisplaySize.x * clipScale.x;
    const float framebufferHeight = drawData->DisplaySize.y * clipScale.y;

    uint32_t indexBase = 0;   // 当前 DrawList 在合并后索引缓冲里的起点
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        for (int c = 0; c < list->CmdBuffer.Size; ++c) {
            const ImDrawCmd* cmd = &list->CmdBuffer[c];

            if (cmd->UserCallback != nullptr) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    cmdList.SetPipelineState(pipeline);         // 外部改过后端状态，重绑一遍
                    cmdList.SetResourcePack(rd->ResourcePack);
                    cmdList.SetVertexBuffer(rd->VertexBuffer, 0, 0);
                    cmdList.SetIndexBuffer(rd->IndexBuffer, 0);
                }
                else {
                    cmd->UserCallback(list, cmd);
                }
                continue;
            }

            float clipMinX = (cmd->ClipRect.x - clipOffset.x) * clipScale.x;
            float clipMinY = (cmd->ClipRect.y - clipOffset.y) * clipScale.y;
            float clipMaxX = (cmd->ClipRect.z - clipOffset.x) * clipScale.x;
            float clipMaxY = (cmd->ClipRect.w - clipOffset.y) * clipScale.y;
            if (clipMinX < 0.0f) clipMinX = 0.0f;
            if (clipMinY < 0.0f) clipMinY = 0.0f;
            if (clipMaxX > framebufferWidth) clipMaxX = framebufferWidth;
            if (clipMaxY > framebufferHeight) clipMaxY = framebufferHeight;
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY) continue;

            cmdList.SetScissorRect((int32_t)clipMinX, (int32_t)clipMinY,
                                   (uint32_t)(clipMaxX - clipMinX), (uint32_t)(clipMaxY - clipMinY));
            // VtxOffset 已经加进索引，故这里只给 firstIndex（= 本 DrawList 的全局索引基址 + 本 cmd 偏移）。
            cmdList.DrawIndex(indexBase + cmd->IdxOffset, cmd->ElemCount, 0, 1);
        }
        indexBase += (uint32_t)list->IdxBuffer.Size;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Win32 平台输入
// ════════════════════════════════════════════════════════════════════════════
#if defined(_WIN32)

#include <windowsx.h>

namespace {

    bool g_PlatformInitialized = false;
    bool g_MouseTracked = false;
    ImGuiMouseCursor g_LastCursor = ImGuiMouseCursor_COUNT;

    ImGuiKey MapVirtualKeyToImGuiKey(WPARAM vk) {
        switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_1: return ImGuiKey_Semicolon;
        case VK_OEM_PLUS: return ImGuiKey_Equal;
        case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_5: return ImGuiKey_Backslash;
        case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_3: return ImGuiKey_GraveAccent;
        case VK_CAPITAL: return ImGuiKey_CapsLock;
        case VK_SCROLL: return ImGuiKey_ScrollLock;
        case VK_NUMLOCK: return ImGuiKey_NumLock;
        case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
        case VK_PAUSE: return ImGuiKey_Pause;
        case VK_NUMPAD0: return ImGuiKey_Keypad0;
        case VK_NUMPAD1: return ImGuiKey_Keypad1;
        case VK_NUMPAD2: return ImGuiKey_Keypad2;
        case VK_NUMPAD3: return ImGuiKey_Keypad3;
        case VK_NUMPAD4: return ImGuiKey_Keypad4;
        case VK_NUMPAD5: return ImGuiKey_Keypad5;
        case VK_NUMPAD6: return ImGuiKey_Keypad6;
        case VK_NUMPAD7: return ImGuiKey_Keypad7;
        case VK_NUMPAD8: return ImGuiKey_Keypad8;
        case VK_NUMPAD9: return ImGuiKey_Keypad9;
        case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE: return ImGuiKey_KeypadDivide;
        case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case VK_ADD: return ImGuiKey_KeypadAdd;
        case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
        case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
            return (ImGuiKey)(ImGuiKey_F1 + (int)(vk - VK_F1));
        default: break;
        }
        if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (int)(vk - '0'));
        if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (int)(vk - 'A'));
        return ImGuiKey_None;
    }

    void UpdateModifierKeys(ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Super,
            (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0);
    }

    // 剪贴板：InputText 的 Ctrl+C/V/X 靠它，否则只能在同一输入框内自循环
    const char* GetClipboardText(void*) {
        static std::string buffer;
        buffer.clear();
        if (!OpenClipboard(nullptr)) return nullptr;
        if (HANDLE handle = GetClipboardData(CF_TEXT)) {
            if (const char* text = static_cast<const char*>(GlobalLock(handle))) {
                buffer = text;
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return buffer.c_str();
    }

    void SetClipboardText(void*, const char* text) {
        if (!text || !OpenClipboard(nullptr)) return;
        if (EmptyClipboard()) {
            const size_t size = strlen(text) + 1;
            if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size)) {
                if (void* dst = GlobalLock(handle)) {
                    memcpy(dst, text, size);
                    GlobalUnlock(handle);
                    SetClipboardData(CF_TEXT, handle);
                }
            }
        }
        CloseClipboard();
    }

} // namespace

void ImGui_ImplFISIR_Win32_NewFrame(HWND hwnd) {
    if (!ImGui::GetCurrentContext() || !hwnd) return;
    // 多视口要用的显示器列表：显示器插拔/分辨率变化后得刷新，每帧重建一次最省心
    //（EnumDisplayMonitors + GetMonitorInfo 都是微秒级，且 vector 容量会复用）。
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        UpdateMonitors();
        g_MainHwnd = hwnd;   // 视口坐标以主窗口客户区原点为基准（见 MainClientOrigin 的注释）
        ImGuiViewport* mainVp = ImGui::GetMainViewport();
        if (mainVp && !mainVp->PlatformHandle) mainVp->PlatformHandle = (void*)hwnd;
    }
    ImGuiIO& io = ImGui::GetIO();

    if (!g_PlatformInitialized) {
        io.BackendPlatformName = "imgui_impl_fisir_win32";
        io.SetClipboardTextFn = SetClipboardText;
        io.GetClipboardTextFn = GetClipboardText;
        g_PlatformInitialized = true;
    }

    // ── DisplaySize / DisplayFramebufferScale（上游后端漏了这一步）──────────
// ImGui::NewFrame() 用 io.DisplaySize 设定主视口尺寸，所有窗口的裁剪矩形都由
// 它推导。不写的话 ImGui 内部保持初始哨兵值（负数），最终 draw data 的
// DisplaySize <= 0，RenderDrawData 第一行就 return —— 现象是「窗口正常开着、
// 里面全黑、UI 完全不见」。
// 本进程是 Per-Monitor DPI Aware（见 Application::createWindow），客户区像素
// 与交换链像素一一对应，所以 framebuffer scale 恒为 1。
    RECT client{};
    if (GetClientRect(hwnd, &client)) {
        io.DisplaySize = ImVec2((float)(client.right - client.left),
            (float)(client.bottom - client.top));
    }
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    if (io.WantSetMousePos) {
        POINT pos{ (LONG)io.MousePos.x, (LONG)io.MousePos.y };
        // 多视口时 io.MousePos **本来就是桌面坐标**，再 ClientToScreen 会多加一次客户区原点
        // （官方 Win32 后端同样只在关闭多视口时才转换）。而这里是"移动系统光标"，
        // 每帧多加一次原点就成了越跑越偏的漂移：光标一路窜到屏幕角上、点击全部落空。
        if (!(io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
            ClientToScreen(hwnd, &pos);
        SetCursorPos(pos.x, pos.y);
    }

    // 焦点判定要把**所有我们自己的窗口**都算进去。只比较主窗口 hwnd 的话，多视口下独立窗口
    // 获得前台时 GetForegroundWindow() != 主窗口，这一条就把鼠标位置丢掉了。
    HWND foreground = GetForegroundWindow();
    bool appFocused = (foreground == hwnd);
    if (!appFocused && foreground && (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
        appFocused = (ImGui::FindViewportByPlatformHandle((void*)foreground) != nullptr);

    // 光标压在我们哪个窗口上（桌面坐标）。两处都要用：报给 ImGui（AddMouseViewportEvent），
    // 以及判断"光标是否还在我们窗口上"（决定能不能不作废鼠标位置）。
    POINT mousePt{};
    const bool hasCursor = GetCursorPos(&mousePt) != 0;
    ImGuiID hoveredVpId = 0;
    if (hasCursor) {
        HWND under = WindowFromPoint(mousePt);
        if (under) {
            if (HWND root = GetAncestor(under, GA_ROOT)) under = root;   // 命中到子窗口时取顶层
            if (ImGuiViewport* v = ImGui::FindViewportByPlatformHandle((void*)under))
                hoveredVpId = v->ID;
        }
    }

    // 鼠标位置：前台时每帧喂；光标还在我们窗口上（哪怕我们不是前台）也喂 ——
    // "光标停在我们窗口里、我们不是前台"是完全合法的状态（刚点独立窗口那一帧、程序刚启动
    // 还没被激活）。原来写成 if (appFocused) {...} else AddMousePosEvent(-FLT_MAX)，
    // 只要不是前台就把坐标作废，而 ImGui 在**按下那一帧**看到 HoveredWindow 为空就会把这次
    // 点击判成"属于应用程序"（imgui.cpp 的 MouseDownOwned / clear_hovered_windows），
    // 后果是**整个按住期间**所有控件都不响应、拖拽和改尺寸一起僵住。
    // 另外按键消息自带坐标（见 HandleMessage），所以即便位置被抹掉，点击也不会再落空。
    if (hasCursor && (appFocused || hoveredVpId != 0)) {
        POINT pos = mousePt;
        // 多视口时喂**桌面坐标**（ImGui 的视口位置也是桌面坐标，见 Platform_GetWindowPos）；
        // 单视口时主视口恒在 (0,0)，喂客户区坐标。
        if (!(io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
            ScreenToClient(hwnd, &pos);
        io.AddMousePosEvent((float)pos.x, (float)pos.y);
    }
    else if (!appFocused) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (appFocused && !g_MouseTracked) {
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&track);
        g_MouseTracked = true;
    }

    // ── 鼠标压在哪个平台窗口上：必须由后端告诉 ImGui（官方 Win32 后端的做法）──────
    // 命中判定的第一行是 if (window->Viewport != g.MouseViewport) continue;（imgui.cpp:5991），
    // 也就是说 g.MouseViewport 一旦指错，那个视口里的**所有**窗口都收不到鼠标输入。
    // 不报这个信息时 ImGui 只能用"最后聚焦时间戳"猜（FindHoveredViewportFromPlatformWindowStack）：
    // 主视口是全屏的、矩形永远包含鼠标，而拖动中的窗口时间戳最新 —— 两边都会猜错。
    // 现象就是独立窗口里「点不动、拖不回、改不了尺寸」三件事一起发生。
    // WindowFromPoint 能返回"被拖窗口**后面**"的那个窗口，前提是 WM_NCHITTEST 对
    // NoInputs 的视口回了 HTTRANSPARENT（见 ViewportWndProc），两处是配套的。
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        io.AddMouseViewportEvent(hoveredVpId);   // 不在我们任何窗口上就报 0，别让 ImGui 用旧值

    // 每秒一次：把 "ImGui 眼里的鼠标位置 / 各视口矩形与标志 / 鼠标按键状态" 落到追踪文件。
    // 这是判断命中判定用的是不是同一套坐标空间的直接证据（和上面的 MSG 行对照即可）。
    {
        static DWORD sLastDump = 0;
        const DWORD now = GetTickCount();
        if (now - sLastDump >= 1000) {
            sLastDump = now;
            ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
            Trace("FRAME mousePos=(%.0f,%.0f) down0=%d down1=%d fg=0x%llx appFocused=%d viewports=%d hoveredVP=0x%08X",
                  io.MousePos.x, io.MousePos.y, io.MouseDown[0] ? 1 : 0, io.MouseDown[1] ? 1 : 0,
                  (unsigned long long)(uintptr_t)foreground, appFocused ? 1 : 0, pio.Viewports.Size,
                  io.MouseHoveredViewport);
            for (int i = 0; i < pio.Viewports.Size; ++i) {
                ImGuiViewport* v = pio.Viewports[i];
                Trace("   vp[%d] id=0x%08X pos=(%.0f,%.0f) size=(%.0f,%.0f) flags=0x%x handle=0x%llx cmdLists=%d",
                      i, v->ID, v->Pos.x, v->Pos.y, v->Size.x, v->Size.y, (unsigned)v->Flags,
                      (unsigned long long)(uintptr_t)v->PlatformHandle,
                      v->DrawData ? v->DrawData->CmdListsCount : -1);
            }
        }
    }

    const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();    if (io.MouseDrawCursor || cursor == ImGuiMouseCursor_None) {
        SetCursor(nullptr);
    }
    else if (cursor != g_LastCursor && cursor >= 0 && cursor < ImGuiMouseCursor_COUNT) {
        // 顺序必须与 ImGuiMouseCursor_ 一致；本工程未定义 UNICODE，故用 A 版 API + LPSTR。
        static LPCSTR cursorIds[ImGuiMouseCursor_COUNT] = {
            IDC_ARROW, IDC_IBEAM, IDC_SIZEALL, IDC_SIZENS, IDC_SIZEWE,
            IDC_SIZENESW, IDC_SIZENWSE, IDC_HAND, IDC_NO
        };
        SetCursor(LoadCursorA(nullptr, cursorIds[cursor]));
        g_LastCursor = cursor;
    }
}

    // ── 鼠标捕获（官方 Win32 后端 WM_LBUTTONDOWN/UP 分支的做法）──────────────
    // 按下时 SetCapture，全部按键都松开时 ReleaseCapture。
    // **没有捕获会坏一大片交互**：光标一旦移出窗口（拖窗口、拖滑块、拖右下角改尺寸都会
    // 移出），消息就断了 —— 连 WM_LBUTTONUP 都收不到，ImGui 的 MouseDown[0] 会一直停在
    // true，ActiveId 卡死：此后所有点击都不再触发，拖拽/缩放也会僵在半路。
    int g_MouseButtonsDown = 0;
    void OnMouseButtonDown(HWND hwnd, int button) {
        if (g_MouseButtonsDown == 0 && GetCapture() == nullptr)
            SetCapture(hwnd);              // 光标跑到窗口外也照样收鼠标消息
        g_MouseButtonsDown |= 1 << button;
    }
    void OnMouseButtonUp(HWND hwnd, int button) {
        g_MouseButtonsDown &= ~(1 << button);
        if (g_MouseButtonsDown == 0 && GetCapture() == hwnd)
            ReleaseCapture();
    }

bool ImGui_ImplFISIR_Win32_HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();

    // 追踪：所有鼠标消息都记下来（含"消息来自哪个窗口"和"我们最终喂给 ImGui 的坐标"）。
    // 这是判断"消息有没有到独立窗口 / 坐标空间对不对"的唯一直接证据。
    const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
    if (isMouseMsg) {
        const int cx = GET_X_LPARAM(lParam), cy = GET_Y_LPARAM(lParam);
        POINT dp{ cx, cy };
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ClientToScreen(hwnd, &dp);
        Trace("MSG hwnd=0x%llx msg=0x%04x client=(%d,%d) fed=(%d,%d) viewports=%d fg=0x%llx",
              (unsigned long long)(uintptr_t)hwnd, (unsigned)msg, cx, cy, (int)dp.x, (int)dp.y,
              (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ? 1 : 0,
              (unsigned long long)(uintptr_t)GetForegroundWindow());
    }

    // 按键/滚轮消息**自带坐标**：先喂位置再喂按键，保证 ImGui 处理这次按下时坐标一定有效。
    // 只靠 WM_MOUSEMOVE 是不够的 —— 光标停着不动就不会再来 MOVE，而位置有可能刚被
    // WM_MOUSELEAVE 之类抹成 -FLT_MAX；那一帧 ImGui 会把这个点击判成"属于应用程序"
    // （HoveredWindow 为空 ⇒ MouseDownOwned=false），于是**整个按住期间**任何控件都不响应，
    // 拖拽/改尺寸也一起僵住。带上坐标就彻底不依赖消息顺序了。
    auto feedPosFromMsg = [&](LPARAM lp) {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ClientToScreen(hwnd, &p);
        io.AddMousePosEvent((float)p.x, (float)p.y);
    };

    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: feedPosFromMsg(lParam); OnMouseButtonDown(hwnd, 0); io.AddMouseButtonEvent(0, true); return true;
    case WM_LBUTTONUP:   feedPosFromMsg(lParam); OnMouseButtonUp(hwnd, 0);   io.AddMouseButtonEvent(0, false); return true;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: feedPosFromMsg(lParam); OnMouseButtonDown(hwnd, 1); io.AddMouseButtonEvent(1, true); return true;
    case WM_RBUTTONUP:   feedPosFromMsg(lParam); OnMouseButtonUp(hwnd, 1);   io.AddMouseButtonEvent(1, false); return true;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: feedPosFromMsg(lParam); OnMouseButtonDown(hwnd, 2); io.AddMouseButtonEvent(2, true); return true;
    case WM_MBUTTONUP:   feedPosFromMsg(lParam); OnMouseButtonUp(hwnd, 2);   io.AddMouseButtonEvent(2, false); return true;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: {
        const int b = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
        feedPosFromMsg(lParam); OnMouseButtonDown(hwnd, b); io.AddMouseButtonEvent(b, true); return true;
    }
    case WM_XBUTTONUP: {
        const int b = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
        feedPosFromMsg(lParam); OnMouseButtonUp(hwnd, b); io.AddMouseButtonEvent(b, false); return true;
    }
    case WM_MOUSEWHEEL:
        feedPosFromMsg(lParam);
        io.AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
        return true;
    case WM_MOUSEHWHEEL:
        feedPosFromMsg(lParam);
        io.AddMouseWheelEvent(-(float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA, 0.0f);
        return true;
    case WM_MOUSEMOVE: {
        // ⚠ lParam 是「收到消息的那个窗口」的**客户区**坐标，而 ImGui 开启多视口后要的是
        // **桌面坐标**（imgui.cpp 里对应变量就叫 mouse_platform_pos，命中判定直接拿它和
        // 各视口的桌面矩形比）。单窗口时两者只差一个客户区原点；独立视口下则是完全不同的
        // 空间 —— 直接用 lParam 会把这个位置写坏，现象是「独立窗口里怎么点都没反应、
        // 也拖不动/改不了尺寸」（消息泵跑在 NewFrame 之前，所以它总是最后一次写入）。
        POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ClientToScreen(hwnd, &p);
        io.AddMousePosEvent((float)p.x, (float)p.y);
        return false;
    }
    case WM_MOUSELEAVE:
        g_MouseTracked = false;
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return false;

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        UpdateModifierKeys(io);
        if (const ImGuiKey key = MapVirtualKeyToImGuiKey(wParam); key != ImGuiKey_None)
            io.AddKeyEvent(key, true);
        return true;
    case WM_KEYUP: case WM_SYSKEYUP:
        UpdateModifierKeys(io);
        if (const ImGuiKey key = MapVirtualKeyToImGuiKey(wParam); key != ImGuiKey_None)
            io.AddKeyEvent(key, false);
        return true;
    case WM_CHAR:
        if (wParam > 0 && wParam < 0x10000) io.AddInputCharacterUTF16((ImWchar16)wParam);
        return true;
    case WM_SETFOCUS:
        io.AddFocusEvent(true);
        return false;
    case WM_KILLFOCUS:
        io.AddFocusEvent(false);
        return false;
    default:
        break;
    }
    return false;
}

#endif // _WIN32
