// BunnyPBR —— 斯坦福兔子导入 + PBR + 多物体 OBB 碰撞样例
//
// 用法：BunnyPBR.exe [-C X] [-Test] [-Frames N] [-Warmup N] [-Model path.ply] [-Slots N] [-VSync]
//                     [-Shot N] [-SunAz deg] [-SunEl deg]
//   -C X      : 兔子数量（可多次指定，每个 -C 生成一组测试；默认 10）
//   -Test     : 性能测试模式，跑完 -Frames 帧后导出报告并退出
//   -Frames N : 测试帧数（默认 5000）
//   -Warmup N : 预热帧数（默认 60）
//   -Shot N   : 在第 N 帧从**离屏颜色纹理**读回一帧并存成 Screenshot_N.bmp，随后退出。
//               走 CopyImageToBuffer + 围栏等待，不依赖交换链取图/呈现，
//               因此在无人值守（窗口被遮挡、无交互桌面）时同样能得到画面结果。
//   -SunAz D  : 太阳方位角（度，从 -Z 轴转向 -X 轴为正；默认 59.0）
//   -SunEl D  : 太阳仰角（度；默认 59.7）。天空烘焙与方向光共用同一个太阳方向，
//               把仰角压到几度即可把太阳拍进画面，用于目视校验两者是否一致。
//   -Model P  : 显式指定模型；缺省按 Res/bunny_hi.ply（高采样原始扫描件）→
//               Res/bunny.ply（降采样）→ 程序化球体 的顺序回退
//   -Slots N  : 交换链槽位数（帧在飞数，默认 3）；交换链会夹取到「≤ 图像数」，
//               实际生效值以 swapchain->getSlotCount() 为准
//   -VSync    : 开启垂直同步（swapchain->sync(true)：FIFO 呈现模式 + 重建交换链）
//   - 模型：ASCII / binary_little_endian（PLY 属性表驱动，多余顶点字段自动跳过）
//   - 渲染：GGX metallic-roughness PBR + **天空盒背景 + IBL**（SH9 漫反射 + 预滤波镜面），
//           per-instance 数据经 StructuredBuffer + gl_InstanceIndex
//   - 环境：程序化天空在 CPU 上一次性烘焙成两张立方体贴图（天空盒源 / GGX 预滤波链）
//           与一组 SH9 系数，详见 SkyIBL.h
//   - 仿真：全在 compute shader（积分 + OBB SAT 冲量 + 脚本自旋 + 撞墙），SLOT_COUNT 槽位环形缓冲 ping-pong
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
#include "SkyIBL.h"

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

// 将 RGBA8 像素缓冲写为 24-bit BMP（BGR、自底向上）—— 与 TextureCube 的截图写法一致。
// 读回缓冲的行序已与 BMP 的 bottom-up 存储匹配，按序写入即可，不要再翻转。
static void WriteBMP(const char* path, uint32_t width, uint32_t height, const unsigned char* rgba) {
    const uint32_t rowSize = (width * 3 + 3) & ~3u;      // 每行按 4 字节对齐
    const uint32_t dataSize = rowSize * height;
    unsigned char header[54] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    *(uint32_t*)(header + 2)  = 54 + dataSize;           // 文件总大小
    *(uint32_t*)(header + 10) = 54;                      // 像素数据偏移
    *(uint32_t*)(header + 14) = 40;                      // BITMAPINFOHEADER 大小
    *(int32_t*)(header + 18)  = (int32_t)width;
    *(int32_t*)(header + 22)  = (int32_t)height;
    *(uint16_t*)(header + 26) = 1;                       // 颜色平面数
    *(uint16_t*)(header + 28) = 24;                      // 每像素位数
    *(uint32_t*)(header + 34) = dataSize;                // 像素数据大小

    FILE* f = fopen(path, "wb");
    if (!f) { Error("Failed to open screenshot file '{}'", path); return; }
    fwrite(header, 1, 54, f);

    std::vector<unsigned char> row(rowSize, 0);
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

// ── 着色器编译封装：加载 .slang → DXC 编译 → RHICreateShader ────────
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

// ── GPU 侧兔子结构（与 BunnyPBR.slang / Simulation.slang 的 Bunny 严格对齐，96 字节）──
struct alignas(16) GPUBunny {
    float position[4];      // xyz + pad
    float orientation[4];   // 四元数 xyzw
    float velocity[4];      // xyz 线速度 + w 角速度
    float spinAxis[4];      // xyz + pad
    float halfExtents[4];   // OBB 半边长 xyz + pad
    float color[4];         // rgb 反照率 + w 粗糙度
};
static_assert(sizeof(GPUBunny) == 96, "GPUBunny must be 96 bytes");

// ── 渲染帧常量（b0，静态相机 + 静态光照 → 每帧不变）────────────────
// 与 BunnyPBR.slang / Skybox.slang 的 cbuffer FrameUB 逐字段一致（368 字节）。
// IBL 数据（IBLParams + SH9）也放在这里：环境是场景级常量，随 count 分组一起重传即可，
// 不值得为它单开一个 binding 与一套资源包。
struct alignas(16) FrameUB {
    glm::mat4 viewProj;         // 64  世界 → 裁剪
    glm::mat4 invViewProj;      // 64  天空盒：裁剪 → 世界
    glm::vec4 cameraPos;        // 16
    glm::vec4 lightDir;         // 16（光线行进方向；太阳在它的反向）
    glm::vec4 lightColor;       // 16
    glm::vec4 pointLightPos;    // 16
    glm::vec4 pointLightColor;  // 16
    glm::vec4 iblParams;        // 16 x=环境强度 y=预滤波最大 mip z=镜面 IBL 强度 w=天空盒亮度
    glm::vec4 sh[9];            // 144 环境辐射亮度 SH9（原始系数，余弦卷积在着色器里做）
};  // 368 字节
static_assert(sizeof(FrameUB) == 368, "FrameUB must match the HLSL cbuffer layout");

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
        // OBB 半边长（w 字段是原 pad，现承载金属度，见下方）
        b.halfExtents[0] = halfExtents[0];
        b.halfExtents[1] = halfExtents[1];
        b.halfExtents[2] = halfExtents[2];
        // 颜色 / 粗糙度 / 金属度：约 4 成金属 —— 金属的 F0 = 反照率且漫反射为 0，
        // 没有环境光照时会全黑，正是 IBL 最有说服力的展示对象，故金属粗糙度给低值
        // 以便看到清晰的环境反射；其余为电介质，粗糙度范围更宽。
        // 金属度写进 halfExtents.w：它是 Simulation.slang 完全不碰的 pad 字段，
        // 复用它既不用改结构体尺寸（仍 96 字节），也不必让仿真多传一份材质数据。
        glm::vec3 c = hsvToRgb(rnd(rng), 0.7f, 0.9f);
        const bool metallic = rnd(rng) < 0.4f;
        b.color[0] = c.r; b.color[1] = c.g; b.color[2] = c.b;
        b.color[3] = metallic ? (0.05f + rnd(rng) * 0.35f) : (0.30f + rnd(rng) * 0.65f);
        b.halfExtents[3] = metallic ? (0.85f + rnd(rng) * 0.15f) : 0.0f;
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
    // -Slots N   : 交换链槽位数（帧在飞数），默认 3；交换链会夹取到「≤ 图像数」
    // -VSync     : 开启垂直同步（sync(true) → FIFO 呈现模式并重建交换链）
    bool runPerfTest = false;
    uint64_t testFrameCount = 5000;
    uint64_t warmupFrameCount = 60;
    std::vector<uint32_t> bunnyCounts;
    std::string modelPath;          // -Model 显式指定；空 = 走默认候选表
    uint32_t swapChainSlots = 10;
    bool requestVSync = false;
    bool     shotRequested = false;   // -Shot N：第 N 帧读回离屏画面并退出
    uint64_t shotFrame = 30;
    bool  sunAngleOverride = false;   // -SunAz/-SunEl：覆盖太阳方位角/仰角（度）
    float sunAzimuthDeg = 59.0f;
    float sunElevationDeg = 59.7f;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-Test" || arg == "-test" || arg == "/Test") {
            runPerfTest = true;
        } else if ((arg == "-SunAz" || arg == "-sunaz") && i + 1 < argc) {
            sunAzimuthDeg = (float)std::atof(argv[++i]);
            sunAngleOverride = true;
        } else if ((arg == "-SunEl" || arg == "-sunel") && i + 1 < argc) {
            sunElevationDeg = (float)std::atof(argv[++i]);
            sunAngleOverride = true;
        } else if ((arg == "-Shot" || arg == "-shot") && i + 1 < argc) {
            shotRequested = true;
            shotFrame = std::stoull(argv[++i]);
        } else if ((arg == "-Frames" || arg == "-frames") && i + 1 < argc) {
            testFrameCount = std::stoull(argv[++i]);
        } else if ((arg == "-Warmup" || arg == "-warmup") && i + 1 < argc) {
            warmupFrameCount = std::stoull(argv[++i]);
        } else if ((arg == "-Model" || arg == "-model") && i + 1 < argc) {
            modelPath = argv[++i];
        } else if ((arg == "-Slots" || arg == "-slots") && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            swapChainSlots = (v > 0) ? (uint32_t)v : FISIR::DEFAULT_SWAPCHAIN_SLOT_COUNT;
        } else if (arg == "-VSync" || arg == "-vsync" || arg == "-Sync") {
            requestVSync = true;
        } else if ((arg == "-C" || arg == "-c" || arg == "-Count" || arg == "-count") && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            if (v < 1) v = 1;
            if (v > 4096) { Warn("bunny count {} exceeds the 4096 cap, clamped", v); v = 4096; }
            bunnyCounts.push_back((uint32_t)v);
        }
    }
    if (bunnyCounts.empty()) bunnyCounts.push_back(10);

    const uint32_t WIDTH = 1280, HEIGHT = 720;

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

    // 呈现设备句柄布局见 RHIDisplay.h：Win32Window = Win32DisplayHandle{ hinstance, hwnd }
    FISIR::Win32DisplayHandle win32Data{ hInstance, hwnd };
    // 槽位数（帧在飞数）由用户指定，经 viewport 传给交换链；交换链会夹取到「≤ 图像数」。
    auto viewport = rhi->RHICreateViewport(WIDTH, HEIGHT, FISIR::TextureCOLORType::RGBA_8,
        FISIR::DisplayDeviceType::Win32Window, (void*)&win32Data, swapChainSlots);
    rhi->Init();

    // ── 3. 加载网格（PLY → uint32 索引 + 加权法线 + OBB 半边长）─────
    // 候选表：高采样原始扫描件优先。Res/bunny_hi.ply = Stanford 官方
    // bun_zipper.ply（35947 顶点 / 69451 面，仓库 README 里的 "high resolution
    // result"，只带 x,y,z + confidence + intensity），Res/bunny.ply 为其
    // 降采样版（8171 顶点）——两级都缺失才退化为程序化球体。
    std::vector<std::string> modelCandidates;
    if (!modelPath.empty()) {
        modelCandidates.push_back(modelPath);
    } else {
        //modelCandidates.push_back("Res/bunny_hi.ply");
        modelCandidates.push_back("Res/bunny.ply");
    }

    FISIR::MeshData mesh;
    std::string meshSource = "(fallback sphere)";
    auto meshLoadStart = std::chrono::steady_clock::now();
    for (const std::string& candidate : modelCandidates) {
        mesh = FISIR::LoadMesh(candidate.c_str(), 0.8f);
        if (mesh.loaded) { meshSource = candidate; break; }
    }
    double meshLoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - meshLoadStart).count();
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        Error("mesh is empty, cannot continue");
        return 1;
    }
    Info("Mesh: {} ({} vertices, {} triangles, halfExtents = ({:.3f}, {:.3f}, {:.3f}), load {:.2f} ms)",
         meshSource, mesh.vertices.size(), mesh.indices.size() / 3,
         mesh.obbHalfExtents[0], mesh.obbHalfExtents[1], mesh.obbHalfExtents[2], meshLoadMs);

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
    auto vs = CompileShader(rhi, "Shader/BunnyPBR.slang", FISIR::__VERTEXSHADER__, "mainVS", "vs_5_0");
    auto ps = CompileShader(rhi, "Shader/BunnyPBR.slang", FISIR::__FRAGMENTSHADER__, "mainPS", "ps_5_0");
    auto cs = CompileShader(rhi, "Shader/Simulation.slang", FISIR::__COMPUTESHADER__, "mainCS", "cs_6_0");
    if (!vs || !ps || !cs) { Error("shader compilation failed"); return 1; }

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

    // ── 7. 程序化天空 + IBL 烘焙（CPU，一次性）──────────────────────
    // 太阳方向与直接光照共用同一个向量：LightDir 是光线**行进**方向，太阳在它的反向。
    // 这样「天空盒里的太阳位置」与「方向光打出来的高光方向」天然一致 ——
    // 两者若各写一份，天空里的太阳和地物上的高光就会对不上，是这类改动最常见的裂缝。
    // 默认仰角 ≈ 59.7°、方位角 ≈ 59°（左前上方），与改造前的固定光照方向一致。
    // -SunAz / -SunEl 可以把它压到地平线附近，用于目视校验「天空太阳 ↔ 方向光」是否一致。
    glm::vec3 sunDir = glm::normalize(glm::vec3(-0.4319f, 0.8638f, -0.2592f));
    if (sunAngleOverride) {
        const float el = glm::radians(sunElevationDeg);
        const float az = glm::radians(sunAzimuthDeg);
        sunDir = glm::normalize(glm::vec3(-std::sin(az) * std::cos(el), std::sin(el), -std::cos(az) * std::cos(el)));
    }
    const glm::vec3 lightTravelDir = -sunDir;

    FISIR::SkyIBL::SkyParams skyParams;
    skyParams.sunDir = sunDir;

    auto bakeStart = std::chrono::steady_clock::now();
    FISIR::SkyIBL::EnvironmentBake environment = FISIR::SkyIBL::BakeEnvironment(skyParams);
    const double bakeMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - bakeStart).count();
    Info("Environment baked in {:.1f} ms: skybox {}^2 x6, prefiltered {}^2 x6 x{} mips, SH9 irradiance, sun=({:.3f}, {:.3f}, {:.3f}), radianceScale={:.2f}",
         bakeMs, environment.env.size, environment.prefiltered.size, environment.prefiltered.mips,
         sunDir.x, sunDir.y, sunDir.z, environment.radianceScale);

    // 立方体贴图 = TextureType::TEXTUREARRAY + arrayLayers == 6：
    // Vulkan 后端据此带上 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT，并把 image view 建成
    // VK_IMAGE_VIEW_TYPE_CUBE（见 VulkanTexture.cpp / VulkanImageView.cpp）。
    auto createCubeTexture = [&](const FISIR::SkyIBL::CubeImage& img) -> FISIR::RHITexture* {
        FISIR::TextureInfo info{
            .size = {img.size, img.size, 1},   // TextureSize 字段序为 {height, width, depth}
            .colorType{FISIR::TextureCOLORType::RGBA_16},
            .type{FISIR::TextureType::TEXTUREARRAY},
            .useFor{FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForTransferDst},
            .mipLevels{(uint16_t)img.mips},
            .arrayLayers{6},
            .sampleCount{0}
        };
        return rhi->RHICreateTexture(info);
    };

    auto envCube        = createCubeTexture(environment.env);
    auto prefilteredCube = createCubeTexture(environment.prefiltered);

    // 暂存缓冲不在这里销毁：fence->wait() 只保证 GPU 完成，资源线程可能仍在遍历
    // 本页的 QuoteResources（缓存了这些缓冲的指针）。与本文件其余资源一致，
    // 统一累积到退出前销毁，避免与异步 RHI 线程竞争。
    std::vector<FISIR::RHIBuffer*> allStagingBuffers;

    // 上传：每个 mip 一次 CopyToTexture，arrayindex=0 + arraycount=6 ——
    // CubeImage 的 mip 块本来就是「6 个面连续排布」，正好对上紧密打包的 VkBufferImageCopy。
    auto uploadCube = [&](FISIR::RHITexture* tex, const FISIR::SkyIBL::CubeImage& img) {
        const size_t bytes = img.texels.size() * sizeof(uint16_t);
        FISIR::BufferInfo stageInfo{
            .data_CPU = nullptr,
            .size = bytes,
            .stride = 0,
            .bufferlayout = FISIR::TransferSrcBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
        };
        FISIR::RHIBuffer* stage = rhi->RHICreateBuffer(stageInfo);
        stage->updateBufferData((void*)img.texels.data(), bytes);
        allStagingBuffers.push_back(stage);

        // 用 Render 命令列表（graphics 队列）而不是 Transfer 命令列表：
        // 纹理是 EXCLUSIVE 且创建在 graphics 队列族上，之后也在 graphics 队列被采样，
        // 同队列族内部转移即可，不需要跨队列族的所有权 release/acquire。
        FISIR::RHIRenderCommandList uploadCmd(rhi);
        FISIR::RHITexture* textures[] = { tex };
        uploadCmd.TransitionTextures(textures, 1,
            FISIR::ResourceAccess::Undefined, FISIR::ResourceAccess::TransferDst,
            FISIR::TextureLayout::Undefined, FISIR::TextureLayout::TransferDstOptimal,
            FISIR::RHIUsingStage::NoneStage, FISIR::RHIUsingStage::PipelineTransferStage);

        for (uint32_t m = 0; m < img.mips; ++m) {
            const uint32_t s = img.mipSize(m);
            const uint64_t srcOffset = (uint64_t)img.mipOffset[m] * sizeof(uint16_t);
            uploadCmd.CopyToTexture(stage, tex, m, 0, 6, srcOffset, {0, 0, 0}, {s, s, 1});
        }

        uploadCmd.TransitionTextures(textures, 1,
            FISIR::ResourceAccess::TransferDst, FISIR::ResourceAccess::ShaderReadOnly,
            FISIR::TextureLayout::TransferDstOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
            FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);

        FISIR::RHIFence* fence = rhi->RHICreateFence();
        uploadCmd.End(fence);
        fence->wait();
        rhi->RHIDestroyFence(fence);
    };

    uploadCube(envCube, environment.env);
    uploadCube(prefilteredCube, environment.prefiltered);

    // 立方体贴图采样器：CLAMP 包裹（立方体贴图语义），minLod=0 且 maxLod=最高 mip ——
    // 后者必须显式给出，否则采样器的 LOD 上界会把「按粗糙度选 mip」压回第 0 级，
    // 粗糙度反射会全部退化成镜面。
    FISIR::SamplerInfo cubeSamplerInfo{
        .enlagerFilter = FISIR::SamplerFilter::LINEAR,
        .minFilter = FISIR::SamplerFilter::LINEAR,
        .mipMapMode = FISIR::SamplerFilter::LINEAR,
        .u = FISIR::SamplerOverFoundMode::CLAMP_TO_EDGE,
        .v = FISIR::SamplerOverFoundMode::CLAMP_TO_EDGE,
        .w = FISIR::SamplerOverFoundMode::CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = false,
        .maxAnisotropy = 1.0f,
        .compareEnable = false,
        .compareOP = FISIR::APIOperation::_NONE_OP_,
        .maxLop = (float)(environment.prefiltered.mips - 1),
        .minLop = 0.0f,
        .unNormalized = false
    };
    auto cubeSampler = rhi->RHICreateSampler(cubeSamplerInfo);

    // IBL 参数：
    //   x = 辐射亮度还原系数（= 烘焙时除掉的天空峰值）。归一化的环境贴图与 SH 都乘回它，
    //       天空盒与地物因此走同一条曝光曲线，不会出现「背景和物体对不上」。
    //   y = 预滤波最大 mip（粗糙度 → LOD 的映射上界）
    //   z = 镜面 IBL 微调
    //   w = 天空盒亮度（在 x 之上再给背景一个独立倍率，正常为 1）
    const glm::vec4 iblParams{
        environment.radianceScale,
        (float)(environment.prefiltered.mips - 1),
        1.0f,
        1.0f
    };

    // ── 8. 渲染管线（b0=UniformBuffer, b1=StructuredBuffer, t2=预滤波环境, s0=立方体采样器）──
    // 三个 binding 都要在**片元阶段**可见：b0 的相机与光照、t2/s0 的 IBL 都在 PS 里读。
    // （原先只声明 VertexShaderStage，靠 heap 路径不校验 stageFlags 才没暴露；
    //   降级到传统 DescriptorSet 时那种声明与 SPIR-V 的 stage 不匹配是会报错的。）
    FISIR::RHIPipelineDescribeInfo renderDescribe{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ALLStage},
        {1, 1, FISIR::RHIDescriptorTyp::RBuffer, FISIR::RHIUsingStage::ALLStage},
        {2, 1, FISIR::RHIDescriptorTyp::SamplerImage, FISIR::RHIUsingStage::FragmentShaderStage},
        {0, 1, FISIR::RHIDescriptorTyp::Sampler, FISIR::RHIUsingStage::FragmentShaderStage}
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

    // ── 9. 天空盒管线 ──────────────────────────────────────────────
    // 与地物共用同一个 render pass / framebuffer（深度附件就是那个用于深度测试的附件），
    // 因此不需要第二条 render pass，也不涉及多余的 layout 转换。
    auto skyVS = CompileShader(rhi, "Shader/Skybox.slang", FISIR::__VERTEXSHADER__, "mainVS", "vs_5_0");
    auto skyPS = CompileShader(rhi, "Shader/Skybox.slang", FISIR::__FRAGMENTSHADER__, "mainPS", "ps_5_0");
    if (!skyVS || !skyPS) { Error("skybox shader compilation failed"); return 1; }

    FISIR::RHIPipelineDescribeInfo skyDescribe{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ALLStage},
        {2, 1, FISIR::RHIDescriptorTyp::SamplerImage, FISIR::RHIUsingStage::FragmentShaderStage},
        {0, 1, FISIR::RHIDescriptorTyp::Sampler, FISIR::RHIUsingStage::FragmentShaderStage}
    };
    FISIR::RHIPipelineState skyPipelineState{
        .describeInfo = skyDescribe,
        // 全屏三角形靠 SV_VertexID 生成，没有顶点缓冲、也就没有顶点属性
        .vertexInfo = FISIR::RHIVertexInputInfo{},
        .topologyType = FISIR::TopologyType::Triangle,
        .rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
        // 深度测试开、深度写关：z=1.0 且 LESS_EQUAL，正好只在「没有不透明几何」的像素通过
        .depthStencilState = { 1, 0, 0, 0.0f, 1.0f, FISIR::_Equal_Less_ },
        .colorblendState = { .UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_) },
        .renderpass = framebuffer->getFrameRenderPass(),
    };
    skyPipelineState.Shaders[FISIR::__VERTEXSHADER__] = skyVS;
    skyPipelineState.Shaders[FISIR::__FRAGMENTSHADER__] = skyPS;
    auto skyPipeline = rhi->RHICreatePipeline(skyPipelineState);

    // ── 10. 计算管线（b0=UniformBuffer, b1=RWBuffer）────────────────
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

    // ── 11. 交换链呈现 ──
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    auto swapchainPipeline = swapchain->getSwapChainRenderPipeline();
    FISIR::SamplerInfo swapSamplerInfo;
    auto swapSampler = rhi->RHICreateSampler(swapSamplerInfo);
    // 呈现资源包改由 swapchain 管理：登记离屏纹理（纹理模式，BufferEnable=0，PS 采样纹理）。
    swapchain->enableTextureInput(colorTexture, swapSampler);

    // 垂直同步：sync(true) → FIFO 呈现模式（重建交换链，下一帧 acquire 生效）。
    if (requestVSync) swapchain->sync(true);

    // 槽位数以交换链的**有效值**为准（用户请求值可能被夹取到 ≤ 图像数），
    // 下面所有「每槽一份」的资源都按它分配。
    const uint32_t SLOT_COUNT = swapchain->getSlotCount();
    Info("SwapChain: slots={} images={} vsync={}", SLOT_COUNT, swapchain->getImageCount(), swapchain->isSyncEnabled() ? "on" : "off");

    // ── 12. 共享缓冲 / 围栏 / 清空值 ──
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

    // 天空盒资源包：只需帧常量 + 天空盒环境图 + 立方体采样器，与槽位无关，建一份即可。
    // 资源顺序 [UBO, SamplerImage] 对应 skyDescribe 的 binding 0 / 2。
    FISIR::RHIResourcePackResult skyPack =
        rhi->RHICreateResourcePack({ frameUBBuffer, envCube, cubeSampler });


    // compute→render 跨队列信号量：compute 写完 bunnyState 后 signal，render 读取前 wait。
    // 每槽一枚、与 swapchain 槽位一一对应，避免「上一帧 render 仍在 wait 时、下一帧 compute 已 signal」的
    // 跨队列族的 device→device 可见性必须靠信号量建立。
    // 槽位数运行期可变（-Slots / 交换链夹取），故每槽资源一律用 vector，不再用定长数组。
    std::vector<FISIR::RHISemaphore*> computeDoneSem(SLOT_COUNT, nullptr);
    for (uint32_t s = 0; s < SLOT_COUNT; ++s) {
        computeDoneSem[s] = rhi->RHICreateSemaphore((std::string("computeDoneSem_") + std::to_string(s)).c_str(), FISIR::FenceType::TimeLine);

    }

    FISIR::ClearValue clearFrame{ .ColorClear = 1, .colorinfo = {0.05f, 0.06f, 0.09f, 1.0f}, .depthclearval = 1.0f };
    FISIR::ClearValue clearPresent{ .ColorClear = 1, .colorinfo = {0.0f, 0.0f, 0.0f, 1.0f}, .DepthStencilClear = 0 };

    // 截图读回缓冲（host-visible）：一次分配、整轮复用。
    FISIR::RHIBuffer* shotBuffer = nullptr;
    if (shotRequested) {
        FISIR::BufferInfo shotInfo{
            .data_CPU = nullptr,
            .size = (uint64_t)WIDTH * HEIGHT * 4,
            .stride = 0,
            .bufferlayout = FISIR::TransferDstBuffer,
            .memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent)
        };
        shotBuffer = rhi->RHICreateBuffer(shotInfo);
    }

    // ── 13. 逐 count 的测试组 / 主循环 ──
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

        // ── count 相关资源：兔子状态缓冲（RWBuffer|RBuffer，SLOT_COUNT 槽 ping-pong）──
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
        std::vector<FISIR::RHIBuffer*> bunnyState(SLOT_COUNT, nullptr);
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
        glm::mat4 viewProj = proj * view;
        FrameUB frameUB{
            .viewProj = viewProj,
            // 天空盒用：从 NDC 反投影出每个像素的视线方向。
            // 只在相机/盒体变化时重算，故与 viewProj 一起放在逐组常量里。
            .invViewProj = glm::inverse(viewProj),
            .cameraPos = glm::vec4(0.0f, boxHalf * 0.7f, camDist, 0.0f),
            // 与天空烘焙共用同一个太阳方向（lightTravelDir 的反向）
            .lightDir = glm::vec4(lightTravelDir, 0.0f),
            .lightColor = glm::vec4(3.0f, 2.9f, 2.6f, 0.0f),
            .pointLightPos = glm::vec4(boxHalf, boxHalf, 0.0f, 0.0f),
            .pointLightColor = glm::vec4(30.0f, 20.0f, 15.0f, 0.0f),
            .iblParams = iblParams
        };
        for (int i = 0; i < 9; ++i) frameUB.sh[i] = glm::vec4(environment.irradianceSH[i], 0.0f);
        frameUBBuffer->updateBufferData(&frameUB, sizeof(FrameUB));

        // 资源包：绑定顺序必须与 describeInfo 的声明顺序**逐一对应** ——
        // 描述符堆路径把「资源在列表中的次序」映射到「第 i 条 binding 的堆偏移」，
        // 次序错位会让 t2 拿到采样器、s0 拿到纹理（表现为花屏或直接校验报错）。
        std::vector<FISIR::RHIResourcePackResult> renderPacks(SLOT_COUNT), computePacks(SLOT_COUNT);
        for (uint32_t s = 0; s < SLOT_COUNT; ++s) {
            renderPacks[s]  = rhi->RHICreateResourcePack({ frameUBBuffer, bunnyState[s], prefilteredCube, cubeSampler });
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
        // 每槽「是否已出现过一帧」：用 1 字节整型 vector（槽位数运行期可变），
        // 复位用 std::fill —— 不再用 memset，避免尺寸与槽位数漂移写坏栈（曾触发 Run-Time Check #2）。
        std::vector<uint8_t> FrameAppared(SLOT_COUNT, 0);
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
                //if (FrameAppared[infoid]) computeDoneSem[infoid]->wait();
                cmdList.End(nullptr, {}, {computeDoneSem[infoid]});
                FrameAppared[infoid] = 1;
                //computeFence[infoid]->wait();      // 等 compute 完成
                //computeFence[infoid]->reset();
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

            // 天空盒：同一个 render pass 内、不透明几何之后。
            // 深度测试 LESS_EQUAL + 深度写关闭 → 只在深度仍是清屏值 1.0 的像素上着色，
            // 地物覆盖到的像素原样保留（所以这里不需要也不应该再清一次深度）。
            // 全屏三角形用 DrawPrimitive(0, 3, 1)（3 个顶点、1 个实例，无顶点缓冲）。
            cmdList.SetPipelineState(skyPipeline);
            cmdList.SetResourcePack(skyPack);
            cmdList.DrawPrimitive(0, 3, 1);

            cmdList.EndRenderPass();

            // 截图：在离屏 pass 之后、呈现 pass 之前读回颜色纹理。
            // 离屏 render pass 的 finalLayout 是 ShaderReadOnlyOptimal，故这里
            // SRO → TransferSrc（拷贝）→ 再转回 SRO（后面的呈现 pass 还要采样它）。
            const bool isShotFrame = shotRequested && shotBuffer && (frameCount + 1 == shotFrame);
            if (isShotFrame) {
                FISIR::RHITexture* shotTextures[] = { colorTexture };
                cmdList.TransitionTextures(shotTextures, 1,
                    FISIR::ResourceAccess::ShaderReadOnly, FISIR::ResourceAccess::TransferSrc,
                    FISIR::TextureLayout::ShaderReadOnlyOptimal, FISIR::TextureLayout::TransferSrcOptimal,
                    FISIR::RHIUsingStage::FragmentShaderStage, FISIR::RHIUsingStage::PipelineTransferStage);
                cmdList.CopyImageToBuffer(colorTexture, shotBuffer, 0, 0, 1, { 0, 0, 0 }, 0, { HEIGHT, WIDTH, 1 });
                cmdList.TransitionTextures(shotTextures, 1,
                    FISIR::ResourceAccess::TransferSrc, FISIR::ResourceAccess::ShaderReadOnly,
                    FISIR::TextureLayout::TransferSrcOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
                    FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);
            }

            auto frameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);
            if (frameBuf) {
                cmdList.BeginRenderPass(frameBuf, 0, clearPresent);
                cmdList.SetPipelineState(swapchainPipeline);
                cmdList.SetResourcePack(swapchain->getSwapchainResourcePack());
                cmdList.SetViewPort(0, 0, viewport->getViewportWidth(), viewport->getViewportHeight(), 1.0f, 0.0f);
                cmdList.SetScissor(viewport->getViewportWidth(), viewport->getViewportHeight());
                cmdList.DrawPrimitive(0, 3, 1);
                cmdList.EndRenderPass();
            }

            std::vector<FISIR::RHISemaphore*> renderWaits{ info.avaliable, computeDoneSem[infoid] };
            std::vector<FISIR::RHISemaphore*> renderSignals{ info.renderFinish };
            // 呈现录成指令（写在 End 之前），由 RHI 线程在本页提交之后执行 vkQueuePresentKHR。
            cmdList.Present(swapchain, infoid);
            cmdList.End(info.finishFence, renderWaits, renderSignals);
            info.finishFence->waitFenceSubmited();

            // 截图的 GPU 拷贝与提交同页：等这条围栏置位即代表拷贝已落地，可以安全读回缓冲。
            // 用带超时的 waitFor，避免无人值守时死等。
            if (isShotFrame) {
                if (!info.finishFence->waitFor(5ull * 1000 * 1000 * 1000)) {
                    Error("Screenshot readback timed out on frame {}", frameCount + 1);
                } else {
                    char shotPath[64];
                    snprintf(shotPath, sizeof(shotPath), "Screenshot_%llu.bmp", (unsigned long long)(frameCount + 1));
                    WriteBMP(shotPath, WIDTH, HEIGHT,
                             static_cast<const unsigned char*>(shotBuffer->getBufferData()));
                    Info("Screenshot saved: {}", shotPath);
                }
                quit = true;
                break;
            }

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
            cfg.presentMode       = swapchain->isSyncEnabled() ? "FIFO(vsync)" : "MAILBOX/IMMEDIATE";
            cfg.renderPassesPerFrame = 2;
            cfg.totalFrames       = measured.size();
            cfg.totalSeconds      = totalSeconds;
            cfg.warmupFrames      = warmup;

            std::string mdPath  = "PerfReport_C" + std::to_string(numBunnies) + ".md";
            std::string csvPath = "PerfFrameTimes_C" + std::to_string(numBunnies) + ".csv";
            FISIR::writePerformanceReport(cfg, measured, mdPath, csvPath);
            Info("Performance report exported: {} / {}", mdPath, csvPath);
        }
        std::fill(FrameAppared.begin(), FrameAppared.end(), 0);
        if (quit) break;
    }

cleanup:
    // ── 14. 清理（资源一律经 RHI 接口销毁，避免跨模块 new/delete 不匹配）──
    // 累积的逐组资源在此统一销毁（packs → 状态缓冲）。
    for (auto& p : allRenderPacks)  if (p.ResourcePack || p.SamplerPack) rhi->RHIDestroyResourcePack(p);
    for (auto& p : allComputePacks) if (p.ResourcePack || p.SamplerPack) rhi->RHIDestroyResourcePack(p);
    for (auto* b : allBunnyState)   if (b) rhi->RHIDestroyBuffer(b);

    // 天空盒资源包由本函数持有（不属于 swapchain），与上面的 render/compute pack 一同销毁。
    if (skyPack.ResourcePack || skyPack.SamplerPack) rhi->RHIDestroyResourcePack(skyPack);
    // 暂存缓冲：到这一步所有帧都已结束，可以安全释放。
    for (auto* b : allStagingBuffers) if (b) rhi->RHIDestroyBuffer(b);

    // 呈现资源包由 swapchain 拥有，随 destroyRenderInterface 的 swapchain 析构一并销毁，此处不再手动销毁。
    if (swapSampler) rhi->RHIDestroySampler(swapSampler);
    if (cubeSampler) rhi->RHIDestroySampler(cubeSampler);
    for (auto* sem : computeDoneSem)
        if (sem) rhi->RHIDestroySemaphore(sem);
    if (simParamsBuffer) rhi->RHIDestroyBuffer(simParamsBuffer);
    if (frameUBBuffer) rhi->RHIDestroyBuffer(frameUBBuffer);
    if (shotBuffer) rhi->RHIDestroyBuffer(shotBuffer);
    if (envCube) rhi->RHIDestroyTexture(envCube);
    if (prefilteredCube) rhi->RHIDestroyTexture(prefilteredCube);
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
