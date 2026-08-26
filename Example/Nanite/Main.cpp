#include <windows.h>

#include <atomic>
#include <thread>

#include "Log/Logger.h"
#include "RHICommandList.h"
#include "RHICreator.h"
#include "RHIFence.h"
#include "RHIFrameBuffer.h"
#include "RHISwapChain.h"
#include "RHITypes.h"
#include "BVH.h"
#include "ClusterSelection.h"
// 窗口回调
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

int main() {
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
#endif

    Info("============================================");
    Info("|  Nanite -- Minimal Single-Window Mode    |");
    Info("============================================");

    // 1. 创建 RHI
    FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);
    FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();

    // 2. 创建窗口
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const char CLASS_NAME[] = "NaniteWindow";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, CLASS_NAME, "Nanite",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1024, 768, NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);

    // 3. 创建视口并初始化 RHI
    struct Win32Data { HINSTANCE hinstance; HWND hwnd; } win32Data{hInstance, hwnd};
    auto viewport = rhi->RHICreateViewport(1024, 768,
        FISIR::TextureCOLORType::RGBA_8, (void*)&win32Data);
    rhi->Init();

    InitClusterSelection(rhi);
    SetClusterSelectionBuffer(rhi);

    // 4. 交换链 + 清屏值
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    FISIR::ClearValue clearPresent{.ColorClear = 1,
        .colorinfo = {0.1f, 0.1f, 0.2f, 1.f}, .DepthStencilClear = 0};

    // 5. 主循环：清屏 + 呈现
    MSG msg = {0};
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        FISIR::RHIComputeCommandList list(rhi);
        ExecuteClusterSelectionPass(list);

        uint32_t frameID = swapchain->acquireGetImageInfoID();
        if (frameID == FISIR::RHISwapChain::FAILEID) continue;

        FISIR::RHIRenderCommandList cmdList(rhi);
        auto info = swapchain->getSwapChainGetImageInfo(frameID);
        auto frameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);
        if (frameBuf) {
            cmdList.BeginRenderPass(frameBuf, 0, clearPresent);
            // TODO: 在这里录制 Nanite 渲染命令
            cmdList.EndRenderPass();
        }

        cmdList.End(info.finishFence, {info.avaliable}, {info.renderFinish});
        info.finishFence->waitFenceSubmited();
        swapchain->present(frameID);
    }

cleanup:
    Info("Shutting down...");
    DestroyClusterResource(rhi);
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    Info("=== Nanite -- Done ===");
    return 0;
}
