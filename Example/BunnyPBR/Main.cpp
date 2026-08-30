// BunnyPBR —— 斯坦福兔子导入 + PBR + 多物体 OBB 碰撞样例
//
// 用法：BunnyPBR.exe [-C X] [-Test] [-Frames N] [-Warmup N]
//   -C X      : 兔子数量（可多次指定，每个 -C 生成一组测试；默认 10）
//   -Test     : 性能测试模式，跑完 -Frames 帧后导出报告并退出
//   -Frames N : 测试帧数（默认 5000）
//   -Warmup N : 预热帧数（默认 60）
//   - 模型：Res/bunny.ply（ASCII / binary_little_endian），缺失时程序化球体兜底
//   - 渲染：GGX dielectric PBR（金属锁 0），per-instance 数据经 StructuredBuffer + gl_InstanceIndex
//   - 仿真：全在 compute shader（积分 + OBB SAT 冲量 + 脚本自旋 + 撞墙），5 槽位环形缓冲 ping-pong
//
// 同步：compute 写入 bunnyState 后，CPU 侧 fence->wait() 保证 compute 完成；
//       随后渲染命令列表内 TransitionBuffers（ShaderWriteOnly→ShaderReadOnly）插入缓冲屏障。
//       注：若 GPU 的 compute 与 graphics 属不同队列族（NVIDIA/AMD 常见），此处缺一次
//       队列族所有权转移，运行时因 fence 串行化仍正确，但会触发 validation layer 提示。
//       见 .claude/Results/BunnyPBR-实现报告.md 的「遗留风险」。

#include <windows.h>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <random>
#include <fstream>
#include <iterator>
#include <algorithm>

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
#include "RHISemaphore.h"
#include "Log/Logger.h"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

#include "PLYLoader.h"
#include "PerformanceTest.h"

// ── 窗口回调 ──────────────────────────────────────────────────────
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// ── 从磁盘读取整个文件为字符串 ────────────────────────────────────
static std::string LoadFileText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Error("Failed to open shader file: {}", path);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// ── 着色器编译封装：加载 .hlsl → DXC 编译 → RHICreateShader ────────
static FISIR::RHIShader* CompileShader(FISIR::DynamicRHI* rhi, const char* file,
                                       FISIR::ShaderTYP typ, const char* entry, const char* target) {
    std::string source = LoadFileText(file);
    if (source.empty()) return nullptr;
    FISIR::ShaderComplier* compiler = new FISIR::ShaderComplier();
    compiler->compileShader(source.data(), source.size(), entry, target);
    auto* shader = rhi->RHICreateShader(typ, entry, compiler->getShaderData(), compiler->getShaderDataSize());
    delete compiler;
    return shader;
}

// ── GPU 侧兔子结构（与 BunnyPBR.hlsl / Simulation.hlsl 的 Bunny 严格对齐，96 字节）──
struct alignas(16) GPUBunny {
    float position[4];      // xyz + pad
    float orientation[4];   // 四元数 xyzw
    float velocity[4];      // xyz 线速度 + w 角速度
    float spinAxis[4];      // xyz + pad
    float halfExtents[4];   // OBB 半边长 xyz + pad
    float color[4];         // rgb 反照率 + w 粗糙度
};
static_assert(sizeof(GPUBunny) == 96, "GPUBunny 必须 96 字节");

// ── 渲染帧常量（b0，静态相机 + 静态光照 → 每帧不变）────────────────
struct alignas(16) FrameUB {
    glm::mat4 viewProj;         // 64
    glm::vec4 cameraPos;        // 16
    glm::vec4 lightDir;         // 16（光线行进方向）
    glm::vec4 lightColor;       // 16
    glm::vec4 pointLightPos;    // 16
    glm::vec4 pointLightColor;  // 16
};  // 144 字节

// ── 仿真参数（compute b0）────────────────────────────────────────
struct alignas(16) SimParams {
    float    dt;              // 帧步长
    float    boxHalf;         // 大盒体半边长
    float    restitution;     // 恢复系数 e
    float    pad0;
    uint32_t numBunnies;
    uint32_t pad1, pad2, pad3;
};  // 32 字节

// ── HSV → RGB（每兔一色）────────────────────────────────────────
static glm::vec3 hsvToRgb(float h, float s, float v) {
    float r, g, b;
    int i = (int)(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return glm::vec3(r, g, b);
}

// ── 初始化 N 只兔子（网格摆放避免初始重叠）────────────────────────
static void InitBunnies(std::vector<GPUBunny>& bunnies, uint32_t count,
                        float boxHalf, const float halfExtents[3]) {
    bunnies.resize(count);
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> rnd(0.0f, 1.0f);
    std::uniform_real_distribution<float> rndS(-1.0f, 1.0f);

    uint32_t gridSide = std::max(1u, (uint32_t)std::ceil(std::cbrt((double)count)));
    float spacing = (2.0f * boxHalf) / (float)gridSide;
    float jitter = spacing * 0.3f;

    for (uint32_t i = 0; i < count; ++i) {
        GPUBunny& b = bunnies[i];
        // 位置：网格 + 抖动
        b.position[0] = ((float)(i % gridSide) - (gridSide - 1) * 0.5f) * spacing + (rnd(rng) - 0.5f) * jitter;
        b.position[1] = ((float)((i / gridSide) % gridSide) - (gridSide - 1) * 0.5f) * spacing + (rnd(rng) - 0.5f) * jitter;
        b.position[2] = ((float)(i / (gridSide * gridSide)) - (gridSide - 1) * 0.5f) * spacing + (rnd(rng) - 0.5f) * jitter;
        b.position[3] = 0.0f;
        // 朝向：单位四元数
        b.orientation[0] = 0.0f; b.orientation[1] = 0.0f; b.orientation[2] = 0.0f; b.orientation[3] = 1.0f;
        // 速度：随机线速度 + 脚本角速度
        b.velocity[0] = (rnd(rng) - 0.5f) * 1.5f;
        b.velocity[1] = (rnd(rng) - 0.5f) * 1.5f;
        b.velocity[2] = (rnd(rng) - 0.5f) * 1.5f;
        b.velocity[3] = (rnd(rng) < 0.5f ? -1.0f : 1.0f) * (1.5f + rnd(rng) * 2.5f);
        // 自旋轴：随机单位向量
        float sx = rndS(rng), sy = rndS(rng), sz = rndS(rng);
        float slen = sqrtf(sx * sx + sy * sy + sz * sz);
        if (slen < 1e-4f) { sx = 0.0f; sy = 1.0f; sz = 0.0f; slen = 1.0f; }
        b.spinAxis[0] = sx / slen; b.spinAxis[1] = sy / slen; b.spinAxis[2] = sz / slen; b.spinAxis[3] = 0.0f;
        // OBB 半边长
        b.halfExtents[0] = halfExtents[0];
        b.halfExtents[1] = halfExtents[1];
        b.halfExtents[2] = halfExtents[2];
        b.halfExtents[3] = 0.0f;
        // 颜色：随机色相 + 粗糙度 0.25..0.95
        glm::vec3 c = hsvToRgb(rnd(rng), 0.7f, 0.9f);
        b.color[0] = c.r; b.color[1] = c.g; b.color[2] = c.b;
        b.color[3] = 0.25f + rnd(rng) * 0.7f;
    }
}

int main(int argc, char* argv[]) {
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
#endif

    // ── 0. 命令行参数解析 ──
    // -Test      : 启用性能测试，跑完指定帧数后导出报告并退出
    // -Frames N  : 测试帧数（默认 5000，样本量越大统计越稳定）
    // -Warmup N  : 预热帧数，前 N 帧（着色器/管线首帧编译等）不计入统计（默认 60）
    // -C X       : 兔子数量；可多次指定，每个 -C 生成一组测试（默认 10）
    bool runPerfTest = false;
    uint64_t testFrameCount = 5000;
    uint64_t warmupFrameCount = 60;
    std::vector<uint32_t> bunnyCounts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-Test" || arg == "-test" || arg == "/Test") {
            runPerfTest = true;
        } else if ((arg == "-Frames" || arg == "-frames") && i + 1 < argc) {
            testFrameCount = std::stoull(argv[++i]);
        } else if ((arg == "-Warmup" || arg == "-warmup") && i + 1 < argc) {
            warmupFrameCount = std::stoull(argv[++i]);
        } else if ((arg == "-C" || arg == "-c" || arg == "-Count" || arg == "-count") && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            if (v < 1) v = 1;
            if (v > 4096) { Warn("兔子数量 {} 超过上限 4096，已截断", v); v = 4096; }
            bunnyCounts.push_back((uint32_t)v);
        }
    }
    if (bunnyCounts.empty()) bunnyCounts.push_back(10);

    const uint32_t WIDTH = 1280, HEIGHT = 720;
    const uint32_t SLOT_COUNT = 5;   // 环形槽数，必须 == VulkanSwapChain::SWAPCHAIN_SLOT_COUNT

    if (runPerfTest) {
        Info("===========");
        Info("| Test Mode |");
        Info("===========");
    }

    // ── 1. 创建 RHI ──
    FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);
    FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();

    // ── 2. 创建窗口 / 视口 ──
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const char CLASS_NAME[] = "BunnyPBR";
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, CLASS_NAME, "Bunny PBR + OBB Collision",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, WIDTH, HEIGHT,
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);

    struct Win32Data { HINSTANCE hinstance; HWND hwnd; } win32Data{ hInstance, hwnd };
    auto viewport = rhi->RHICreateViewport(WIDTH, HEIGHT, FISIR::TextureCOLORType::RGBA_8, (void*)&win32Data);
    rhi->Init();

    // ── 3. 加载网格（PLY → uint32 索引 + 加权法线 + OBB 半边长）─────
    FISIR::MeshData mesh = FISIR::LoadMesh("Res/bunny.ply", 0.8f);
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        Error("网格为空，无法继续");
        return 1;
    }
    Info("Mesh: {} vertices, {} triangles, halfExtents = ({:.3f}, {:.3f}, {:.3f})",
         mesh.vertices.size(), mesh.indices.size() / 3,
         mesh.obbHalfExtents[0], mesh.obbHalfExtents[1], mesh.obbHalfExtents[2]);

    // ── 4. 顶点 / 索引缓冲（host-visible，构造时 memcpy）────────────
    FISIR::BufferInfo vbInfo{
        .data_CPU = mesh.vertices.data(),
        .size = sizeof(FISIR::MeshVertex) * mesh.vertices.size(),
        .bufferlayout = FISIR::VertexBuffer | FISIR::TransferSrcBuffer,
        .memoryType = FISIR::MemTypHostVisable
    };
    auto vertexBuffer = rhi->RHICreateBuffer(vbInfo);

    FISIR::BufferInfo ibInfo{
        .data_CPU = mesh.indices.data(),
        .size = sizeof(uint32_t) * mesh.indices.size(),
        .stride = sizeof(uint32_t),
        .bufferlayout = FISIR::IndexBuffer,
        .memoryType = (FISIR::MemType)(FISIR::MemTypHostCoherent | FISIR::MemTypHostVisable)
    };
    auto indexBuffer = rhi->RHICreateBuffer(ibInfo);

    // ── 5. 着色器编译 ──
    auto vs = CompileShader(rhi, "Shader/BunnyPBR.hlsl", FISIR::__VERTEXSHADER__, "mainVS", "vs_5_0");
    auto ps = CompileShader(rhi, "Shader/BunnyPBR.hlsl", FISIR::__FRAGMENTSHADER__, "mainPS", "ps_5_0");
    auto cs = CompileShader(rhi, "Shader/Simulation.hlsl", FISIR::__COMPUTESHADER__, "mainCS", "cs_6_0");
    if (!vs || !ps || !cs) { Error("着色器编译失败"); return 1; }

    // ── 6. 离屏渲染目标 + 渲染通道 + 帧缓冲 ──
    FISIR::TextureInfo colorTexInfo{
        .size = {HEIGHT, WIDTH, 1}, // TextureSize 字段序为 {height, width, depth}
        .colorType {FISIR::TextureCOLORType::RGBA_8},
        .type {FISIR::TextureType::TEXTURE2D},
        .useFor {FISIR::TextureUseForColorAttachment | FISIR::TextureUseForTransferSrc | FISIR::TextureUseForTransferDst | FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForInputAttachment},
        .mipLevels{1}, .arrayLayers{1}, .sampleCount{0}
    };
    auto colorTexture = rhi->RHICreateTexture(colorTexInfo);

    FISIR::TextureInfo depthTexInfo{
        .size = {HEIGHT, WIDTH, 1}, // TextureSize 字段序为 {height, width, depth}
        .colorType{FISIR::TextureCOLORType::Depth24_Stencil8},
        .type{FISIR::TextureType::TEXTURE2D},
        .useFor{FISIR::TextureUseForDepthStencilAttachment},
        .mipLevels{1}, .arrayLayers{1}, .sampleCount{0}
    };
    auto depthTexture = rhi->RHICreateTexture(depthTexInfo);

    FISIR::ColorEntry colorEntry{ {.loadOp = FISIR::RenderTargetLoadAction::Clear, .storeOp = FISIR::RenderTargetStoreAction::Store, .dstLayout = FISIR::TextureLayout::ShaderReadOnlyOptimal, .colorType = FISIR::TextureCOLORType::RGBA_8, .sampleCount = 0} };
    FISIR::DepthStencilEntry depthStencilEntry{ .sampleCount = 0, .dstLayout = FISIR::TextureLayout::DepthStencilAttachmentOptimal, .exeit = true };
    depthStencilEntry.depthAction.setDWAndSW(true, true);
    FISIR::SubPassInfo subPassInfo{ .ColorEntryMask = 1, .UseDepthStencil = true, .ReadDepthAsInput = false };
    FISIR::RHIRenderPassInfo renderPassInfo({ {0, colorEntry} }, depthStencilEntry, { subPassInfo });
    auto renderPass = rhi->RHICreateRenderPass(renderPassInfo);
    auto framebuffer = rhi->RHICreateFrameBuffer(WIDTH, HEIGHT, { colorTexture, depthTexture }, renderPassInfo);

    // ── 7. 渲染管线（b0=UniformBuffer, b1=RBuffer 均 Vertex 阶段）────
    FISIR::RHIPipelineDescribeInfo renderDescribe{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage},
        {1, 1, FISIR::RHIDescriptorTyp::RBuffer, FISIR::RHIUsingStage::VertexShaderStage}
    };
    FISIR::RHIVertexInputInfo vertexInputInfo{
        FISIR::RHIBaseDataTYPE::_Fvec3,
        FISIR::RHIBaseDataTYPE::_Fvec3
    };
    FISIR::RHIPipelineState pipelineState{
        .describeInfo = renderDescribe,
        .vertexInfo = vertexInputInfo,
        .topologyType = FISIR::TopologyType::Triangle,
        .rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
        .depthStencilState = { 1, 1, 0, 0.0f, 1.0f, FISIR::_Equal_Less_ },
        .colorblendState = { .UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_) },
        .renderpass = framebuffer->getFrameRenderPass(),
    };
    pipelineState.Shaders[FISIR::__VERTEXSHADER__] = vs;
    pipelineState.Shaders[FISIR::__FRAGMENTSHADER__] = ps;
    auto pipeline = rhi->RHICreatePipeline(pipelineState);

    // ── 8. 计算管线（b0=UniformBuffer, b1=RWBuffer）────────────────
    FISIR::RHIPipelineDescribeInfo computeDescribe{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
        {1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage}
    };
    FISIR::RHIPipelineState computeState{
        .describeInfo = computeDescribe,
        .isComputePipeline = 1,
    };
    computeState.Shaders[FISIR::__COMPUTESHADER__] = cs;
    auto computePipeline = rhi->RHICreatePipeline(computeState);

    // ── 9. 交换链呈现 ──
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    auto swapchainPipeline = swapchain->getSwapChainRenderPipeline();
    FISIR::SamplerInfo swapSamplerInfo;
    auto swapSampler = rhi->RHICreateSampler(swapSamplerInfo);
    auto swapchainPack = rhi->RHICreateResourcePack({ colorTexture, swapSampler });

    // ── 10. 共享缓冲 / 围栏 / 清空值 ──
    // 帧常量与仿真参数缓冲内容随 count 变化（相机/盒体/数量），故构造为空，逐组 updateBufferData。
    FISIR::BufferInfo ubInfo{
        .data_CPU = nullptr,
        .size = sizeof(FrameUB),
        .stride = sizeof(FrameUB),
        .bufferlayout = FISIR::UniformBuffer,
        .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
    };
    auto frameUBBuffer = rhi->RHICreateBuffer(ubInfo);

    FISIR::BufferInfo simInfo{
        .data_CPU = nullptr,
        .size = sizeof(SimParams),
        .stride = sizeof(SimParams),
        .bufferlayout = FISIR::UniformBuffer,
        .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
    };
    auto simParamsBuffer = rhi->RHICreateBuffer(simInfo);


    // compute→render 跨队列信号量：compute 写完 bunnyState 后 signal，render 读取前 wait。
    // 每槽一枚、与 swapchain 槽位一一对应，避免「上一帧 render 仍在 wait 时、下一帧 compute 已 signal」的
    // 跨队列族的 device→device 可见性必须靠信号量建立。
    FISIR::RHISemaphore* computeDoneSem[SLOT_COUNT];
    FISIR::RHIFence* computeFence[SLOT_COUNT];
    for (uint32_t s = 0; s < SLOT_COUNT; ++s) {
        computeFence[s] = rhi->RHICreateFence(false, (std::string("computeFence_") + std::to_string(s)).c_str());
        computeDoneSem[s] = rhi->RHICreateSemaphore((std::string("computeDoneSem_") + std::to_string(s)).c_str());
    }

    FISIR::ClearValue clearFrame{ .ColorClear = 1, .colorinfo = {0.05f, 0.06f, 0.09f, 1.0f}, .depthclearval = 1.0f };
    FISIR::ClearValue clearPresent{ .ColorClear = 1, .colorinfo = {0.0f, 0.0f, 0.0f, 1.0f}, .DepthStencilClear = 0 };

    // ── 11. 逐 count 的测试组 / 主循环 ──
    // 本 RHI 的 RHIDestroyBuffer / RHIDestroyResourcePack 是「立即 delete」，若在仍有帧在飞、
    // 资源线程仍遍历命令缓冲 QuoteResources（其中缓存了 bunnyState 指针）时销毁，会触发
    // Release 下的 use-after-free 崩溃。故逐组只创建、不销毁，统一累积到结尾（程序退出前）
    // 一次性销毁 —— 与普通模式的退出清理时序一致，避免中途销毁与异步 RHI 线程竞争。
    std::vector<FISIR::RHIBuffer*> allBunnyState;
    std::vector<FISIR::RHIResourcePackResult> allRenderPacks, allComputePacks;

    MSG msg = { 0 };
    bool quit = false;
    for (uint32_t numBunnies : bunnyCounts) {
        Info("BunnyPBR: {} bunnies", numBunnies);

        // ── count 相关资源：兔子状态缓冲（RWBuffer|RBuffer，5 槽 ping-pong）──
        float boxHalf = std::max(2.5f, std::pow((float)numBunnies, 1.0f / 3.0f) * 1.2f);
        std::vector<GPUBunny> initialBunnies;
        InitBunnies(initialBunnies, numBunnies, boxHalf, mesh.obbHalfExtents);

        FISIR::BufferInfo stateInfo{
            .data_CPU = initialBunnies.data(),
            .size = sizeof(GPUBunny) * numBunnies,
            .stride = sizeof(GPUBunny),
            .bufferlayout = FISIR::RBuffer | FISIR::RWBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
            .concurrentSharing = true
        };
        FISIR::RHIBuffer* bunnyState[SLOT_COUNT];
        for (uint32_t s = 0; s < SLOT_COUNT; ++s) bunnyState[s] = rhi->RHICreateBuffer(stateInfo);
        

        // 仿真参数（compute b0）—— boxHalf / numBunnies 随 count 变化
        SimParams simParams{
            .dt = 1.0f / 60.0f,
            .boxHalf = boxHalf,
            .restitution = 0.8f,
            .pad0 = 0.0f,
            .numBunnies = numBunnies,
            .pad1 = 0, .pad2 = 0, .pad3 = 0
        };
        simParamsBuffer->updateBufferData(&simParams, sizeof(SimParams));

        // 静态相机 + 光照 —— 相机距离 / 盒体随 count 变化
        float camDist = boxHalf * 2.8f + 0.5f;
        glm::mat4 proj = glm::perspective(glm::radians(50.0f), (float)WIDTH / (float)HEIGHT, 0.1f, 100.0f);
        glm::mat4 view = glm::lookAt(glm::vec3(0.0f, boxHalf * 0.7f, camDist), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        FrameUB frameUB{
            .viewProj = proj * view,
            .cameraPos = glm::vec4(0.0f, boxHalf * 0.7f, camDist, 0.0f),
            .lightDir = glm::normalize(glm::vec4(0.5f, -1.0f, 0.3f, 0.0f)),
            .lightColor = glm::vec4(3.0f, 2.9f, 2.6f, 0.0f),
            .pointLightPos = glm::vec4(boxHalf, boxHalf, 0.0f, 0.0f),
            .pointLightColor = glm::vec4(30.0f, 20.0f, 15.0f, 0.0f)
        };
        frameUBBuffer->updateBufferData(&frameUB, sizeof(FrameUB));

        // 资源包（每槽位一组，绑定顺序 = 资源顺序）
        FISIR::RHIResourcePackResult renderPacks[SLOT_COUNT];
        FISIR::RHIResourcePackResult computePacks[SLOT_COUNT];
        for (uint32_t s = 0; s < SLOT_COUNT; ++s) {
            renderPacks[s]  = rhi->RHICreateResourcePack({ frameUBBuffer, bunnyState[s] });
            computePacks[s] = rhi->RHICreateResourcePack({ simParamsBuffer, bunnyState[s] });
            allRenderPacks.push_back(renderPacks[s]);
            allComputePacks.push_back(computePacks[s]);
        }

        uint32_t groupCountX = (numBunnies + 63u) / 64u;
        Info("dispatch = ({}, 1, 1)", groupCountX);

        // ── 主循环 ──
        uint64_t frameCount = 0;
        std::vector<double> frameTimes;
        if (runPerfTest) {
            frameTimes.reserve(static_cast<size_t>(testFrameCount));
            Info("Performance Test Mode: {} bunnies, target {} frames", numBunnies, testFrameCount);
        }
        auto testStart = std::chrono::steady_clock::now();
        auto fpsStart = testStart;
        bool FrameAppare[SLOT_COUNT] = {0};
        while (true) {
            auto now = std::chrono::steady_clock::now();

            bool gotQuit = false;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { gotQuit = true; break; }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (gotQuit) { quit = true; break; }

            uint32_t infoid = swapchain->acquireGetImageInfoID();
            if (infoid == FISIR::RHISwapChain::FAILEID) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // 把上一帧状态（prevSlot）拷到本帧槽位，使多槽 ping-pong 的仿真状态连续传递，
            // 否则每槽各自独立演化，渲染在多条分叉轨迹间切换 → 兔子看起来像「影分身」各跑各的。
            uint32_t prevSlot = (infoid + SLOT_COUNT - 1) % SLOT_COUNT;
            bunnyState[infoid]->updateBufferData(bunnyState[prevSlot]->getBufferData(), sizeof(GPUBunny) * numBunnies);
            // ── 仿真（compute 写入 bunnyState[infoid]）────────────
            {
                FISIR::RHIComputeCommandList cmdList(rhi);
                cmdList.SetPipelineState(computePipeline);
                cmdList.SetResourcePack(computePacks[infoid]);
                cmdList.dispatch(groupCountX, 1, 1);
                // signal computeDoneSem[infoid]：建立 compute 写 → render 读的跨队列内存依赖。
                cmdList.End(computeFence[infoid], {}, {computeDoneSem[infoid]});
                computeFence[infoid]->wait();      // 等 compute 完成
                computeFence[infoid]->reset();
            }

            // ── 渲染（离屏 PBR → 呈现）───────────────────────────
            auto info = swapchain->getSwapChainGetImageInfo(infoid);

            FISIR::RHIRenderCommandList cmdList(rhi);

            cmdList.BeginRenderPass(framebuffer, 0, clearFrame);
            cmdList.SetPipelineState(pipeline);
            cmdList.SetVertexBuffer(vertexBuffer, 0, 0);
            cmdList.SetIndexBuffer(indexBuffer, 0);
            cmdList.SetResourcePack(renderPacks[infoid]);
            cmdList.SetViewPort(0, 0, WIDTH, HEIGHT, 1.0f, 0.0f);
            cmdList.SetScissor(WIDTH, HEIGHT);
            cmdList.DrawIndex(0, (uint32_t)mesh.indices.size(), 0, numBunnies);
            cmdList.EndRenderPass();

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

            std::vector<FISIR::RHISemaphore*> renderWaits{ info.avaliable, computeDoneSem[infoid] };
            std::vector<FISIR::RHISemaphore*> renderSignals{ info.renderFinish };
            cmdList.End(info.finishFence, renderWaits, renderSignals);
            info.finishFence->waitFenceSubmited();
            swapchain->present(infoid);

            // FPS 标题
            if (++frameCount % 100 == 0) {
                auto fpsNow = std::chrono::steady_clock::now();
                float elapsed = std::chrono::duration<float>(fpsNow - fpsStart).count();
                float fps = 100.0f / elapsed;
                fpsStart = fpsNow;
                char title[128];
                snprintf(title, sizeof(title), "Bunny PBR + OBB Collision | %u bunnies | %.1f FPS", numBunnies, fps);
                SetWindowTextA(hwnd, title);
            }

            // 性能采样：记录本轮 CPU 帧耗时
            if (runPerfTest) {
                auto frameEnd = std::chrono::steady_clock::now();
                frameTimes.push_back(std::chrono::duration<double, std::milli>(frameEnd - now).count());
                if (frameTimes.size() >= testFrameCount) break;
            }
        }

        // ── 性能报告导出（按 count 分组）──
        if (runPerfTest) {
            double totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - testStart).count();
            // 剔除预热帧：前 N 帧包含着色器/管线首帧编译与缓存未命中，不具代表性
            size_t warmup = static_cast<size_t>(std::min<uint64_t>(warmupFrameCount, frameTimes.size()));
            std::vector<double> measured(frameTimes.begin() + warmup, frameTimes.end());

            FISIR::PerfConfig cfg;
            cfg.bunnyCount        = numBunnies;
            cfg.trianglesPerBunny = static_cast<uint32_t>(mesh.indices.size() / 3);
            cfg.dispatchGroupX    = groupCountX;
            cfg.offscreenWidth    = WIDTH;
            cfg.offscreenHeight   = HEIGHT;
            cfg.swapchainWidth    = viewport->getViewportWidth();
            cfg.swapchainHeight   = viewport->getViewportHeight();
            cfg.presentMode       = "Mailbox";
            cfg.renderPassesPerFrame = 2;
            cfg.totalFrames       = measured.size();
            cfg.totalSeconds      = totalSeconds;
            cfg.warmupFrames      = warmup;

            std::string mdPath  = "PerfReport_C" + std::to_string(numBunnies) + ".md";
            std::string csvPath = "PerfFrameTimes_C" + std::to_string(numBunnies) + ".csv";
            FISIR::writePerformanceReport(cfg, measured, mdPath, csvPath);
            Info("Performance report exported: {} / {}", mdPath, csvPath);
        }
        memset(FrameAppare, 0, 5*sizeof(bool));
        for (auto fence : computeFence) rhi->RHIDestroyFence(fence);
        if (quit) break;
    }

cleanup:
    // ── 12. 清理（资源一律经 RHI 接口销毁，避免跨模块 new/delete 不匹配）──
    // 累积的逐组资源在此统一销毁（packs → 状态缓冲）。
    for (auto& p : allRenderPacks)  if (p.ResourcePack || p.SamplerPack) rhi->RHIDestroyResourcePack(p);
    for (auto& p : allComputePacks) if (p.ResourcePack || p.SamplerPack) rhi->RHIDestroyResourcePack(p);
    for (auto* b : allBunnyState)   if (b) rhi->RHIDestroyBuffer(b);

    if (swapchainPack.ResourcePack || swapchainPack.SamplerPack) rhi->RHIDestroyResourcePack(swapchainPack);
    if (swapSampler) rhi->RHIDestroySampler(swapSampler);
    for (uint32_t s = 0; s < SLOT_COUNT; ++s)
        if (computeDoneSem[s]) rhi->RHIDestroySemaphore(computeDoneSem[s]);
    if (simParamsBuffer) rhi->RHIDestroyBuffer(simParamsBuffer);
    if (frameUBBuffer) rhi->RHIDestroyBuffer(frameUBBuffer);
    if (vertexBuffer) rhi->RHIDestroyBuffer(vertexBuffer);
    if (indexBuffer) rhi->RHIDestroyBuffer(indexBuffer);
    if (framebuffer) rhi->RHIDestroyFrameBuffer(framebuffer);
    if (colorTexture) rhi->RHIDestroyTexture(colorTexture);
    if (depthTexture) rhi->RHIDestroyTexture(depthTexture);
    Info("BunnyPBR cleanup done.");
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    return 0;
}
