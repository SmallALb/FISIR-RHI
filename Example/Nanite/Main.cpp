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
#include "NaniteUI.h"

// 窗口回调：先把消息喂给 ImGui 后端（鼠标/键盘/滚轮/字符），它认领的消息就不再往下传。
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (NaniteUI::HandleMessage(hwnd, uMsg, wParam, lParam)) return 0;

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
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, CLASS_NAME, "Nanite",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        NANITE_RT_WIDTH, NANITE_RT_HEIGHT, NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);

    // 3. 创建视口并初始化 RHI
    //    呈现设备句柄布局由 RHI 定义（见 RHIDisplay.h）：Win32Window 用 Win32DisplayHandle{ hinstance, hwnd }，
    //    与原来的 Win32Data 布局完全一致。
    FISIR::Win32DisplayHandle win32Data{ hInstance, hwnd };
    rhi->Init();
    auto viewport = rhi->RHICreateViewport(NANITE_RT_WIDTH, NANITE_RT_HEIGHT,
        FISIR::TextureCOLORType::RGBA_8, FISIR::DisplayDeviceType::Win32Window, (void*)&win32Data, 2000);

	// 注释掉 = 用内置的 Res/mitsuba.bvh + Res/mitsuba.nanitemesh（当前状态）；换模型请取消注释。
	SetModelPath("Res/Bunny_hi.obj", 130.0f);
    InitClusterSelection(rhi);
    SetClusterSelectionBuffer(rhi);

    getInputData().LodScale = 1000.f;

    getInputData().SwRasterThreshold = 0.02f;
	getRenderParams().FarPlane = 20000.f;   // 投影矩阵远裁剪面，决定了簇的投影尺度

    // 4. 交换链 + 清屏值
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    // 缓冲呈现模式（BufferEnable=1）：软光栅 compute 与硬光栅 PS 都写 FrameBuffer，
    // 呈现 PS 直接读它显示；离屏颜色/深度纹理只作占位，不再被采样。
    swapchain->enableBufferInput(NANITE_RT_WIDTH, NANITE_RT_HEIGHT, GetFrameBuffer());

    auto PresentPipeline = swapchain->getSwapChainRenderPipeline();
    FISIR::ClearValue clearPresent{ .ColorClear = 1,
        .colorinfo = {0.1f, 0.1f, 0.2f, 1.f}, .DepthStencilClear = 0 };

    // 5. ImGui：面板用于在线调 LodScale / 软光栅阈值 / 远裁剪面 / 清屏色 / 相机参数。
    if (!NaniteUI::Init(rhi, hwnd)) Error("[Nanite] ImGui init FAILED -- UI will be missing");

    // 6. 相机状态
    // 场景 AABB 中心约 (0,-107,3)，半径约 248；相机置于 +Z 前方，yaw=π 朝 -Z 望向场景。
    glm::vec3 camPos(0.0f, -60.0f, 400.0f);
    float camYaw = glm::pi<float>(), camPitch = 0.0f;
    NaniteUI::CameraSettings cameraSettings;
    // 鼠标转向：俯仰角钳制（避免 gimbal lock 翻转）
    const float maxPitch = glm::radians(89.0f);
    POINT lastMouse{};
    GetCursorPos(&lastMouse);

    // 7. 主循环：更新相机 → 选择+渲染 → 呈现
    MSG msg = { 0 };
    uint64_t frameCount = 0;
    auto fpsStart = std::chrono::steady_clock::now();
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        uint32_t lastFrameSwCount = 0, lastFrameHwCount = 0, lastFrameSwTris = 0;

        uint32_t frameID = swapchain->acquireGetImageInfoID();
        if (frameID == FISIR::RHISwapChain::FAILEID) continue;

        WaitFrameGPUIdle();

        // 统计量必须在 WaitFrameGPUIdle() **之后**读：上一帧 GPU 已经跑完，主机可见缓冲里
        // 才是那一帧的最终计数。放在它之前读会和本帧 CPU 侧的重置赛跑（实测标题偶尔读到 hw 0）。
        GetSelectedClusterCounts(lastFrameSwCount, lastFrameHwCount, lastFrameSwTris);

        // 面板可能改过相机参数，这里先取一份最新值给本帧的输入处理用。
        const float moveStep = cameraSettings.MoveStep;
        const float mouseSens = cameraSettings.MouseSensitivity;

        // ── 相机转向：按住右键拖动鼠标（鼠标右移右转，上移抬头）──
        // 鼠标压在面板上时不转视角（否则拖滑条会同时把镜头甩走）。
        POINT curMouse;
        GetCursorPos(&curMouse);
        if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) && !NaniteUI::WantCaptureMouse()) {
            float dx = (float)(curMouse.x - lastMouse.x);
            float dy = (float)(curMouse.y - lastMouse.y);
            camYaw -= dx * mouseSens;   // 此相机约定 yaw 减小为右转（见 lookAt 右向量）
            camPitch += dy * mouseSens;   // 屏幕 y 向下，故上移 dy<0 → pitch 增大 → 抬头
            if (camPitch > maxPitch) camPitch = maxPitch;
            if (camPitch < -maxPitch) camPitch = -maxPitch;
        }
        lastMouse = curMouse;

        // ── 相机移动：WASD 沿 front 的水平投影前后左右，QE 升降 ──
        glm::vec3 camDir(
            cosf(camPitch) * sinf(camYaw),
            sinf(camPitch),
            cosf(camPitch) * cosf(camYaw));
        // 水平前向（忽略 pitch，保证前后左右始终平面移动，避免抬头时 W 上天）
        //glm::vec3 front = glm::normalize(glm::vec3(camDir.x, 0.0f, camDir.z));
        glm::vec3 right = glm::normalize(glm::cross(camDir, glm::vec3(0.0f, 1.0f, 0.0f)));

        // 正在输入文本 / 面板想独占键盘时不让 WASD 移动相机。
        if (!NaniteUI::WantCaptureKeyboard()) {
            if (GetAsyncKeyState('W') & 0x8000) camPos += camDir * moveStep;
            if (GetAsyncKeyState('S') & 0x8000) camPos -= camDir * moveStep;
            if (GetAsyncKeyState('A') & 0x8000) camPos -= right * moveStep;
            if (GetAsyncKeyState('D') & 0x8000) camPos += right * moveStep;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) camPos.y += moveStep;
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) camPos.y -= moveStep;
        }

        glm::vec3 camTarget = camPos + camDir;
        glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0, 1, 0));

        // ── ImGui 帧：面板里的改动在这里落进 InputData / RenderParams（含 FOV）──
        NaniteUI::FrameStats stats;
        stats.SoftwareClusters = lastFrameSwCount;
        stats.SoftwareTriangles = lastFrameSwTris;
        stats.HardwareClusters = lastFrameHwCount;
        stats.CameraX = camPos.x; stats.CameraY = camPos.y; stats.CameraZ = camPos.z;
        stats.YawDegrees = glm::degrees(camYaw);
        stats.PitchDegrees = glm::degrees(camPitch);
        NaniteUI::NewFrame(cameraSettings, stats,
            viewport->getViewportWidth(), viewport->getViewportHeight());

        // 用（可能刚被面板改过的）FOV 重建投影矩阵，所以 UI 调整没有一帧延迟。
        glm::mat4 proj = glm::perspective(glm::radians(cameraSettings.FovDegrees),
            (float)viewport->getViewportWidth() / (float)viewport->getViewportHeight(), 1.0f, 20000.0f);
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
            // ImGui 必须画在同一个 render pass 内（交换链的 pass 是 loadOp=Clear，
            // 另起一个 pass 会把刚呈现的画面再清一遍），且排在场景之后靠提交顺序覆盖。
            NaniteUI::Render(cmdList, frameBuf->getFrameRenderPass());
            cmdList.EndRenderPass();
        }

        // 呈现录成指令（必须在 End 之前）：由 RHI 线程在本页 vkQueueSubmit 之后执行
        // vkQueuePresentKHR。这里不再、也不能自己 present —— 交换链是外部同步对象，
        // acquire / present / 重建全归 RHI 线程。
        cmdList.Present(swapchain, frameID);

        EndFramePresentPass(cmdList, info);

        // 纯节流：等 RHI 线程把本帧提交出去，避免 CPU 无界地跑到 GPU 前面。
        info.finishFence->waitFenceSubmited();

        // 多视口：面板被拖出主窗口后，那些独立 OS 窗口各自渲染自己的交换链。
        // 放在主窗口呈现之后 —— 它们画的是本帧已经 Render() 好的 draw data。
        NaniteUI::UpdatePlatformWindows();
        // 每 100 帧更新标题：FPS + 软/硬光栅的簇数/三角形数 + 相机参数
        if (++frameCount % 100 == 0) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - fpsStart).count();
            fpsStart = now;
            char title[192];
            snprintf(title, sizeof(title),
                "Nanite | %.1f FPS | sw %u clusters / %u tris | hw %u clusters | pos(%.2f, %.2f, %.2f) yaw %.1f pitch %.1f",
                100.0f / elapsed, lastFrameSwCount, lastFrameSwTris, lastFrameHwCount,
                camPos.x, camPos.y, camPos.z,
                glm::degrees(camYaw), glm::degrees(camPitch));
            SetWindowTextA(hwnd, title);
        }
    }

cleanup:
    Info("Shutting down...");
    NaniteUI::Shutdown();
    DestroyClusterResource(rhi);
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    Info("=== Nanite -- Done ===");
    return 0;
}
