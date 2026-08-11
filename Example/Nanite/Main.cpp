#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "Log/Logger.h"
#include "RHICommandList.h"
#include "RHICreator.h"
#include "RHIFence.h"
#include "RHIFrameBuffer.h"
#include "RHIPipeline.h"
#include "RHIResourcePack.h"
#include "RHISampler.h"
#include "RHISwapChain.h"
#include "RHITypes.h"
#include "ShaderComplier.h"

// -------------------------------------------------------------------
// FPS camera state (shared with WindowProc via file scope)
// -------------------------------------------------------------------
struct FPSCamera {
    float posX = 7.0f, posY = 5.0f, posZ = 0.0f;
    float yaw   = -1.5708f;  // atan2(-7,0) — looking toward origin from +X
    float pitch = -0.6199f;  // asin(-5/√74) — looking down ~36°
    bool  locked = false;
    float rawYaw   = 0.0f;   // accumulated from WM_INPUT this frame
    float rawPitch = 0.0f;
    float moveFwd = 0, moveRight = 0, moveUp = 0; // WASD / QE state
    bool  resetRequested = false;
};
static FPSCamera g_Cam;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {

    case WM_DESTROY:
        PostQuitMessage(0); return 0;

    case WM_KEYDOWN: {
        if (wParam == 'L') {
            g_Cam.locked = !g_Cam.locked;
            if (g_Cam.locked) {
                ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = {r.right/2, r.bottom/2};
                ClientToScreen(hwnd, &c);
                SetCursorPos(c.x, c.y);
            } else {
                ShowCursor(TRUE);
                g_Cam.rawYaw = 0; g_Cam.rawPitch = 0;
            }
        }
        if (wParam == 'R') {
            g_Cam.resetRequested = true;
        }
        return 0;
    }

    case WM_KILLFOCUS:
        if (g_Cam.locked) {
            g_Cam.locked = false;
            ShowCursor(TRUE);
        }
        return 0;

    case WM_INPUT: {
        if (!g_Cam.locked) return DefWindowProc(hwnd, uMsg, wParam, lParam);
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        if (size > 0) {
            RAWINPUT raw;
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) == size &&
                raw.header.dwType == RIM_TYPEMOUSE) {
                g_Cam.rawYaw   += (float)raw.data.mouse.lLastX;
                g_Cam.rawPitch += (float)raw.data.mouse.lLastY;
            }
        }
        return 0;
    }

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// GPU-side indirect draw command (matches VkDrawIndexedIndirectCommand)
struct alignas(4) GPU_DrawIndexedIndirectCommand {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t firstInstance;
};

// =====================================================================
//  Frustum-Culled GPU-Driven Indirect Draw
//
//  Per frame:
//   1. CPU computes camera matrices + updates UBOs.
//   2. Compute shader: frustum-culls N instances, atomically compacts
//      visible instances into a dense buffer, writes the draw count.
//   3. CPU fence-waits for compute completion (required by RHI page
//      concurrency — compute & render vkQueueSubmit order is
//      non-deterministic without CPU-side sequencing).
//   4. Single DrawIndexedIndirect draws only the visible instances.
// =====================================================================
int main() {
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
#endif

    Info("============================================");
    Info("|  Nanite -- Frustum-Culled Indirect Draw |");
    Info("|  GPU decides visibility per frame       |");
    Info("============================================");

    // ---- 1. Create RHI -------------------------------------------------
    FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);
    FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();

    // ---- 2. Create window ----------------------------------------------
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const char CLASS_NAME[] = "NaniteIndirectDraw";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, CLASS_NAME,
                                "Nanite -- Frustum-Culled Indirect Draw",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1024, 768, NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);

    // Register raw mouse input for FPS camera
    RAWINPUTDEVICE rid = {0x01, 0x02, 0, hwnd}; // HID mouse
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    struct Win32Data { HINSTANCE hinstance; HWND hwnd; } win32Data{hInstance, hwnd};
    auto viewport = rhi->RHICreateViewport(1024, 768,
        FISIR::TextureCOLORType::RGBA_8, (void*)&win32Data);
    rhi->Init();

    // ---- 3. Test parameters --------------------------------------------
    const uint32_t NUM_INSTANCES   = 65536; // 256x256 grid on the XZ plane
    const uint32_t GRID_SIZE       = 256;
    const float    SPACING         = 0.08f;
    const uint32_t INDIRECT_STRIDE = sizeof(GPU_DrawIndexedIndirectCommand);
    const uint32_t INSTANCE_STRIDE = 32;  // 2 x float4 per instance

    // ---- 4. Vertex + index buffers (one quad) --------------------------
    struct Vertex { float pos[3]; float color[3]; };
    Vertex quadVerts[] = {
        {{-1.f, -1.f, 0.f}, {1,1,1}},
        {{ 1.f, -1.f, 0.f}, {1,1,1}},
        {{-1.f,  1.f, 0.f}, {1,1,1}},
        {{ 1.f,  1.f, 0.f}, {1,1,1}},
    };
    FISIR::BufferInfo vboInfo{
        .data_CPU = quadVerts, .size = sizeof(quadVerts),
        .bufferlayout = FISIR::VertexBuffer | FISIR::TransferSrcBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostVisable),
    };
    auto vbo = rhi->RHICreateBuffer(vboInfo);

    uint32_t quadIndices[] = {0, 1, 2, 1, 3, 2};
    FISIR::BufferInfo iboInfo{
        .data_CPU = quadIndices, .size = sizeof(quadIndices),
        .stride = sizeof(uint32_t),
        .bufferlayout = FISIR::IndexBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    auto ibo = rhi->RHICreateBuffer(iboInfo);

    // ---- 5. Indirect draw buffer (GPU writes instanceCount each frame) ---
    GPU_DrawIndexedIndirectCommand initCmd = {6, 0, 0, 0, 0};
    FISIR::BufferInfo indirectInfo{
        .data_CPU = &initCmd, .size = INDIRECT_STRIDE,
        .bufferlayout = FISIR::StorageBuffer | FISIR::IndirectBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    auto indirectBuf = rhi->RHICreateBuffer(indirectInfo);

    // ---- 6. Compacted instance buffer (worst case: all visible) ---------
    std::vector<float> zeroInstances(NUM_INSTANCES * 8, 0.f);
    FISIR::BufferInfo instBufInfo{
        .data_CPU = zeroInstances.data(), .size = zeroInstances.size() * sizeof(float),
        .stride = INSTANCE_STRIDE,
        .bufferlayout = FISIR::StorageBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    auto instanceBuf = rhi->RHICreateBuffer(instBufInfo);

    // ---- 7. Uniform buffers --------------------------------------------
    // CS UBO: viewProj + camera + grid params
    struct alignas(16) CullUBO {
        float viewProj[16];
        float cameraPos[4];
        uint32_t totalInstances;
        uint32_t gridSize;
        float    spacing;
        float    pad[3];
    };
    CullUBO cullData{};
    FISIR::BufferInfo cullUBOInfo{
        .data_CPU = &cullData, .size = sizeof(CullUBO),
        .bufferlayout = FISIR::UniformBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    auto cullUBO = rhi->RHICreateBuffer(cullUBOInfo);

    // VS UBO: viewProj + time
    struct alignas(16) SceneUBO {
        float viewProj[16];
        float time;
        float pad[3];
    };
    SceneUBO sceneData{};
    FISIR::BufferInfo sceneUBOInfo{
        .data_CPU = &sceneData, .size = sizeof(SceneUBO),
        .bufferlayout = FISIR::UniformBuffer,
        .memoryType = FISIR::MemType(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
    };
    auto sceneUBO = rhi->RHICreateBuffer(sceneUBOInfo);

    Info("Buffers created.  NUM_INSTANCES = {},  GRID = {}x{}",
         NUM_INSTANCES, GRID_SIZE, GRID_SIZE);

    // ---- 8. Shaders ----------------------------------------------------

    // CS-entry-1: reset indirect draw command (sets instanceCount to 0)
    const wchar_t* csResetCode = LR"(
        struct DrawCmd {
            uint indexCount;
            uint instanceCount;
            uint firstIndex;
            int  vertexOffset;
            uint firstInstance;
        };
        RWStructuredBuffer<DrawCmd> indirectBuf : register(u1);

        [numthreads(1, 1, 1)]
        void main() {
            DrawCmd cmd;
            cmd.indexCount    = 6;
            cmd.instanceCount = 0;
            cmd.firstIndex    = 0;
            cmd.vertexOffset  = 0;
            cmd.firstInstance = 0;
            indirectBuf[0] = cmd;
        }
    )";
    FISIR::ShaderComplier* csResetCompiler = new FISIR::ShaderComplier();
    csResetCompiler->compileShader(csResetCode, wcslen(csResetCode) * sizeof(wchar_t),
                                   L"main", L"cs_6_0");
    auto csResetShader = rhi->RHICreateShader(FISIR::__COMPUTESHADER__,
        csResetCompiler->getShaderData(), csResetCompiler->getShaderDataSize());
    delete csResetCompiler;

    // CS-entry-2: frustum cull + 3D-terrain atomic compact
    const wchar_t* csCullCode = LR"(
        struct DrawCmd {
            uint indexCount;
            uint instanceCount;
            uint firstIndex;
            int  vertexOffset;
            uint firstInstance;
        };

        cbuffer CullParams : register(b0) {
            float4x4 viewProj;
            float4   cameraPos;
            uint     totalInstances;
            uint     gridSize;
            float    spacing;
            float3   pad_cb;
        };

        RWStructuredBuffer<DrawCmd>  indirectBuf : register(u1);
        RWStructuredBuffer<float4>   instanceBuf : register(u2);

        // Hue [0,1] → RGB (saturated + full value)
        float3 hueToRgb(float h) {
            float x = h * 6.0;
            return float3(
                saturate(abs(x - 3.0) - 1.0),
                saturate(2.0 - abs(x - 2.0)),
                saturate(2.0 - abs(x - 4.0))
            );
        }

        [numthreads(64, 1, 1)]
        void main(uint3 tid : SV_DispatchThreadID) {
            uint i = tid.x;
            if (i >= totalInstances) return;

            uint row = i / gridSize;
            uint col = i % gridSize;
            float cx = (float(col) - float(gridSize - 1u) * 0.5) * spacing;
            float cz = (float(row) - float(gridSize - 1u) * 0.5) * spacing;

            // Multi-octave terrain height — rolling neon hills
            float y = sin(cx * 0.5) * cos(cz * 0.6) * 3.0
                    + sin(cx * 0.15 + cz * 0.2) * 5.0
                    + cos(cx * 0.3) * sin(cz * 0.35) * 2.0;

            float3 worldPos = float3(cx, y, cz);

            // Frustum test in clip space
            float4 clip = mul(viewProj, float4(worldPos, 1.0));
            float3 ndc  = clip.xyz / clip.w;
            bool inside  = clip.w > 0.0 &&
                           all(abs(ndc) <= float3(1.001, 1.001, 1.001));

            if (!inside) return;

            // Quad scale driven by local surface steepness
            float steepness = abs(cos(cx * 0.5) * cos(cz * 0.6) * 1.5
                                + cos(cx * 0.15 + cz * 0.2) * 0.75
                                - sin(cx * 0.3) * sin(cz * 0.35) * 0.6);
            float scale = 0.04 + steepness * 0.04;

            // Neon colour: hue cycles with height + position
            float hue = frac(y * 0.08 + (cx + cz) * 0.015);
            float3 rgb = hueToRgb(hue);

            uint slot;
            InterlockedAdd(indirectBuf[0].instanceCount, 1, slot);
            instanceBuf[slot * 2 + 0] = float4(cx, y, cz, scale);
            instanceBuf[slot * 2 + 1] = float4(rgb, 1.0);
        }
    )";
    FISIR::ShaderComplier* csCullCompiler = new FISIR::ShaderComplier();
    csCullCompiler->compileShader(csCullCode, wcslen(csCullCode) * sizeof(wchar_t),
                                  L"main", L"cs_6_0");
    auto csCullShader = rhi->RHICreateShader(FISIR::__COMPUTESHADER__,
        csCullCompiler->getShaderData(), csCullCompiler->getShaderDataSize());
    delete csCullCompiler;

    // VS: reads instance buffer (pos+color) + UBO (viewProj), animated quads
    const wchar_t* vsCode = LR"(
        cbuffer Scene : register(b0) {
            float4x4 viewProj;
            float    time;
            float3   pad_vs;
        };
        StructuredBuffer<float4> instanceBuf : register(t1);

        struct VSInput  { float3 pos : POSITION;  float3 color : COLOR; };
        struct PSInput  { float4 pos : SV_POSITION; float4 color : COLOR; };

        PSInput main(VSInput input, uint instanceID : SV_InstanceID) {
            float4 pos_scale = instanceBuf[instanceID * 2 + 0];
            float4 clr       = instanceBuf[instanceID * 2 + 1];

            // spin + breathe animation
            float s    = pos_scale.w * (1.0 + 0.2 * sin(time * 2.0 + pos_scale.x * 0.5 + pos_scale.z * 0.7));
            float rot  = time * 0.6;
            float cosA = cos(rot);
            float sinA = sin(rot);

            float3 localRot;
            localRot.x = input.pos.x * cosA - input.pos.y * sinA;
            localRot.y = input.pos.x * sinA + input.pos.y * cosA;
            localRot.z = 0.0;
            float3 worldPos = localRot * s + pos_scale.xyz;

            PSInput output;
            output.pos   = mul(viewProj, float4(worldPos, 1.0));
            output.color = clr;
            return output;
        }
    )";
    FISIR::ShaderComplier* vsCompiler = new FISIR::ShaderComplier();
    vsCompiler->compileShader(vsCode, wcslen(vsCode) * sizeof(wchar_t), L"main", L"vs_6_0");
    auto vsShader = rhi->RHICreateShader(FISIR::__VERTEXSHADER__,
        vsCompiler->getShaderData(), vsCompiler->getShaderDataSize());
    delete vsCompiler;

    const wchar_t* psCode = LR"(
        struct PSInput { float4 pos : SV_POSITION; float4 color : COLOR; };
        float4 main(PSInput input) : SV_TARGET { return input.color; }
    )";
    FISIR::ShaderComplier* psCompiler = new FISIR::ShaderComplier();
    psCompiler->compileShader(psCode, wcslen(psCode) * sizeof(wchar_t), L"main", L"ps_6_0");
    auto psShader = rhi->RHICreateShader(FISIR::__FRAGMENTSHADER__,
        psCompiler->getShaderData(), psCompiler->getShaderDataSize());
    delete psCompiler;

    Info("All 4 shaders compiled (CS-reset, CS-cull, VS, PS).");

    // ---- 8.5. Pre-fill indirect + instance buffers (CPU) --------------------
    // Matches CS cull shader formulas so first frame starts with all visible.
    // Multi-octave terrain heights, HSL colours, steepness-driven quad scales.
    {
        GPU_DrawIndexedIndirectCommand cpuCmd = {6, NUM_INSTANCES, 0, 0, 0};
        indirectBuf->updateBufferData(&cpuCmd, sizeof(cpuCmd));

        auto hueToRgb = [](float h) -> std::tuple<float,float,float> {
            float r = std::max(0.0f, std::min(1.0f, std::abs(h * 6.0f - 3.0f) - 1.0f));
            float g = std::max(0.0f, std::min(1.0f, 2.0f - std::abs(h * 6.0f - 2.0f)));
            float b = std::max(0.0f, std::min(1.0f, 2.0f - std::abs(h * 6.0f - 4.0f)));
            return {r, g, b};
        };

        std::vector<float> cpuInst(NUM_INSTANCES * 8);
        for (uint32_t i = 0; i < NUM_INSTANCES; ++i) {
            uint32_t row = i / GRID_SIZE;
            uint32_t col = i % GRID_SIZE;
            float cx = (float(col) - float(GRID_SIZE - 1) * 0.5f) * SPACING;
            float cz = (float(row) - float(GRID_SIZE - 1) * 0.5f) * SPACING;

            float y = std::sin(cx * 0.5f) * std::cos(cz * 0.6f) * 3.0f
                    + std::sin(cx * 0.15f + cz * 0.2f) * 5.0f
                    + std::cos(cx * 0.3f) * std::sin(cz * 0.35f) * 2.0f;

            float steepness = std::abs(std::cos(cx * 0.5f) * std::cos(cz * 0.6f) * 1.5f
                                     + std::cos(cx * 0.15f + cz * 0.2f) * 0.75f
                                     - std::sin(cx * 0.3f) * std::sin(cz * 0.35f) * 0.6f);
            float scale = 0.04f + steepness * 0.04f;

            float hRaw = y * 0.08f + (cx + cz) * 0.015f;
            float hue = hRaw - std::floor(hRaw);  // match HLSL frac()
            auto [r, g, b] = hueToRgb(hue);

            cpuInst[i * 8 + 0] = cx;
            cpuInst[i * 8 + 1] = y;
            cpuInst[i * 8 + 2] = cz;
            cpuInst[i * 8 + 3] = scale;
            cpuInst[i * 8 + 4] = r;
            cpuInst[i * 8 + 5] = g;
            cpuInst[i * 8 + 6] = b;
            cpuInst[i * 8 + 7] = 1.0f;
        }
        instanceBuf->updateBufferData(cpuInst.data(), cpuInst.size() * sizeof(float));
    }
    Info("Pre-filled {} instances into indirect + instance buffers.", NUM_INSTANCES);

    // ---- 9. Compute pipelines (reset + cull) ---------------------------
    FISIR::RHIPipelineDescribeInfo csDescribeInfo{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
        {1, 1, FISIR::RHIDescriptorTyp::StorageBuffer,  FISIR::RHIUsingStage::ComputeShaderStage},
        {2, 1, FISIR::RHIDescriptorTyp::StorageBuffer,  FISIR::RHIUsingStage::ComputeShaderStage},
    };
    FISIR::RHIPipelineState csResetState{
        .describeInfo = csDescribeInfo, .isComputePipeline = true,
    };
    csResetState.Shaders[FISIR::__COMPUTESHADER__] = csResetShader;
    auto csResetPipeline = rhi->RHICreatePipeline(csResetState);

    FISIR::RHIPipelineState csCullState{
        .describeInfo = csDescribeInfo, .isComputePipeline = true,
    };
    csCullState.Shaders[FISIR::__COMPUTESHADER__] = csCullShader;
    auto csCullPipeline = rhi->RHICreatePipeline(csCullState);

    // ---- 10. Graphics pipeline -----------------------------------------
    FISIR::RHIPipelineDescribeInfo gfxDescribeInfo{
        {0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage},
        {1, 1, FISIR::RHIDescriptorTyp::StorageBuffer,  FISIR::RHIUsingStage::VertexShaderStage},
    };
    FISIR::RHIVertexInputInfo vertexInputInfo{
        FISIR::RHIBaseDataTYPE::_Fvec3,
        FISIR::RHIBaseDataTYPE::_Fvec3,
    };

    FISIR::TextureInfo colorTexInfo{
        .size = {768, 1024, 1},
        .colorType{FISIR::TextureCOLORType::RGBA_8},
        .type{FISIR::TextureType::TEXTURE2D},
        .useFor{FISIR::TextureUseForColorAttachment | FISIR::TextureUseForTransferDst
                | FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForInputAttachment},
        .mipLevels{1}, .arrayLayers{1}, .sampleCount{1}
    };
    auto colorTexture = rhi->RHICreateTexture(colorTexInfo);

    FISIR::TextureInfo depthTexInfo{
        .size = {768, 1024, 1},
        .colorType{FISIR::TextureCOLORType::Depth24_Stencil8},
        .type{FISIR::TextureType::TEXTURE2D},
        .useFor{FISIR::TextureUseForDepthStencilAttachment},
        .mipLevels{1}, .arrayLayers{1}, .sampleCount{1}
    };
    auto depthTexture = rhi->RHICreateTexture(depthTexInfo);

    FISIR::ColorEntry colorEntry{{.loadOp = FISIR::RenderTargetLoadAction::Clear,
        .storeOp = FISIR::RenderTargetStoreAction::Store,
        .dstLayout = FISIR::TextureLayout::ShaderReadOnlyOptimal,
        .colorType = FISIR::TextureCOLORType::RGBA_8, .sampleCount = 1}};
    FISIR::DepthStencilEntry depthEntry{.sampleCount = 1,
        .dstLayout = FISIR::TextureLayout::DepthStencilAttachmentOptimal, .exeit = true};
    depthEntry.depthAction.setDWAndSW(true, true);
    FISIR::SubPassInfo subPassInfo{.ColorEntryMask = 1, .UseDepthStencil = true,
                                    .ReadDepthAsInput = false};
    FISIR::RHIRenderPassInfo rpInfo({{0, colorEntry}}, depthEntry, {subPassInfo});
    auto framebuffer = rhi->RHICreateFrameBuffer(1024, 768,
        {colorTexture, depthTexture}, rpInfo);

    FISIR::RHIPipelineState gfxState{
        .describeInfo = gfxDescribeInfo,
        .vertexInfo = vertexInputInfo,
        .topologyType = FISIR::TopologyType::Triangle,
        .rasterizationState = {false, false, false, FISIR::PolygonMode::Fill,
                               FISIR::FrontFace::CW, FISIR::CullMode::None},
        .depthStencilState = {true, true, false, 0.f, 1.f, FISIR::_Equal_Less_},
        .colorblendState = {.UsingColorBit = FISIR::ColorBit(
            FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_)},
        .renderpass = framebuffer->getFrameRenderPass(),
    };
    gfxState.Shaders[FISIR::__VERTEXSHADER__]   = vsShader;
    gfxState.Shaders[FISIR::__FRAGMENTSHADER__] = psShader;
    auto gfxPipeline = rhi->RHICreatePipeline(gfxState);

    // ---- 11. Resource packs --------------------------------------------
    auto csPack  = rhi->RHICreateResourcePack(
        std::vector<FISIR::RHIResource*>{cullUBO, indirectBuf, instanceBuf});
    auto gfxPack = rhi->RHICreateResourcePack(
        std::vector<FISIR::RHIResource*>{sceneUBO, instanceBuf});

    // ---- 12. Swapchain setup -------------------------------------------
    auto swapchain = rhi->RHIGetSwapChain(viewport);
    auto swapPipeline = swapchain->getSwapChainRenderPipeline();
    FISIR::SamplerInfo sampInfo;
    auto blitSampler = rhi->RHICreateSampler(sampInfo);
    auto blitPack = rhi->RHICreateResourcePack(
        std::vector<FISIR::RHIResource*>{colorTexture, blitSampler});

    FISIR::ClearValue clearOffscreen{.ColorClear = 1,
        .colorinfo = {0.02f, 0.02f, 0.05f, 1.f}, .depthclearval = 1.f};
    FISIR::ClearValue clearPresent{.ColorClear = 1,
        .colorinfo = {0.f, 0.f, 0.f, 1.f}, .DepthStencilClear = 0};

    // ---- 13. Helper: build perspective + view matrices ------------------
    auto buildViewProj = [](float* dst, const float eye[3], const float center[3],
                            float aspect, float fovY) {
        float up[3] = {0, 1, 0};

        float fwd[3] = {center[0]-eye[0], center[1]-eye[1], center[2]-eye[2]};
        float len = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        fwd[0]/=len; fwd[1]/=len; fwd[2]/=len;

        float right[3] = {up[1]*fwd[2] - up[2]*fwd[1],
                          up[2]*fwd[0] - up[0]*fwd[2],
                          up[0]*fwd[1] - up[1]*fwd[0]};
        len = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
        right[0]/=len; right[1]/=len; right[2]/=len;

        float realUp[3] = {fwd[1]*right[2] - fwd[2]*right[1],
                           fwd[2]*right[0] - fwd[0]*right[2],
                           fwd[0]*right[1] - fwd[1]*right[0]};

        float dotEyeRight = -(eye[0]*right[0] + eye[1]*right[1] + eye[2]*right[2]);
        float dotEyeUp    = -(eye[0]*realUp[0] + eye[1]*realUp[1] + eye[2]*realUp[2]);
        float dotEyeFwd   = -(eye[0]*fwd[0] + eye[1]*fwd[1] + eye[2]*fwd[2]);

        // Perspective projection (Vulkan NDC: Y-down, Z=[0,1], w>0)
        float h = 1.0f / tanf(fovY * 0.5f);
        float w = h / aspect;
        float n = 0.01f, f = 200.0f;

        float proj[16] = {
            w, 0, 0, 0,
            0,-h, 0, 0,
            0, 0, f/(f-n), -f*n/(f-n),
            0, 0, 1, 0,
        };

        float view[16] = {
            right[0], realUp[0], fwd[0], 0,
            right[1], realUp[1], fwd[1], 0,
            right[2], realUp[2], fwd[2], 0,
            dotEyeRight, dotEyeUp, dotEyeFwd, 1,
        };

        // viewProj = proj * view (row-major)
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += proj[r*4 + k] * view[k*4 + c];
                dst[r*4 + c] = sum;
            }
        }
    };

    // ---- 14. FPS camera + dynamic main loop ------------------------------
    uint32_t csGroups = (NUM_INSTANCES + 63) / 64;
    auto startTime = std::chrono::steady_clock::now();
    auto prevTime = startTime;
    Info("Entering dynamic loop -- GPU frustum-culls {} instances per frame.",
         NUM_INSTANCES);
    Info("Controls: WASD=move  QE=up/down  L=lock mouse  R=reset camera");

    // Transpose row-major C++ matrix → column-major for HLSL float4x4
    auto Transpose4x4 = [](float* m) {
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
                std::swap(m[i * 4 + j], m[j * 4 + i]);
    };

    const float MOUSE_SENS = 0.002f;
    const float MOVE_SPEED = 10.0f;
    const float FOVY       = 1.0f;          // ~57°
    const float ASPECT     = 1024.0f / 768.0f;

    uint32_t gpuVisible = NUM_INSTANCES; // updated each frame after CS fence

    MSG msg = {0};
    uint64_t frameCount = 0;
    auto fpsStart = std::chrono::steady_clock::now();
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // ---- Delta time ---------------------------------------------------
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp after breakpoints / long pauses

        // ---- R key: reset camera ------------------------------------------
        if (g_Cam.resetRequested) {
            g_Cam.posX = 7.0f; g_Cam.posY = 5.0f; g_Cam.posZ = 0.0f;
            g_Cam.yaw = -1.5708f; g_Cam.pitch = -0.6199f;
            g_Cam.resetRequested = false;
        }

        // ---- Mouse look ---------------------------------------------------
        if (g_Cam.locked) {
            g_Cam.yaw   -= g_Cam.rawYaw   * MOUSE_SENS;
            g_Cam.pitch -= g_Cam.rawPitch * MOUSE_SENS;
            const float MAX_PITCH = 1.5f;
            if (g_Cam.pitch >  MAX_PITCH) g_Cam.pitch =  MAX_PITCH;
            if (g_Cam.pitch < -MAX_PITCH) g_Cam.pitch = -MAX_PITCH;
            g_Cam.rawYaw = 0; g_Cam.rawPitch = 0;
        }

        // ---- WASD / QE movement -------------------------------------------
        {
            auto keyDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
            float fwd = 0, rgt = 0, up = 0;
            if (keyDown('W')) fwd += 1;
            if (keyDown('S')) fwd -= 1;
            if (keyDown('D')) rgt += 1;
            if (keyDown('A')) rgt -= 1;
            if (keyDown('E')) up  += 1;
            if (keyDown('Q')) up  -= 1;
            float len = sqrtf(fwd*fwd + rgt*rgt);
            if (len > 0.001f) { fwd /= len; rgt /= len; }

            float cosP  = cosf(g_Cam.pitch);
            float fwdX  = cosP * sinf(g_Cam.yaw);
            float fwdY  = sinf(g_Cam.pitch);
            float fwdZ  = cosP * cosf(g_Cam.yaw);
            float rLen  = sqrtf(fwdX*fwdX + fwdZ*fwdZ);
            float rgtX  = 0, rgtZ = 0;
            if (rLen > 0.0001f) { rgtX = fwdZ / rLen; rgtZ = -fwdX / rLen; }

            float speed = MOVE_SPEED * dt;
            g_Cam.posX += (fwdX * fwd + rgtX * rgt) * speed;
            g_Cam.posY += (fwdY * fwd + up) * speed;
            g_Cam.posZ += (fwdZ * fwd + rgtZ * rgt) * speed;
        }

        float t = std::chrono::duration<float>(now - startTime).count();

        // ---- FPS counter (update title every 100 frames) -------------------
        ++frameCount;
        if (frameCount % 100 == 0) {
            float elapsed = std::chrono::duration<float>(now - fpsStart).count();
            float fps = 100.0f / elapsed;
            fpsStart = now;
            char title[128];
            snprintf(title, sizeof(title),
                "Nanite GPU Culling | %u/%d visible | %.1f FPS%s",
                gpuVisible, NUM_INSTANCES, fps,
                g_Cam.locked ? " [LOCKED]" : "");
            SetWindowTextA(hwnd, title);
        }

        // ---- Build camera vectors for this frame ---------------------------
        float cosP  = cosf(g_Cam.pitch);
        float fwdX  = cosP * sinf(g_Cam.yaw);
        float fwdY  = sinf(g_Cam.pitch);
        float fwdZ  = cosP * cosf(g_Cam.yaw);
        float eye[3]    = {g_Cam.posX, g_Cam.posY, g_Cam.posZ};
        float center[3] = {eye[0] + fwdX, eye[1] + fwdY, eye[2] + fwdZ};

        // ---- Acquire swapchain image ----
        uint32_t frameID = swapchain->acquireGetImageInfoID();
        if (frameID == FISIR::RHISwapChain::FAILEID) continue;

        // ---- Compute: GPU frustum cull + compact (reset + cull) ------------
        {
            buildViewProj(cullData.viewProj, eye, center, ASPECT, FOVY);
            Transpose4x4(cullData.viewProj);
            cullData.cameraPos[0] = g_Cam.posX;
            cullData.cameraPos[1] = g_Cam.posY;
            cullData.cameraPos[2] = g_Cam.posZ;
            cullData.totalInstances = NUM_INSTANCES;
            cullData.gridSize       = GRID_SIZE;
            cullData.spacing        = SPACING;
            cullUBO->updateBufferData(&cullData, sizeof(CullUBO));

            auto cmdCS = FISIR::RHIComputeCommandList(rhi);
            cmdCS.SetResourcePack(csPack);

            // Dispatch 1: reset instanceCount to 0
            cmdCS.SetPipelineState(csResetPipeline);
            cmdCS.dispatch(1, 1, 1);

            // Dispatch 2: frustum cull, atomic compact
            cmdCS.SetPipelineState(csCullPipeline);
            cmdCS.dispatch(csGroups, 1, 1);

            auto fence = rhi->RHICreateFence();
            cmdCS.End(fence);
            fence->wait();   // required: RHI threads compute & render pages concurrently
            rhi->RHIDestroyFence(fence);

            // Read GPU-computed visible instance count from indirect buffer
            auto* indirectCmd = (GPU_DrawIndexedIndirectCommand*)
                indirectBuf->getBufferData();
            gpuVisible = indirectCmd->instanceCount;
        }

        // ---- Render: single DrawIndexedIndirect (GPU-decided visibility) ---
        buildViewProj(sceneData.viewProj, eye, center, ASPECT, FOVY);
        Transpose4x4(sceneData.viewProj);
        sceneData.time = t;
        sceneUBO->updateBufferData(&sceneData, sizeof(SceneUBO));

        FISIR::RHIRenderCommandList cmdList(rhi);

        // Barrier: compute writes → vertex / indirect reads
        FISIR::RHIBuffer* toTransition[] = {indirectBuf, instanceBuf};
        cmdList.TransitionBuffers(toTransition, 2,
            FISIR::ResourceAccess::ShaderWriteOnly, FISIR::ResourceAccess::ShaderReadOnly,
            FISIR::RHIUsingStage::ComputeShaderStage, FISIR::RHIUsingStage::VertexShaderStage);

        // Off-screen -- single DrawIndexedIndirect (GPU-decided draw count)
        cmdList.BeginRenderPass(framebuffer, 0, clearOffscreen);
        cmdList.SetPipelineState(gfxPipeline);
        cmdList.SetResourcePack(gfxPack);
        cmdList.SetVertexBuffer(vbo, 0, 0);
        cmdList.SetIndexBuffer(ibo, 0);
        cmdList.SetViewPort(0, 0, 1024, 768, 1, 0);
        cmdList.SetScissor(1024, 768);
        cmdList.DrawIndexedIndirect(indirectBuf, 0, 1, INDIRECT_STRIDE);
        cmdList.EndRenderPass();

        // Blit to swapchain
        auto info = swapchain->getSwapChainGetImageInfo(frameID);
        auto swFrameBuf = swapchain->getSwapChainFrameBuffer(info.imageIndex);
        if (swFrameBuf) {
            cmdList.BeginRenderPass(swFrameBuf, 0, clearPresent);
            cmdList.SetPipelineState(swapPipeline);
            cmdList.SetResourcePack(blitPack);
            cmdList.SetViewPort(0, 0, viewport->getViewportWidth(),
                                viewport->getViewportHeight(), 1, 0);
            cmdList.SetScissor(viewport->getViewportWidth(), viewport->getViewportHeight());
            cmdList.DrawPrimitive(0, 3, 1);
            cmdList.EndRenderPass();
        }
        cmdList.End(info.finishFence, {info.avaliable}, {info.renderFinish},
                    swapchain, frameID);
    }

cleanup:
    Info("Shutting down...");
    rhi->RHIDestroyResourcePack(blitPack);
    rhi->RHIDestroySampler(blitSampler);
    rhi->RHIDestroyResourcePack(csPack);
    rhi->RHIDestroyResourcePack(gfxPack);
    rhi->RHIDestroyBuffer(sceneUBO);
    rhi->RHIDestroyBuffer(cullUBO);
    rhi->RHIDestroyBuffer(instanceBuf);
    rhi->RHIDestroyBuffer(indirectBuf);
    rhi->RHIDestroyBuffer(ibo);
    rhi->RHIDestroyBuffer(vbo);
    rhi->RHIDestroyFrameBuffer(framebuffer);
    rhi->RHIDestroyTexture(depthTexture);
    rhi->RHIDestroyTexture(colorTexture);
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    Info("=== Frustum-Culled Test -- Done ===");
    return 0;
}
