    #include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>
#include <deque>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
#include <thread>
#include "RHITypes.h"
#include "RHICreator.h"
#include "ShaderComplier.h"
#include "RHIShader.h"
#include "RHIRenderPass.h"
#include "RHIPipeline.h"
#include "RHIFrameBuffer.h"
#include "RHICommandList.h"
#include "RHISwapChain.h"
#include "RHIFence.h"
#include "RHISampler.h"
#include "Log/Logger.h"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "PerformanceTest.h"
#include "Platform.h"   // 平台层：窗口/事件/资源读取（Win32 与 Android 二选一实现）

#ifdef __ANDROID__
// 安卓端叠一层 ImGui 显示实时帧率（桌面端不引入，保持既有行为）。
// 后端是仓库根的 ImGui_Impl_FISIR.cpp —— 它把 ImGui 的绘制数据录进我们自己的 RHI 命令列表，
// 所以这里不需要任何原生窗口/输入后端（安卓只需 io.DisplaySize 与 io.DeltaTime）。
#include "imgui.h"
#include "ImGui_Impl_FISIR.h"
#include <chrono>
#endif

// 将 RGBA8 像素缓冲写为 24-bit BMP（BGR、自底向上）。截图读回用。
static void WriteBMP(const char* path, uint32_t width, uint32_t height, const unsigned char* rgba) {
    uint32_t rowSize = (width * 3 + 3) & ~3u;      // 每行按 4 字节对齐
    uint32_t dataSize = rowSize * height;
    uint32_t fileSize = 54 + dataSize;
    unsigned char header[54] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    *(uint32_t*)(header + 2)  = fileSize;          // 文件总大小
    *(uint32_t*)(header + 10) = 54;                // 像素数据偏移
    *(uint32_t*)(header + 14) = 40;                // BITMAPINFOHEADER 大小
    *(int32_t*)(header + 18)  = (int32_t)width;
    *(int32_t*)(header + 22)  = (int32_t)height;
    *(uint16_t*)(header + 26) = 1;                 // 颜色平面数
    *(uint16_t*)(header + 28) = 24;                // 每像素位数
    *(uint32_t*)(header + 34) = dataSize;          // 像素数据大小

    FILE* f = fopen(path, "wb");
    if (!f) { Error("Failed to open screenshot file '{}'", path); return; }
    fwrite(header, 1, 54, f);

    std::vector<unsigned char> row(rowSize, 0);
    // 读回缓冲的行序已与 BMP 的 bottom-up 存储一致（首行即图像顶部），按序写入即可，勿再翻转。
    for (uint32_t y = 0; y < height; ++y) {
        const unsigned char* src = rgba + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];   // B
            row[x * 3 + 1] = src[x * 4 + 1];   // G
            row[x * 3 + 2] = src[x * 4 + 0];   // R
        }
        fwrite(row.data(), 1, rowSize, f);
    }
    fclose(f);
}

// 每帧写入 b0 的常量缓冲：ViewProj + Model + 网格参数（N 个立方体按行列铺开、各自自旋）
struct FrameUniforms {
    glm::mat4 viewProj;    // proj * view（自旋模式下固定）
    glm::mat4 model;       // 绕 Y 轴旋转（每个立方体绕自身中心自旋）
    glm::vec4 gridParams;  // x=列数, y=行数, z=列间距, w=行间距
};

// 平台入口：
//   · 桌面（Win32）：main(argc, argv)
//   · Android      ：PlatformAndroid.cpp 的 android_main(app) → RunTextureCube()
// 渲染主体两边共用，差异全在 Platform::* 里。
#ifdef __ANDROID__
int RunTextureCube() {
#else
int main(int argc, char* argv[]) {
#endif
#if defined(_DEBUG) && !defined(__ANDROID__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
#endif
    //TextureCube.exe -Test -Frames 5000 -Warmup 100 -DC 1 -DC 10 -DC 100 -DC 1000 -DC 2000 -DC 3000 > sweep_bench2.log 2 > &1; echo "EXIT=$?"

    // ---------- 0. 命令行参数解析 ----------
    // -Test      : 启用性能测试，跑完指定帧数后导出报告并退出
    // -Frames N  : 测试帧数（默认 5000，样本量越大统计越稳定）
    // -Warmup N  : 预热帧数，前 N 帧（着色器/管线首帧编译等）不计入统计（默认 60）
    // -DC N      : 每帧绘制调用数；可多次指定，每个 -DC 生成一组测试（默认 1）
    // -INS N     : 每次绘制的实例数（多实例绘制）；可多次指定，每个 -INS 生成一组测试（默认 1）
    bool runPerfTest = false;
    uint64_t testFrameCount = 5000;
    uint64_t warmupFrameCount = 60;
    std::vector<uint32_t> drawCallCounts;
    std::vector<uint32_t> instanceCounts;
    // 截图参数：-GetFrames Begin End，截取 [Begin, End] 帧（闭区间）
    bool getFrames = false;
    uint64_t getFrameBegin = 0;
    uint64_t getFrameEnd = 0;
    // 呈现设备：--display=win32|hidden|headless（见 RHIDisplay.h）；-ExitAfter N：跑 N 帧后干净退出
    std::string displayName = "win32";
    uint64_t exitAfterFrames = 0;
#ifdef __ANDROID__
    // Android 没有命令行：全部取默认值，呈现设备固定 AndroidWindow
    displayName = "android";
#else
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-Test" || arg == "-test" || arg == "/Test") {
            runPerfTest = true;
        } else if ((arg == "-Frames" || arg == "-frames") && i + 1 < argc) {
            testFrameCount = std::stoull(argv[++i]);
        } else if ((arg == "-Warmup" || arg == "-warmup") && i + 1 < argc) {
            warmupFrameCount = std::stoull(argv[++i]);
        } else if ((arg == "-DC" || arg == "-dc") && i + 1 < argc) {
            drawCallCounts.push_back(static_cast<uint32_t>(std::stoul(argv[++i])));
        } else if ((arg == "-INS" || arg == "-ins") && i + 1 < argc) {
            instanceCounts.push_back(static_cast<uint32_t>(std::stoul(argv[++i])));
        } else if ((arg == "-GetFrames" || arg == "-getframes") && i + 2 < argc) {
            getFrames = true;
            getFrameBegin = std::stoull(argv[++i]);
            getFrameEnd = std::stoull(argv[++i]);
        } else if ((arg == "-ExitAfter" || arg == "-exitafter") && i + 1 < argc) {
            // 跑够 N 帧就干净退出。无窗口模式（hidden/headless）没法靠点叉关掉，必须给个出口；
            // 也给自动化脚本用。
            exitAfterFrames = std::stoull(argv[++i]);
        } else if (arg == "-Hidden" || arg == "-hidden") {
            displayName = "hidden";
        } else if (arg.rfind("--display=", 0) == 0 || arg.rfind("-display=", 0) == 0) {
            displayName = arg.substr(arg.find('=') + 1);
        } else if ((arg == "--display" || arg == "-display") && i + 1 < argc) {
            displayName = argv[++i];
        }
    }
#endif // !__ANDROID__
    if (drawCallCounts.empty()) drawCallCounts.push_back(1);
    if (instanceCounts.empty()) instanceCounts.push_back(1);

    // ── 呈现设备选择（抽象见 RHIDisplay.h）──────────────────────────────
    //   win32        ：普通 Win32 窗口（默认）
    //   hidden       ：同样的 Win32Window 设备，但窗口不 Show（验证"没有可见窗口也能跑完整交换链+呈现"）
    //   headless     ：DisplayDeviceType::Headless —— 不建窗口、不建 surface、不建交换链，
    //                  只渲染到离屏 colorTexture，靠 -GetFrames 的读回证明"确实画出来了"
    //   displayplane ：直连显示器（DisplayPlaneHandle）。Windows 上拿不到（没有 display、也缺
    //                  VK_KHR_display_swapchain），这里用它演示"后端未实现的设备类型会被明确拒绝，
    //                  并自动退化成离屏渲染"，而不是崩在空 surface 上。
    FISIR::DisplayDeviceType displayType = FISIR::DisplayDeviceType::Win32Window;
    bool hiddenWindow = false;
#ifdef __ANDROID__
    // Android：呈现设备固定是 AndroidWindow（ANativeWindow 由 NativeActivity 交给平台层），
    // 上面那些 --display 选项只在桌面有意义。
    displayType = FISIR::DisplayDeviceType::AndroidWindow;
#else
    if (displayName == "headless") {
        displayType = FISIR::DisplayDeviceType::Headless;
    } else if (displayName == "hidden") {
        hiddenWindow = true;
    } else if (displayName == "displayplane" || displayName == "display") {
        displayType = FISIR::DisplayDeviceType::DisplayPlane;
    } else if (!displayName.empty() && displayName != "win32" && displayName != "win32window") {
        Error("未知的 --display={}（可用：win32 | hidden | headless | displayplane）", displayName);
        return 1;
    }
#endif
    Info("==== display device: {} ====", FISIR::DisplayDeviceTypeName(displayType));

    if (runPerfTest) {
        Info("=======");
        Info("|Test Mode|");
        Info("========");

    }

    Debug("Main Thread 0x{:x}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // 1. 创建 RHI
    FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);
    FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();

    // 2. 平台窗口 / 呈现句柄：Win32 建窗口；Android 取 NativeActivity 的 ANativeWindow。
    //    实现见 PlatformWin32.cpp / PlatformAndroid.cpp，句柄布局都是 RHIDisplay.h 里那套 void* 结构。
    Platform::Window window{};
    window.width = 800;
    window.height = 600;
    void* deviceHandle = nullptr;
    if (displayType == FISIR::DisplayDeviceType::Win32Window ||
        displayType == FISIR::DisplayDeviceType::AndroidWindow) {
        if (!Platform::Init(window, hiddenWindow, "Textured Cube (Single Pass)")) {
            Error("Platform::Init 失败：拿不到可用的呈现目标");
            return 1;
        }
        displayType = window.type;
        deviceHandle = window.deviceHandle;
    } else if (displayType == FISIR::DisplayDeviceType::DisplayPlane) {
        static FISIR::DisplayPlaneHandle planeData{ 0, 0, 0 };   // 直连显示器：display / plane / mode 下标
        deviceHandle = &planeData;
        Info("DisplayPlane: 句柄 = {{ displayIndex={}, planeIndex={}, modeIndex={} }}"
             "（本机拿不到 display，预期被后端拒绝）", planeData.displayIndex, planeData.planeIndex, planeData.modeIndex);
    } else {
        Info("Headless: 不创建任何窗口");
    }

    auto viewport = rhi->RHICreateViewport(window.width ? window.width : 800, window.height ? window.height : 600,
                                           FISIR::TextureCOLORType::RGBA_8, displayType, deviceHandle, 3);
    if (!rhi->Init()) {
        // 设备初始化失败必须在这里停住：继续往下走会拿空的逻辑设备去调 vkCreateShaderModule，
        // 在手机上（无验证层）表现为直接 SIGSEGV，看不出真正原因。
        Error("RHI Init 失败（设备初始化不通过），退出");
        return 1;
    }

    // ---------- 3. 顶点着色器 ----------
    const wchar_t* vsCode = LR"(
        [[vk::binding(0, 0)]] cbuffer MVPBuffer  : register(b0) {
            float4x4 ViewProj;
            float4x4 Model;
            float4 GridParams; // x=cols, y=rows, z=spacingX, w=spacingY
        };
        struct VSInput {
            float3 pos : POSITION;
            float3 color : COLOR;
            float2 uv : TEXCOORD;
        };
        struct PSInput {
            float4 pos : SV_POSITION;
            float3 color : COLOR;
            float2 uv : TEXCOORD;
        };
        PSInput mainVs(VSInput input, uint instanceID : SV_InstanceID) {
            PSInput output;
            // 先绕自身中心旋转（Model），再平移到网格位置 → 每个立方体自旋
            float4 worldPos = mul(Model, float4(input.pos, 1.0));
            float col = fmod(float(instanceID), GridParams.x);
            float row = floor(float(instanceID) / GridParams.x);
            worldPos.xyz += float3(
                (col - (GridParams.x - 1.0) * 0.5) * GridParams.z,
                ((GridParams.y - 1.0) * 0.5 - row) * GridParams.w,
                0.0);
            output.pos = mul(ViewProj, worldPos);
            output.color = input.color;
            output.uv = input.uv;
            return output;
        }
    )";
    FISIR::ShaderComplier* vCompiler = new FISIR::ShaderComplier();
    vCompiler->compileShader(vsCode, wcslen(vsCode) * sizeof(wchar_t), L"mainVs", L"vs_5_0");
    auto vs = rhi->RHICreateShader(FISIR::ShaderTYP::__VERTEXSHADER__, "mainVs", vCompiler->getShaderData(), vCompiler->getShaderDataSize());
    delete vCompiler;

    // ---------- 4. 像素着色器 ----------
    const wchar_t* psCode = LR"(
        [[vk::binding(1, 0)]] Texture2D myTexture  : register(t1);
        [[vk::binding(2, 0)]] SamplerState mySampler  : register(s2);
        struct PSInput {
            float4 pos : SV_POSITION;
            float3 color : COLOR;
            float2 uv : TEXCOORD;
        };
        float4 mainPs(PSInput input) : SV_TARGET {
            float4 texColor = myTexture.Sample(mySampler, input.uv);
            return texColor;
        }
    )";
    FISIR::ShaderComplier* pCompiler = new FISIR::ShaderComplier();
    pCompiler->compileShader(psCode, wcslen(psCode) * sizeof(wchar_t), L"mainPs", L"ps_5_0");
    auto ps = rhi->RHICreateShader(FISIR::ShaderTYP::__FRAGMENTSHADER__, "mainPs", pCompiler->getShaderData(), pCompiler->getShaderDataSize());
    delete pCompiler;

    // ---------- 5. 离屏渲染目标 ----------
    FISIR::TextureInfo colorTexInfo{
        .size = {1024, 1024, 1},
        .colorType {FISIR::TextureCOLORType::RGBA_8},
        .type {FISIR::TextureType::TEXTURE2D},
        .useFor {FISIR::TextureUseForColorAttachment | FISIR::TextureUseForTransferSrc | FISIR::TextureUseForTransferDst | FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForInputAttachment},
        .mipLevels{ 1 }, .arrayLayers{ 1 }, .sampleCount{ 0 }
    };
    auto colorTexture = rhi->RHICreateTexture(colorTexInfo);

    FISIR::TextureInfo depthTexInfo{
        .size = {1024, 1024, 1},
        .colorType{ FISIR::TextureCOLORType::Depth24_Stencil8 },
        .type{ FISIR::TextureType::TEXTURE2D },
        .useFor{ FISIR::TextureUseForDepthStencilAttachment },
        .mipLevels{ 1 }, .arrayLayers{ 1 }, .sampleCount{ 0 }
    };
    auto depthTexture = rhi->RHICreateTexture(depthTexInfo);

    // RenderPass
    FISIR::ColorEntry colorEntry{ {.loadOp = FISIR::RenderTargetLoadAction::Clear, .storeOp = FISIR::RenderTargetStoreAction::Store, .dstLayout = FISIR::TextureLayout::ShaderReadOnlyOptimal, .colorType = FISIR::TextureCOLORType::RGBA_8, .sampleCount = 0 } };
    FISIR::DepthStencilEntry depthStencilEntry{ .sampleCount = 0, .dstLayout = FISIR::TextureLayout::DepthStencilAttachmentOptimal, .exeit = true };
    depthStencilEntry.depthAction.setDWAndSW(true, true);
    FISIR::SubPassInfo subPassInfo{ .ColorEntryMask = 1, .UseDepthStencil = true, .ReadDepthAsInput = false };
    FISIR::RHIRenderPassInfo renderPassInfo({ {0, colorEntry} }, depthStencilEntry, { subPassInfo });
    auto renderPass = rhi->RHICreateRenderPass(renderPassInfo);

    auto framebuffer = rhi->RHICreateFrameBuffer(1024, 1024, { colorTexture, depthTexture }, renderPassInfo);

    // ---------- 6. 管线描述 ----------
    FISIR::RHIPipelineDescribeInfo describeInfo{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage},
        {1, 1, FISIR::RHIDescriptorTyp::SamplerImage, FISIR::RHIUsingStage::FragmentShaderStage},
        {2, 1, FISIR::RHIDescriptorTyp::Sampler, FISIR::RHIUsingStage::FragmentShaderStage}
    };

    FISIR::RHIVertexInputInfo vertexInputInfo{
        FISIR::RHIBaseDataTYPE::_Fvec3,
        FISIR::RHIBaseDataTYPE::_Fvec3,
        FISIR::RHIBaseDataTYPE::_Fvec2
    };

    FISIR::RHIPipelineState pipelineState{
        .describeInfo = describeInfo,
        .vertexInfo = vertexInputInfo,
        .topologyType = FISIR::TopologyType::Triangle,
        .rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
        .depthStencilState = {1, 1, 0, 0.0, 1.0, FISIR::_Equal_Less_},
        .colorblendState = {.UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_) },
        .renderpass = framebuffer->getFrameRenderPass(),
    };
    pipelineState.Shaders[FISIR::__VERTEXSHADER__] = vs;
    pipelineState.Shaders[FISIR::__FRAGMENTSHADER__] = ps;

    auto pipeline = rhi->RHICreatePipeline(pipelineState);

    // ---------- 7. MVP 常量缓冲 ----------
    FISIR::BufferInfo mvpBufferInfo{
        .data_CPU = nullptr,
        .size = sizeof(FrameUniforms),
        .stride = sizeof(FrameUniforms),
        .bufferlayout = FISIR::UniformBuffer,
        .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    FISIR::RHIBuffer* mvpBuffers[5];
    for (int i = 0; i < 5; i++) {
        mvpBuffers[i] = rhi->RHICreateBuffer(mvpBufferInfo);
    }

    // ---------- 8. 加载纹理 ----------
    // 平台差异在 Platform::LoadAsset：桌面按相对路径读磁盘，Android 从 APK 的 assets 里读。
    int texWidth = 0, texHeight = 0, texChannels = 0;
    const char* texturePath = "Test3.png";
    std::vector<unsigned char> textureBytes;
    unsigned char* textureData = nullptr;
    if (Platform::LoadAsset(texturePath, textureBytes) && !textureBytes.empty()) {
        textureData = stbi_load_from_memory(textureBytes.data(), (int)textureBytes.size(),
                                            &texWidth, &texHeight, &texChannels, 4);
    }
    FISIR::RHIBuffer* uploadBuffer = nullptr;
    FISIR::RHITexture* inputTexture = nullptr;
    if (textureData) {
        Warn("Try Input Image Data To Gpu");
        FISIR::TextureInfo inputTexInfo{
            .size = { static_cast<uint32_t>(texHeight), static_cast<uint32_t>(texWidth), 1 },
            .colorType = FISIR::TextureCOLORType::RGBA_8,
            .type = FISIR::TextureType::TEXTURE2D,
            .useFor = FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForTransferDst | FISIR::TextureUseForInputAttachment,
            .mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,
        };
        inputTexture = rhi->RHICreateTexture(inputTexInfo);

        FISIR::BufferInfo uploadBufferInfo{
            .data_CPU = textureData,
            .size = (size_t)texHeight * texWidth * 4,
            .stride = 0,
            .bufferlayout = FISIR::TransferSrcBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
        };
        uploadBuffer = rhi->RHICreateBuffer(uploadBufferInfo);

        FISIR::RHIRenderCommandList uploadCmdList(rhi);

        FISIR::RHITexture* texArray[] = { inputTexture };
        uploadCmdList.TransitionTextures(texArray, 1,
            FISIR::ResourceAccess::Undefined, FISIR::ResourceAccess::TransferDst,
            FISIR::TextureLayout::Undefined, FISIR::TextureLayout::TransferDstOptimal,
            FISIR::RHIUsingStage::NoneStage, FISIR::RHIUsingStage::PipelineTransferStage);

        Warn("Try Copy Data To Texture");
        uploadCmdList.CopyToTexture(uploadBuffer, inputTexture, 0, 0, 1, 0, { 0,0,0 }, inputTexInfo.size);
        // 这里需要调用 TransitionTextures，但注意参数：需要传入 RHITexture** 和 count，以及各个标志
        
        Warn("Try Translate Texture To Shader Read Only");

        uploadCmdList.TransitionTextures(texArray, 1,
            FISIR::ResourceAccess::TransferDst, FISIR::ResourceAccess::ShaderReadOnly,
            FISIR::TextureLayout::TransferDstOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
            FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);
		auto resfence = rhi->RHICreateFence();
        uploadCmdList.End(resfence);
        resfence->wait();
        rhi->RHIDestroyFence(resfence);
        stbi_image_free(textureData);
    } else {
        Error("Failed to load texture '{}' — check working directory!", texturePath);
    }

    FISIR::SamplerInfo samplerInfo;
    auto sampler = rhi->RHICreateSampler(samplerInfo);

    // ---------- 9. 资源包 ----------
    FISIR::RHIResourcePackResult resourcePacks[5];
    for (int i = 0; i < 5; i++) {
        resourcePacks[i] = rhi->RHICreateResourcePack({ mvpBuffers[i], inputTexture, sampler });
    }

    // ---------- 10. 顶点数据 ----------
    struct Vertex { float pos[3]; float color[3]; float uv[2]; };
    std::vector<Vertex> cubeVertices;
    cubeVertices.reserve(36);
    float s = 0.3f;
    // 前
    cubeVertices.push_back({ {-s, -s,  s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ { s, -s,  s}, {1,1,1}, {1,1} });
    cubeVertices.push_back({ {-s,  s,  s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ {-s,  s,  s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ { s, -s,  s}, {1,1,1}, {1,1} });
    cubeVertices.push_back({ { s,  s,  s}, {1,1,1}, {1,0} });
    // 后
    cubeVertices.push_back({ { s,  s, -s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ { s, -s, -s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ {-s,  s, -s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ {-s,  s, -s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ { s, -s, -s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ {-s, -s, -s}, {1,1,1}, {1,1} });
    // 左
    cubeVertices.push_back({ {-s, -s, -s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ {-s,  s, -s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ {-s, -s,  s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ {-s, -s,  s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ {-s,  s, -s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ {-s,  s,  s}, {1,1,1}, {1,1} });
    // 右
    cubeVertices.push_back({ { s,  s,  s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ { s, -s,  s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ { s,  s, -s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ { s,  s, -s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ { s, -s,  s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ { s, -s, -s}, {1,1,1}, {1,1} });
    // 上
    cubeVertices.push_back({ {-s,  s, -s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ { s,  s, -s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ {-s,  s,  s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ {-s,  s,  s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ { s,  s, -s}, {1,1,1}, {1,0} });
    cubeVertices.push_back({ { s,  s,  s}, {1,1,1}, {1,1} });
    // 下
    cubeVertices.push_back({ {-s, -s,  s}, {1,1,1}, {0,1} });
    cubeVertices.push_back({ { s, -s,  s}, {1,1,1}, {1,1} });
    cubeVertices.push_back({ {-s, -s, -s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ {-s, -s, -s}, {1,1,1}, {0,0} });
    cubeVertices.push_back({ { s, -s,  s}, {1,1,1}, {1,1} });
    cubeVertices.push_back({ { s, -s, -s}, {1,1,1}, {1,0} });

    FISIR::BufferInfo vertexBufferInfo{
        .data_CPU = cubeVertices.data(),
        .size = sizeof(Vertex) * cubeVertices.size(),
        .bufferlayout = FISIR::VertexBuffer | FISIR::TransferSrcBuffer,
        .memoryType = FISIR::MemTypHostVisable
    };
    auto vertexBuffer = rhi->RHICreateBuffer(vertexBufferInfo);

    std::vector<uint32_t> indices(36);
    for (uint32_t i = 0; i < 36; ++i) indices[i] = i;
    FISIR::BufferInfo indexBufferInfo{
        .data_CPU = indices.data(),
        .size = sizeof(uint32_t) * indices.size(),
        .stride = sizeof(uint32_t),
        .bufferlayout = FISIR::BufferLayout::IndexBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostCoherent | FISIR::MemTypHostVisable)
    };
    auto indexBuffer = rhi->RHICreateBuffer(indexBufferInfo);

    // ---------- 11. 交换链 ----------
    // Headless（以及后端未实现的呈现设备）没有 surface ⇒ 这里拿到 nullptr，本示例退化成
    // "只渲染到离屏 colorTexture"，present 与交换链相关的等待全部跳过。
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    const bool hasSwapChain = (swapchain != nullptr);
    FISIR::RHIPipeline* swapchainPipeline = hasSwapChain ? swapchain->getSwapChainRenderPipeline() : nullptr;
    FISIR::SamplerInfo swapSamplerInfo;
    auto swapSampler = rhi->RHICreateSampler(swapSamplerInfo);
    if (hasSwapChain) {
        // 呈现资源包改由 swapchain 管理：登记离屏纹理（纹理模式，BufferEnable=0，PS 采样纹理）。
        swapchain->enableTextureInput(colorTexture, swapSampler);

#ifdef __ANDROID__
        // 安卓默认仍用 MAILBOX（不限帧），便于观察吞吐；但想得到「和屏幕刷新率一致、稳定且省电」
        // 的行为（也让全屏/小窗表现一致），设 FISIR_ANDROID_VSYNC=1 即可切到 FIFO（垂直同步）。
        if (const char* vs = getenv("FISIR_ANDROID_VSYNC"); vs && vs[0] == '1') {
            swapchain->sync(true);
            Info("[Android] FISIR_ANDROID_VSYNC=1 → 切 FIFO（帧率锁定到屏幕刷新率）");
        }
#endif
    } else {
        Info("无交换链：跳过 present，离屏结果由 -GetFrames 读回");
    }
    // 无窗口/无交换链时没有"点叉关闭"这条出口，必须给个帧数上限，否则脚本会挂住。
    if (!hasSwapChain || hiddenWindow) {
        if (!exitAfterFrames && !runPerfTest) {
            Error("{} 模式没有可关闭的窗口，请用 -ExitAfter N（或 -Test -Frames N）指定跑多少帧后退出",
                  FISIR::DisplayDeviceTypeName(displayType));
            return 1;
        }
        // 没人看得见画面：默认截最后一帧当证据（可用 -GetFrames 覆盖）。
        if (!getFrames && exitAfterFrames > 0) {
            getFrames = true;
            getFrameBegin = getFrameEnd = exitAfterFrames;
            Info("自动启用截图：读回第 {} 帧的离屏结果", exitAfterFrames);
        }
    }
    // ---------- 11.6 安卓端 ImGui（帧率面板）----------
    // 必须在 rhi->Init() 之后建：后端会用它建字体图集纹理、采样器与顶点/索引缓冲。
    // 管线是**按 render pass 懒建**的（见 ImGui_Impl_FISIR.cpp 的说明），所以这里先不指定目标，
    // 真正画的时候传入呈现 pass 即可。
#ifdef __ANDROID__
    bool imguiReady = false;
    if (hasSwapChain) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& imguiIO = ImGui::GetIO();
        imguiIO.IniFilename = nullptr;      // 安卓上没有可写的 .ini，省掉这套文件读写
        imguiIO.LogFilename = nullptr;
        imguiIO.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;   // 手机上不需要多视口
        // 默认字体是 13px，在 2400×1080 上小得看不清：按 48px 光栅化（比 FontGlobalScale 清晰，
        // 因为图集就是按这个字号生成的）。必须在 ImGui_ImplFISIR_Init 之前加好 —— 后者会立刻上传图集。
        ImFontConfig imguiFontCfg;
        imguiFontCfg.SizePixels = 48.0f;
        imguiIO.Fonts->AddFontDefault(&imguiFontCfg);
        if (ImGui_ImplFISIR_Init(rhi)) {
            imguiReady = true;
            Info("[Android] ImGui 帧率面板已启用");
        } else {
            Error("[Android] ImGui_ImplFISIR_Init 失败，本示例继续跑（只是没有帧率面板）");
        }
    }
    auto imguiLastFrame = std::chrono::steady_clock::now();
    // 会话级帧率统计（自启动累计；见帧循环里的说明）。声明在此处是为了不跨过后面的 goto cleanup。
    double   imguiSumDt  = 0.0;
    uint64_t imguiFrames = 0;
    float    imguiMinMs  = 1e9f;
    float    imguiMaxMs  = 0.0f;
    // CPU 侧耗时拆解累加（微秒）。全屏与非全屏在 Android 上走的是**不同的合成路径**
    //（全屏更容易被提升为 direct scanout / 硬件 overlay，非全屏走 GPU 合成），
    // 「慢」到底是慢在 acquire（等交换链图像，被合成器或显示管线节流）还是慢在我们自己的录制，
    // 只有这组数据能区分 —— 单看帧率分不出来。
    double   imguiAccAcquireUs = 0.0;
    double   imguiAccRecordUs  = 0.0;
    double   imguiAccWaitUs    = 0.0;
    double   imguiAccOtherUs   = 0.0;
    uint64_t imguiBreakdownFrames = 0;
#endif

    // 无交换链（headless）时自建的槽围栏：每个缓冲槽一个、跨帧复用；收尾时等 GPU 完成再销毁。
    FISIR::RHIFence* frameFences[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    bool frameFenceUsed[5] = { false, false, false, false, false };
    // 收尾时要等的「最后几帧围栏」：交换链路径登记 info.finishFence（归交换链所有，只等不销毁），
    // headless 路径登记自建的 frameFences。销毁资源前必须等它们真的跑完。
    FISIR::RHIFence* shutdownFences[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };

    // ---------- 11.5 截图读回 ----------
    FISIR::BufferInfo readbackInfo{
        .data_CPU = nullptr,
        .size = (uint64_t)1024 * 1024 * 4,
        .stride = 0,
        .bufferlayout = FISIR::TransferDstBuffer,
        .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
    };

    struct ScreenshotTask {
        FISIR::RHIFence* fence;
        FISIR::RHIBuffer* buffer;
        uint64_t frameIndex;
        bool ownsFence;   // false = 围栏是交换链/headless 的槽围栏，不能在这里销毁
    };
    FISIR::LockFreeQue<ScreenshotTask, 256> screenshotTasks;
    std::atomic<bool> screenshotStop{false};
    const uint64_t screenshotTotal = (getFrames && getFrameEnd >= getFrameBegin)
        ? (getFrameEnd - getFrameBegin + 1) : 0;
    std::atomic<uint64_t> screenshotDone{0};
    // 已投递给 worker 的截图任务数。收尾时要等它全部落盘 —— 截图的 GPU 等待与写文件都在 worker
    // 线程上，若主线程直接销毁 colorTexture/framebuffer，就会和在飞的拷贝打架（实测 0xC0000409）。
    std::atomic<uint64_t> screenshotPushed{0};
    // 每帧量化后的索引像素，按帧序（frameIndex - getFrameBegin 作下标）。预先定长，
    // 各线程只写各自下标，互不冲突，也无需按完成顺序排序。

    const int SCREENSHOT_WORKERS = 25;
    std::vector<std::thread> screenshotWorkers;
    if (getFrames) {
        Info("Screenshot enabled: frames [{}..{}]", getFrameBegin, getFrameEnd);
        screenshotWorkers.reserve(SCREENSHOT_WORKERS);
        for (int t = 0; t < SCREENSHOT_WORKERS; ++t) {
            screenshotWorkers.emplace_back([&]() {
                while (true) {
                    ScreenshotTask task{};
                    if (screenshotStop.load()) break;
                    if (!screenshotTasks.pop_wait(task)) continue;
                    // 等 GPU 拷贝完成

                    if (task.fence) task.fence->wait();
                    const unsigned char* rgba = static_cast<const unsigned char*>(task.buffer->getBufferData());

                    // 写单帧 BMP
                    char bmpPath[64];
                    snprintf(bmpPath, sizeof(bmpPath), "Screenshot_%llu.bmp", (unsigned long long)task.frameIndex);
                    WriteBMP(bmpPath, 1024, 1024, rgba);
                    Info("Screenshot saved: {}", bmpPath);

                    // 量化并按帧序存入对应槽位
                    std::vector<uint8_t> indices(1024 * 1024);

                    // 回收本帧的 fence/buffer（围栏若归交换链所有则只还 buffer）
                    if (task.ownsFence) rhi->RHIDestroyFence(task.fence);
                    rhi->RHIDestroyBuffer(task.buffer);

                    // 全部帧处理完 → 由最后完成的线程串 GIF → 通知所有线程退出
                    uint64_t done = screenshotDone.fetch_add(1) + 1;
                    if (done == screenshotTotal) {
                        Info("Done wait GIF");
                        screenshotTasks.stopQue();
                        screenshotStop.store(true);

                        break;
                    }
                }
            });
        }
    }

    // ---------- 12. 清空值 ----------
    FISIR::ClearValue clearFrame{ .ColorClear = 1, .colorinfo = {0.1f, 0.1f, 0.2f, 1.0f}, .depthclearval = 1.0f };
    FISIR::ClearValue clearPresent{ .ColorClear = 1, .colorinfo = {0.0f, 0.0f, 0.0f, 1.0f}, .DepthStencilClear = 0 };

    // ---------- 13. 主循环 ----------
    float angle = 0.0f;
    float aspect = 1024.0f / 1024.0f;
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 50.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.0f, 3.0f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 viewProj = proj * view; // 自旋模式下 Model 在 shader 单独应用，view*proj 每帧不变

    uint64_t frameCount = 0;
    Warn("Begin Main Loop");
    std::vector<std::pair<uint32_t, uint32_t>> testGroups;
    testGroups.reserve(drawCallCounts.size() * instanceCounts.size());
    for (uint32_t dc : drawCallCounts)
        for (uint32_t ins : instanceCounts)
            testGroups.emplace_back(dc, ins);

    for (auto& [dcCount, insCount] : testGroups) {
        auto startTime = std::chrono::steady_clock::now();
        auto fpsStart = startTime;
        angle = 0.0f;
        frameCount = 0;
        std::vector<double> frameTimes;
        if (runPerfTest) {
            frameTimes.reserve(static_cast<size_t>(testFrameCount));
            Warn("Performance Test Mode: {} DrawCall(s) x {} Instance(s), target {} frames", dcCount, insCount, testFrameCount);
        }

        // 网格布局：把 dcCount × insCount 个立方体按行列铺满视口（不重叠、测极限）
        uint32_t totalCubes = dcCount * insCount;
        float halfH = 3.0f * std::tan(glm::radians(30.0f)); // 相机距离 3、FOV 60° 在 z=0 的可见半高
        float halfW = halfH * aspect;
        uint32_t gridCols = (uint32_t)std::ceil(std::sqrt((double)totalCubes * (double)aspect));
        uint32_t gridRows = (uint32_t)std::ceil((double)totalCubes / (double)gridCols);
        glm::vec4 gridParams((float)gridCols, (float)gridRows,
                             2.0f * halfW / (float)gridCols,
                             2.0f * halfH / (float)gridRows);

        // ── 相位计时（微秒累计）──
        double accAcquire = 0, accRecord = 0, accWaitSubmit = 0, accPresent = 0, accOther = 0;
        uint64_t accFrames = 0;

        while (true) {
            auto now = std::chrono::steady_clock::now();

            // 平台事件：Win32 抽 PeekMessage（拿到 WM_QUIT 就结束）；Android 抽 looper
            // 并处理 APP_CMD_* 生命周期（窗口被系统回收 → 结束，见 PlatformAndroid.cpp 说明）。
            if (!Platform::PumpEvents(window)) goto cleanup;
            //Warn("Main Running");
            // 更新 Model（自旋）
            glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
            FrameUniforms uniforms{ viewProj, model, gridParams };

            //Info("MTag0");

            auto tAcq0 = std::chrono::steady_clock::now();
            uint32_t infoid = 0;
            FISIR::SwapChainGetImageInfo info{};
            if (hasSwapChain) {
                infoid = swapchain->acquireGetImageInfoID();
                if (infoid == FISIR::RHISwapChain::FAILEID) continue;
                info = swapchain->getSwapChainGetImageInfo(infoid);
            } else {
                // 无交换链：自己按 5 个缓冲槽轮转。
                // 注意：这里**没有 present 做节流**，CPU 会远远跑在 GPU 前面 —— 复用某一槽之前
                // 必须确认 GPU 已经用完它，否则会改写 GPU 还没读的 uniform，画面（以及截图）不可复现。
                infoid = static_cast<uint32_t>(frameCount % 5);
                FISIR::RHIFence* slotFence = frameFences[infoid];
                if (slotFence && slotFence->isSubmited()) slotFence->wait();
            }
            auto tAcq1 = std::chrono::steady_clock::now();
            static_cast<FISIR::RHIBuffer*>(mvpBuffers[infoid])->updateBufferData(&uniforms, sizeof(FrameUniforms));

            ++frameCount;
            if (frameCount % 100 == 0) {
                float elapsed = std::chrono::duration<float>(now - fpsStart).count();
                float fps = 100.0f / elapsed;
                fpsStart = now;
                char title[128];
                snprintf(title, sizeof(title),
                    "Test Cube |%.1f FPS", fps);
                Platform::SetTitle(window, title);   // Win32 改标题；Android 是空实现
            }


#ifdef __ANDROID__
            // ImGui 帧：没有输入后端，DisplaySize/DeltaTime 要自己喂。FPS 由 ImGui 依据
            // DeltaTime 做滚动平均（io.Framerate），所以这里用 steady_clock 的真实帧间隔。
            if (imguiReady) {
                const auto imguiNow = std::chrono::steady_clock::now();
                const float imguiDt = std::chrono::duration<float>(imguiNow - imguiLastFrame).count();
                imguiLastFrame = imguiNow;

                ImGuiIO& imguiIO = ImGui::GetIO();
                imguiIO.DisplaySize = ImVec2((float)viewport->getViewportWidth(), (float)viewport->getViewportHeight());
                imguiIO.DeltaTime = (imguiDt > 0.0f && imguiDt < 1.0f) ? imguiDt : (1.0f / 60.0f);

                // 会话级统计：io.Framerate 只是**滚动**平均（约最近 60 帧），看不出整体水平与抖动。
                // 这里另外累加「自启动以来的平均帧率」和帧时间的极值 —— 对比不同设备/不同场景
                // （例如 8+ Gen 1 上的 400+）时，看的是这几项，而不是某一瞬间的读数。
                // 注意：呈现模式是 MAILBOX（不限帧）时帧率代表「循环吞吐」而非屏幕刷新率，
                // 也不直接等于 GPU 能力；要量化 GPU 开销应看帧时间分布或 GPU 时间戳。
                const float imguiMs = imguiIO.DeltaTime * 1000.0f;
                imguiSumDt += imguiIO.DeltaTime;
                ++imguiFrames;
                if (imguiMs < imguiMinMs) imguiMinMs = imguiMs;
                if (imguiMs > imguiMaxMs) imguiMaxMs = imguiMs;
                const double imguiAvgFps = (imguiSumDt > 0.0) ? ((double)imguiFrames / imguiSumDt) : 0.0;

                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.55f);
                ImGui::Begin("##fisir_fps", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove);
                ImGui::Text("FPS   %.1f", imguiIO.Framerate);                 // 滚动（约最近 60 帧）
                ImGui::Text("avg   %.1f", (float)imguiAvgFps);                // 会话平均
                ImGui::Text("ms    %.2f", imguiMs);
                ImGui::Text("min/max %.2f/%.2f", imguiMinMs, imguiMaxMs);
                ImGui::Text("frames %llu", (unsigned long long)imguiFrames);
                // CPU 侧拆解（累计平均）：全屏 vs 非全屏的差别几乎总出现在 acquire 一栏
                //（等图像 = 被合成器/显示管线节流），而 record 一栏基本不变。
                if (imguiBreakdownFrames > 0) {
                    const double bf = (double)imguiBreakdownFrames * 1000.0;   // us → ms（累计和 / 帧数）
                    const double acqMs = imguiAccAcquireUs / bf, recMs = imguiAccRecordUs / bf;
                    const double wtMs = imguiAccWaitUs / bf, othMs = imguiAccOtherUs / bf;
                    ImGui::Text("cpu   acq %.2f  rec %.2f", acqMs, recMs);
                    ImGui::Text("      wait %.2f  other %.2f", wtMs, othMs);
                    // 每 300 帧把同一组数据写进日志：手机上不用截图 OCR，adb 就能读
                    if (imguiFrames % 300 == 1) {
                        Info("[Android] 帧统计: avg {:.1f} fps | 帧 {:.2f} ms (min {:.2f}/max {:.2f}) | "
                             "CPU: acquire {:.2f} / record {:.2f} / waitSubmit {:.2f} / other {:.2f} ms | "
                             "extent {}x{} | frames {}",
                             (float)imguiAvgFps, imguiMs, imguiMinMs, imguiMaxMs,
                             acqMs, recMs, wtMs, othMs,
                             viewport->getViewportWidth(), viewport->getViewportHeight(),
                             (unsigned long long)imguiFrames);
                    }
                }
                ImGui::End();
                ImGui::Render();
                // 安卓下没有平台窗口，这个函数是 no-op；保留调用是为了和桌面的每帧序列一致。
                ImGui_ImplFISIR_PrepareViewportSwapChains();
            }
#endif

            auto tRec0 = std::chrono::steady_clock::now();
            // 渲染到离屏 Framebuffer
            FISIR::RHIRenderCommandList cmdList(rhi);

            cmdList.BeginRenderPass(framebuffer, 0, clearFrame);
            cmdList.SetPipelineState(pipeline);
            cmdList.SetVertexBuffer(vertexBuffer, 0, 0);
            cmdList.SetIndexBuffer(indexBuffer, 0);
            cmdList.SetResourcePack(resourcePacks[infoid]);
            cmdList.SetViewPort(0, 0, 1024, 1024, 1.0f, 0.0f);
            cmdList.SetScissor(1024, 1024);
            for (uint32_t d = 0; d < dcCount; ++d) cmdList.DrawIndex(0, 36, d * insCount, insCount);
            cmdList.EndRenderPass();
            // 呈现到交换链（无交换链时整个 present pass 跳过 —— 离屏 colorTexture 就是最终产物）
            if (hasSwapChain) {
                auto frameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);
                if (frameBuf) {
                    cmdList.BeginRenderPass(frameBuf, 0, clearPresent);
                    cmdList.SetPipelineState(swapchainPipeline);
                    cmdList.SetResourcePack(swapchain->getSwapchainResourcePack());
                    cmdList.SetViewPort(0, 0, viewport->getViewportWidth(), viewport->getViewportHeight(), 1.0f, 0.0f);
                    cmdList.SetScissor(viewport->getViewportWidth(), viewport->getViewportHeight());
                    cmdList.DrawPrimitive(0, 3, 1);
#ifdef __ANDROID__
                    // ImGui 叠在**最终呈现**这一趟上：分辨率就是屏幕，字号清晰、位置准确。
                    // 放在全屏四边形之后 ⇒ 画在画面最上层（不依赖深度测试）。
                    if (imguiReady && ImGui::GetDrawData()) {
                        ImGui_ImplFISIR_RenderDrawData(cmdList, frameBuf->getFrameRenderPass(),
                                                              ImGui::GetDrawData());
                    }
#endif
                    cmdList.EndRenderPass();
                }
            }
            // 截图帧：读回**写在本帧同一条命令缓冲里**（SRO → TransferSrc → 拷贝 → SRO），
            // 顺序由「同一页、按录制序提交」天然保证：拷贝必然落在本帧渲染之后、下一帧渲染之前。
            // 原先是另开一条列表异步提交，会和下一帧对同一张离屏纹理的渲染重叠 —— 偶发捕获到
            // 相位不同的帧（实测同一设备两次运行位图不同，跨设备比对因此不可信）。
            const bool isScreenshotFrame = getFrames && frameCount >= getFrameBegin && frameCount <= getFrameEnd;
            FISIR::RHIBuffer* readback = nullptr;
            if (isScreenshotFrame) {
                readback = rhi->RHICreateBuffer(readbackInfo);
                FISIR::RHITexture* texArray[] = { colorTexture };
                cmdList.TransitionTextures(texArray, 1,
                    FISIR::ResourceAccess::ShaderReadOnly, FISIR::ResourceAccess::TransferSrc,
                    FISIR::TextureLayout::ShaderReadOnlyOptimal, FISIR::TextureLayout::TransferSrcOptimal,
                    FISIR::RHIUsingStage::FragmentShaderStage, FISIR::RHIUsingStage::PipelineTransferStage);
                cmdList.CopyImageToBuffer(colorTexture, readback, 0, 0, 1, { 0,0,0 }, 0, { 1024, 1024, 1 });
                // 转回 SRO：后面的 present pass 还要采样这张纹理，且要与 renderpass 的 finalLayout 一致。
                cmdList.TransitionTextures(texArray, 1,
                    FISIR::ResourceAccess::TransferSrc, FISIR::ResourceAccess::ShaderReadOnly,
                    FISIR::TextureLayout::TransferSrcOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
                    FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);
            }

            if (hasSwapChain) {
                // 渲染提交：wait = swapchain 可用，signal = 呈现信号量。
                std::vector<FISIR::RHISemaphore*> renderWaits{ info.avaliable };
                std::vector<FISIR::RHISemaphore*> renderSignals{ info.renderFinish };
                // 呈现录成指令（写在 End 之前），由 RHI 线程在本页提交之后执行 vkQueuePresentKHR。
                cmdList.Present(swapchain, infoid);
                cmdList.End(info.finishFence, renderWaits, renderSignals);
                shutdownFences[infoid] = info.finishFence;
            } else {
                // 无交换链：用自建围栏提交（只当节流与截图等待用），每个缓冲槽一个、跨帧复用
                //（与交换链的槽围栏同一套用法，避免每帧新建/销毁围栏）。
                if (!frameFences[infoid]) frameFences[infoid] = rhi->RHICreateFence(false, "HeadlessFrameFence");
                frameFenceUsed[infoid] = true;
                // **提交前必须 reset()**：交换链路径是 tryAcquire 里的 slot.finishFence->reset() 干的活。
                // 少了它，围栏对象里那个「已置位」缓存不会清，后面所有 wait() 都立刻返回
                //（VulkanFence::wait 先看缓存），截图 worker 就会读到还没拷贝完的缓冲 ——
                // 现象是 headless 每次运行的位图都不一样（实测）。
                frameFences[infoid]->reset();
                cmdList.End(frameFences[infoid], {}, {});
                shutdownFences[infoid] = frameFences[infoid];
            }
            auto tRec1 = std::chrono::steady_clock::now();
            if (hasSwapChain) info.finishFence->waitFenceSubmited();
            else             frameFences[infoid]->waitFenceSubmited();
            auto tWt1 = std::chrono::steady_clock::now();

            // present 已指令化、由 RHI 线程执行，主线程这里不再有 CPU 呈现开销
            // （accPresent 恒为 ~0）；tPr1 保留，作为「其它」时间段的起点。
            auto tPr1 = tWt1;

            // 截图任务在**本帧提交之后**交给 worker：它只等本帧那条围栏（不再自建 ScreenshotFence，
            // 所以 ownsFence=false），GPU 完成等待与写文件仍然后台做，主线程不阻塞。
            if (isScreenshotFrame) {
                FISIR::RHIFence* frameFence = hasSwapChain ? info.finishFence : frameFences[infoid];
                screenshotPushed.fetch_add(1);
                while(!screenshotTasks.push({ frameFence, readback, frameCount, /*ownsFence=*/false }));
            }

            angle += 0.02f;

            // ---------- 性能采样：记录本轮 CPU 帧耗时 ----------
            if (runPerfTest) {
                auto frameEnd = std::chrono::steady_clock::now();
                frameTimes.push_back(std::chrono::duration<double, std::milli>(frameEnd - now).count());
                auto us = [](auto a, auto b){ return std::chrono::duration<double, std::micro>(b - a).count(); };
                accAcquire   += us(tAcq0, tAcq1);
                accRecord    += us(tRec0, tRec1);
                accWaitSubmit+= us(tRec1, tWt1);
                accPresent   += us(tWt1, tPr1);
                accOther     += us(now, tAcq0) + us(tAcq1, tRec0) + us(tPr1, frameEnd);
                accFrames++;
                if (frameTimes.size() >= testFrameCount) break;
            }

#ifdef __ANDROID__
            // 安卓端常开的耗时拆解：与 -Test 模式共用同一组测量点（tAcq/tRec/tWt/tPr）。
            {
                auto frameEnd = std::chrono::steady_clock::now();
                auto us = [](auto a, auto b){ return std::chrono::duration<double, std::micro>(b - a).count(); };
                imguiAccAcquireUs += us(tAcq0, tAcq1);   // 等「下一条可写图像」= 被呈现节奏节流的量
                imguiAccRecordUs  += us(tRec0, tRec1);   // 录制命令缓冲（CPU 侧，与屏幕大小关系不大）
                imguiAccWaitUs    += us(tRec1, tWt1);    // 等本帧提交完成
                imguiAccOtherUs   += us(now, tAcq0) + us(tAcq1, tRec0) + us(tPr1, frameEnd);
                imguiBreakdownFrames++;
            }
#endif

            // 无窗口模式（hidden / headless）没有"点叉关窗口"这个出口：跑够 -ExitAfter 帧就收尾。
            if (exitAfterFrames && frameCount >= exitAfterFrames) {
                Info("ExitAfter {} 帧到达，正常收尾", exitAfterFrames);
                break;
            }
        }
        if (exitAfterFrames && frameCount >= exitAfterFrames) break;

        // ---------- 14. 性能报告导出（按 DrawCall 分组）----------
        if (runPerfTest) {
            double totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
            // 剔除预热帧：前 N 帧包含着色器/管线首帧编译与缓存未命中，不具代表性
            size_t warmup = static_cast<size_t>(std::min<uint64_t>(warmupFrameCount, frameTimes.size()));
            std::vector<double> measured(frameTimes.begin() + warmup, frameTimes.end());

            FISIR::PerfConfig cfg;
            cfg.offscreenWidth  = 1024;
            cfg.offscreenHeight = 1024;
            cfg.swapchainWidth  = viewport->getViewportWidth();
            cfg.swapchainHeight = viewport->getViewportHeight();
            cfg.sampleCount     = 1;
            cfg.presentMode     = "Mailbox";
            cfg.renderPassesPerFrame = 2;
            cfg.drawCallsPerFrame    = dcCount;
            cfg.instancesPerDraw     = insCount;
            cfg.verticesPerFrame     = 36 * dcCount * insCount;
            cfg.indicesPerFrame      = 36 * dcCount * insCount;
            cfg.totalFrames     = measured.size();
            cfg.totalSeconds    = totalSeconds;
            cfg.warmupFrames    = warmup;

            std::string mdPath  = "PerfReport_DC" + std::to_string(dcCount) + "_INS" + std::to_string(insCount) + ".md";
            std::string csvPath = "PerfFrameTimes_DC" + std::to_string(dcCount) + "_INS" + std::to_string(insCount) + ".csv";
            FISIR::writePerformanceReport(cfg, measured, mdPath, csvPath);
            Info("Performance report exported: {} / {}", mdPath, csvPath);

            if (accFrames) {
                Info("PHASE[us] acquire={:.1f} record={:.1f} waitSubmit={:.1f} present={:.1f} other={:.1f}",
                    accAcquire / accFrames, accRecord / accFrames,
                    accWaitSubmit / accFrames, accPresent / accFrames, accOther / accFrames);
            }
            // GPU 时间戳（最近一帧）与估算占用率 = GPU 帧耗时 / CPU 平均帧耗时。
            double gpuMs = rhi->getLastGPUTimeMs();
            double avgFrameMs = measured.empty() ? 0.0 :
                std::accumulate(measured.begin(), measured.end(), 0.0) / static_cast<double>(measured.size());
            if (gpuMs > 0.0 && avgFrameMs > 0.0)
                Info("GPU[ms] last={:.3f}  occupancy~={:.1f}%", gpuMs, 100.0 * gpuMs / avgFrameMs);
        }
    }

cleanup:
    // ---------- 14. 清理 ----------
    // 先等在飞的截图任务收尾：worker 线程要等 GPU 拷贝完成才写文件，而拷贝读的正是下面要销毁的
    // colorTexture。少了这一步，"-ExitAfter 紧跟在截图帧之后"就会撞上在飞的拷贝（实测 0xC0000409）。
    if (getFrames) {
        for (int waited = 0; waited < 500 && screenshotDone.load() < screenshotPushed.load(); ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (screenshotDone.load() < screenshotPushed.load())
            Warn("退出时仍有 {}/{} 张截图未落盘（等待超时 5s），继续清理",
                 screenshotPushed.load() - screenshotDone.load(), screenshotPushed.load());
    }
    // 关键顺序：**先让在飞的那几帧 GPU 落地，再销毁任何资源**。
    // RHI 是按页异步翻译/提交的：命令缓冲里绑着的 framebuffer/纹理/缓冲如果先被销毁，
    // 校验层会直接报 "objects bound to the command buffer were invalidated"，
    // 之后就是访问违例（Debug 实测 0xC0000005；Release 侥幸没崩，但同样是竞态）。
    for (int i = 0; i < 5; ++i) {
        FISIR::RHIFence* f = shutdownFences[i];
        if (f && f->isSubmited()) f->wait();          // 等这一次提交真的跑完（不是只等提交出去）
    }
    // 由 RHI 创建的所有资源对象都应经由 RHI 接口销毁，
    // 确保 new/delete 在同一个模块（RHIVK.dll）内完成，
    // 避免跨模块 new/delete 不匹配导致的堆损坏。
    for (int i = 0; i < 5; i++) {
        auto& rc = resourcePacks[i];
        if (rc.ResourcePack || rc.SamplerPack) rhi->RHIDestroyResourcePack(rc);
    }
    // 呈现资源包由 swapchain 拥有，随 destroyRenderInterface 的 swapchain 析构一并销毁，此处不再手动销毁。
    if (sampler)     rhi->RHIDestroySampler(sampler);
    if (swapSampler) rhi->RHIDestroySampler(swapSampler);
    for (int i = 0; i < 5; i++) {
        if (mvpBuffers[i]) rhi->RHIDestroyBuffer(mvpBuffers[i]);
    }
    if (vertexBuffer) rhi->RHIDestroyBuffer(vertexBuffer);
    if (indexBuffer)  rhi->RHIDestroyBuffer(indexBuffer);
    // framebuffer 引用了 colorTexture / depthTexture，必须先于它们销毁。
    if (framebuffer)  rhi->RHIDestroyFrameBuffer(framebuffer);
    if (colorTexture) rhi->RHIDestroyTexture(colorTexture);
    if (depthTexture) rhi->RHIDestroyTexture(depthTexture);
    if (uploadBuffer) rhi->RHIDestroyBuffer(uploadBuffer);
    if (inputTexture) rhi->RHIDestroyTexture(inputTexture);
    // headless 自建的槽围栏（上面的 wait 已经让 GPU 落定，这里可以安全还给池子）
    for (int i = 0; i < 5; ++i) {
        if (frameFences[i] && frameFenceUsed[i]) rhi->RHIDestroyFence(frameFences[i]);
    }
    screenshotStop.store(true);
    for (auto& t : screenshotWorkers) if (t.joinable()) t.join();
#ifdef __ANDROID__
    // ImGui 的资源（字体纹理/缓冲/管线）归后端所有，必须在拆 RHI 之前释放。
    if (imguiReady) {
        ImGui_ImplFISIR_Shutdown();
        ImGui::DestroyContext();
        imguiReady = false;
        Info("[Android] ImGui 已关闭");
    }
#endif
    Info("Destroy RHI Done!");
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    // 窗口/ANativeWindow 必须活到交换链销毁之后，所以放到最后收尾。
    Platform::Shutdown(window);
    return 0;
}
