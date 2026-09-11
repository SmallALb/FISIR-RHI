#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
    auto viewport = rhi->RHICreateViewport(512, 384,
        FISIR::TextureCOLORType::RGBA_8, (void*)&win32Data);
    rhi->Init();

    InitClusterSelection(rhi);
    SetClusterSelectionBuffer(rhi);

	getInputData().LodScale = 70.0f;

    // 4. 交换链 + 清屏值
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    // 纹理呈现模式：硬光栅把结果渲染进离屏颜色纹理，PS 采样该纹理呈现。
    swapchain->enableTextureInput(GetOffscreenColorTexture(), GetOffscreenSampler());

    auto PresentPipeline = swapchain->getSwapChainRenderPipeline();
    FISIR::ClearValue clearPresent{.ColorClear = 1,
        .colorinfo = {0.1f, 0.1f, 0.2f, 1.f}, .DepthStencilClear = 0};

    // 5. 相机状态
    // 场景 AABB 中心约 (0,-107,3)，半径约 248；相机置于 +Z 前方，yaw=π 朝 -Z 望向场景。
    glm::vec3 camPos(0.0f, -60.0f, 400.0f);
    float camYaw = glm::pi<float>(), camPitch = 0.0f;
    const float moveStep = 0.3f;
    // 鼠标转向：灵敏度（弧度/像素）与俯仰角钳制（避免 gimbal lock 翻转）
    const float mouseSens = 0.003f;
    const float maxPitch = glm::radians(89.0f);
    POINT lastMouse{};
    GetCursorPos(&lastMouse);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f),
        (float)NANITE_RT_WIDTH / (float)NANITE_RT_HEIGHT, 1.0f, 2000.0f);

    // 6. 主循环：更新相机 → 选择+渲染 → 呈现
    MSG msg = {0};
    uint64_t frameCount = 0;
    auto fpsStart = std::chrono::steady_clock::now();
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        WaitFrameGPUIdle();

        const uint32_t lastFrameClusterCount = GetEnabledClusterCount();

        // 获取交换链图像。刻意放在簇选择/渲染之前：若失败则整帧跳过，不会留下
        // 「compute 已提交、present 未提交」的半帧——那会让帧完成信号量多出一次
        // 无人消费的信号，破坏 wait/signal 配对（binary 信号量不允许重复挂起信号）。
        uint32_t frameID = swapchain->acquireGetImageInfoID();
        if (frameID == FISIR::RHISwapChain::FAILEID) continue;

        // ── 相机转向：按住右键拖动鼠标（鼠标右移右转，上移抬头）──
        POINT curMouse;
        GetCursorPos(&curMouse);
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
            float dx = (float)(curMouse.x - lastMouse.x);
            float dy = (float)(curMouse.y - lastMouse.y);
            camYaw   -= dx * mouseSens;   // 此相机约定 yaw 减小为右转（见 lookAt 右向量）
            camPitch += dy * mouseSens;   // 屏幕 y 向下，故上移 dy<0 → pitch 增大 → 抬头
            if (camPitch >  maxPitch) camPitch =  maxPitch;
            if (camPitch < -maxPitch) camPitch = -maxPitch;
        }
        lastMouse = curMouse;

        // ── 相机移动：WASD 沿 front 的水平投影前后左右，QE 升降 ──
        glm::vec3 camDir(
            cosf(camPitch) * sinf(camYaw),
            sinf(camPitch),
            cosf(camPitch) * cosf(camYaw));
        // 水平前向（忽略 pitch，保证前后左右始终平面移动，避免抬头时 W 上天）
        glm::vec3 front = glm::normalize(glm::vec3(camDir.x, 0.0f, camDir.z));
        glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));

        if (GetAsyncKeyState('W') & 0x8000) camPos += front * moveStep;
        if (GetAsyncKeyState('S') & 0x8000) camPos -= front * moveStep;
        if (GetAsyncKeyState('A') & 0x8000) camPos -= right * moveStep;
        if (GetAsyncKeyState('D') & 0x8000) camPos += right * moveStep;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) camPos.y += moveStep;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) camPos.y -= moveStep;

        glm::vec3 camTarget = camPos + camDir;
        glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0, 1, 0));
        // NaniteRender 使用行向量约定 mul(v, M)，故上传转置后的 VP
        getRenderParams().VPMatrix = glm::transpose(proj * view);
        // 选择 shader 的视锥剔除相机同步为移动相机，避免移动后远处簇被错误剔除
        getInputData().Position = glm::vec4(camPos, 0.0f);
        getInputData().Direction = glm::vec4(camDir, 0.0f);

        ExecuteClusterSelectionPass(rhi);

        FISIR::RHIRenderCommandList cmdList(rhi);
        auto info = swapchain->getSwapChainGetImageInfo(frameID);
        auto frameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);
        if (frameBuf) {
            cmdList.BeginRenderPass(frameBuf, 0, clearPresent);
            cmdList.SetPipelineState(PresentPipeline);
            cmdList.SetResourcePack(swapchain->getSwapchainResourcePack());
            cmdList.SetViewPort(0, 0, viewport->getViewportWidth(), viewport->getViewportHeight(), 1.0f, 0.0f);
            cmdList.SetScissor(viewport->getViewportWidth(), viewport->getViewportHeight());
            cmdList.DrawPrimitive(0, 3, 1);
            cmdList.EndRenderPass();
        }

        // 帧内最后一个提交：等待 acquire 与渲染完成信号量，信号 present 与帧完成信号量。
        // 不再等围栏——帧完成信号量在下一帧开头由 WaitFrameGPUIdle() 消费。
        EndFramePresentPass(cmdList, info);
        // 唯一保留的围栏调用，且它不是「等 GPU 完成」，只是「等 RHI 线程把这一页提交出去」：
        // vkQueuePresentKHR 等待的 renderFinish 必须已有对应的 signal 提交，否则校验层报
        // "has no way to be signaled"。这一步只花一次线程间握手，帧依旧在 GPU 上流水。
        info.finishFence->waitFenceSubmited();
        swapchain->present(frameID);
        // 每 100 帧更新标题：FPS + 相机参数
        if (++frameCount % 100 == 0) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - fpsStart).count();
            fpsStart = now;
            char title[192];
            snprintf(title, sizeof(title),
                "Nanite | %.1f FPS | clusters %u | pos(%.2f, %.2f, %.2f) yaw %.1f pitch %.1f",
                100.0f / elapsed, lastFrameClusterCount,
                camPos.x, camPos.y, camPos.z,
                glm::degrees(camYaw), glm::degrees(camPitch));
            SetWindowTextA(hwnd, title);
        }
    }

cleanup:
    Info("Shutting down...");
    DestroyClusterResource(rhi);
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    Info("=== Nanite -- Done ===");
    return 0;
}
