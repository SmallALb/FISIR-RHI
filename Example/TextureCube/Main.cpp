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
#include <atomic>
#include <windows.h>
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

// 窗口回调
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

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

int main(int argc, char* argv[]) {
#ifdef _DEBUG
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
        }
    }
    if (drawCallCounts.empty()) drawCallCounts.push_back(1);
    if (instanceCounts.empty()) instanceCounts.push_back(1);

    if (runPerfTest) {
        Info("=======");
        Info("|Test Mode|");
        Info("========");

    }

    Debug("Main Thread 0x{:x}", std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // 1. 创建 RHI
    FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);
    FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();

    // 2. 创建窗口
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const char CLASS_NAME[] = "CubeDemo";
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, "Textured Cube (Single Pass)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600,
        NULL, NULL, hInstance, NULL
    );
    ShowWindow(hwnd, SW_SHOW);

    struct Win32Data { HINSTANCE hinstance; HWND hwnd; } win32Data{ hInstance, hwnd };
    auto viewport = rhi->RHICreateViewport(800, 600, FISIR::TextureCOLORType::RGBA_8, (void*)&win32Data);
    rhi->Init();

    // ---------- 3. 顶点着色器 ----------
    const wchar_t* vsCode = LR"(
        cbuffer MVPBuffer : register(b0) {
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
        Texture2D myTexture : register(t1);
        SamplerState mySampler : register(s2);
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
    int texWidth, texHeight, texChannels;
    const char* texturePath = "Test3.png";
    unsigned char* textureData = stbi_load(texturePath, &texWidth, &texHeight, &texChannels, 4);
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
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    auto swapchainPipeline = swapchain->getSwapChainRenderPipeline();
    FISIR::SamplerInfo swapSamplerInfo;
    auto swapSampler = rhi->RHICreateSampler(swapSamplerInfo);
    auto swapchainPack = rhi->RHICreateResourcePack({ colorTexture, swapSampler });

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
    };
    FISIR::LockFreeQue<ScreenshotTask, 256> screenshotTasks;
    std::atomic<bool> screenshotStop{false};
    const uint64_t screenshotTotal = (getFrames && getFrameEnd >= getFrameBegin)
        ? (getFrameEnd - getFrameBegin + 1) : 0;
    std::atomic<uint64_t> screenshotDone{0};
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

                    // 回收本帧的 fence/buffer
                    rhi->RHIDestroyFence(task.fence);
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

    MSG msg = { 0 };
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

        while (true) {
            auto now = std::chrono::steady_clock::now();

            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) goto cleanup;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            //Warn("Main Running");
            // 更新 Model（自旋）
            glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
            FrameUniforms uniforms{ viewProj, model, gridParams };

            //Info("MTag0");

            uint32_t infoid = swapchain->acquireGetImageInfoID();
            if (infoid == FISIR::RHISwapChain::FAILEID) continue;
            static_cast<FISIR::RHIBuffer*>(mvpBuffers[infoid])->updateBufferData(&uniforms, sizeof(FrameUniforms));
            if (infoid == FISIR::RHISwapChain::FAILEID) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                Error("Get Failed!");
                continue;
            }

            ++frameCount;
            if (frameCount % 100 == 0) {
                float elapsed = std::chrono::duration<float>(now - fpsStart).count();
                float fps = 100.0f / elapsed;
                fpsStart = now;
                char title[128];
                snprintf(title, sizeof(title),
                    "Test Cube |%.1f FPS", fps);
                SetWindowTextA(hwnd, title);
            }


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
            // 呈现到交换链
            auto info = swapchain->getSwapChainGetImageInfo(infoid);
            auto frameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);

            if (frameBuf) {
                cmdList.BeginRenderPass(frameBuf, 0, clearPresent);
                cmdList.SetPipelineState(swapchainPipeline);
                cmdList.SetResourcePack(swapchainPack);
                cmdList.SetViewPort(0, 0, viewport->getViewportWidth(), viewport->getViewportHeight(), 1.0f, 0.0f);
                cmdList.SetScissor(viewport->getViewportWidth(), viewport->getViewportHeight());
                cmdList.DrawPrimitive(0, 3, 1);
                cmdList.EndRenderPass();
            }
            // 截图帧：读回在同一条渲染队列内完成（SRO → TransferSrc → SRO），无需跨队列所有权转移。
            const bool isScreenshotFrame = getFrames && frameCount >= getFrameBegin && frameCount <= getFrameEnd;
            FISIR::RHIBuffer* readback = nullptr;
            if (isScreenshotFrame) {
                readback = rhi->RHICreateBuffer(readbackInfo);
            }

            // 渲染提交：wait = swapchain 可用，signal = 呈现信号量。
            std::vector<FISIR::RHISemaphore*> renderWaits{ info.avaliable };
            std::vector<FISIR::RHISemaphore*> renderSignals{ info.renderFinish };
            cmdList.End(info.finishFence, renderWaits, renderSignals);
            info.finishFence->waitFenceSubmited();

            swapchain->present(infoid);

            // 截图：在渲染队列内把离屏 colorTexture 拷贝到 readback 缓冲。与上一帧渲染同队列，
            // 提交顺序天然保证拷贝发生在离屏渲染完成之后；主线程只提交命令，等待 + 写文件放到后台线程。
            if (isScreenshotFrame) {
                FISIR::RHIRenderCommandList screenshotCmdList(rhi);
                FISIR::RHITexture* texArray[] = { colorTexture };
                // 转出：ShaderReadOnly → TransferSrc（同队列）
                screenshotCmdList.TransitionTextures(texArray, 1,
                    FISIR::ResourceAccess::ShaderReadOnly, FISIR::ResourceAccess::TransferSrc,
                    FISIR::TextureLayout::ShaderReadOnlyOptimal, FISIR::TextureLayout::TransferSrcOptimal,
                    FISIR::RHIUsingStage::FragmentShaderStage, FISIR::RHIUsingStage::PipelineTransferStage);
                screenshotCmdList.CopyImageToBuffer(colorTexture, readback, 0, 0, 1, { 0,0,0 }, 0, { 1024, 1024, 1 });
                // 转回：TransferSrc → ShaderReadOnly（还原布局，与 renderpass finalLayout 一致）
                screenshotCmdList.TransitionTextures(texArray, 1,
                    FISIR::ResourceAccess::TransferSrc, FISIR::ResourceAccess::ShaderReadOnly,
                    FISIR::TextureLayout::TransferSrcOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
                    FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);

                FISIR::RHIFence* screenshotFence = rhi->RHICreateFence(false, "ScreenshotFence");
                screenshotCmdList.End(screenshotFence, {}, {});
                //screenshotFence->waitFenceSubmited();

                // 提交任务给 worker 线程：GPU 完成等待与写文件在后台，主线程不阻塞。
                while(!screenshotTasks.push({ screenshotFence, readback, frameCount }));
            }

            angle += 0.02f;

            // ---------- 性能采样：记录本轮 CPU 帧耗时 ----------
            if (runPerfTest) {
                auto frameEnd = std::chrono::steady_clock::now();
                frameTimes.push_back(std::chrono::duration<double, std::milli>(frameEnd - now).count());
                if (frameTimes.size() >= testFrameCount) break;
            }
        }

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
        }
    }

cleanup:
    // ---------- 14. 清理 ----------
    // 由 RHI 创建的所有资源对象都应经由 RHI 接口销毁，
    // 确保 new/delete 在同一个模块（RHIVK.dll）内完成，
    // 避免跨模块 new/delete 不匹配导致的堆损坏。
    for (int i = 0; i < 5; i++) {
        auto& rc = resourcePacks[i];
        if (rc.ResourcePack || rc.SamplerPack) rhi->RHIDestroyResourcePack(rc);
    }
    if (swapchainPack.ResourcePack || swapchainPack.SamplerPack) rhi->RHIDestroyResourcePack(swapchainPack);
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
    screenshotStop.store(true);
    for (auto& t : screenshotWorkers) if (t.joinable()) t.join();
    Info("Destroy RHI Done!");
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    return 0;
}