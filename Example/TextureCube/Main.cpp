#include <iostream>
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
            float4x4 MVP;
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
        PSInput main(VSInput input) {
            PSInput output;
            output.pos = mul(MVP, float4(input.pos, 1.0));
            output.color = input.color;
            output.uv = input.uv;
            return output;
        }
    )";
    FISIR::ShaderComplier* vCompiler = new FISIR::ShaderComplier();
    vCompiler->compileShader(vsCode, wcslen(vsCode) * sizeof(wchar_t), L"main", L"vs_5_0");
    auto vs = rhi->RHICreateShader(FISIR::ShaderTYP::__VERTEXSHADER__, vCompiler->getShaderData(), vCompiler->getShaderDataSize());
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
        float4 main(PSInput input) : SV_TARGET {
            float4 texColor = myTexture.Sample(mySampler, input.uv);
            return texColor;
        }
    )";
    FISIR::ShaderComplier* pCompiler = new FISIR::ShaderComplier();
    pCompiler->compileShader(psCode, wcslen(psCode) * sizeof(wchar_t), L"main", L"ps_5_0");
    auto ps = rhi->RHICreateShader(FISIR::ShaderTYP::__FRAGMENTSHADER__, pCompiler->getShaderData(), pCompiler->getShaderDataSize());
    delete pCompiler;

    // ---------- 5. 离屏渲染目标 ----------
    FISIR::TextureInfo colorTexInfo{
        .size = {1024, 1024, 1},
        .colorType {FISIR::TextureCOLORType::RGBA_8},
        .type {FISIR::TextureType::TEXTURE2D},
        .useFor {FISIR::TextureUseForColorAttachment | FISIR::TextureUseForTransferDst | FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForInputAttachment},
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
        .size = sizeof(glm::mat4),
        .stride = sizeof(glm::mat4),
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
            .bufferlayout = FISIR::StorageBuffer | FISIR::TransferSrcBuffer,
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
        uploadCmdList.End();
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

    // ---------- 12. 清空值 ----------
    FISIR::ClearValue clearFrame{ .ColorClear = 1, .colorinfo = {0.1f, 0.1f, 0.2f, 1.0f}, .depthclearval = 1.0f };
    FISIR::ClearValue clearPresent{ .ColorClear = 1, .colorinfo = {0.0f, 0.0f, 0.0f, 1.0f}, .DepthStencilClear = 0 };

    // ---------- 13. 主循环 ----------
    float angle = 0.0f;
    float aspect = 1024.0f / 1024.0f;
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 50.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.0f, 3.0f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));

    MSG msg = { 0 };
    Warn("Begin Main Loop");
    while (true) {

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        //Warn("Main Running");
        // 更新 MVP
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
        glm::mat4 mvp = proj * view * model;
        
        //Info("MTag0");

        uint32_t infoid = swapchain->acquireGetImageInfoID();
        if (infoid == FISIR::RHISwapChain::FAILEID) continue;
        static_cast<FISIR::RHIBuffer*>(mvpBuffers[infoid])->updateBufferData(&mvp, sizeof(glm::mat4));
        if (infoid == FISIR::RHISwapChain::FAILEID) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Error("Get Failed!");
            continue;
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
        cmdList.DrawIndex(0, 36, 0, 1);
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
        cmdList.End(info.finishFence, { info.avaliable }, { info.renderFinish });

		swapchain->present(infoid);
        angle += 0.02f;
    }
cleanup:
    // ---------- 14. 清理 ----------
    for (int i = 0; i < 5; i++) {
        delete resourcePacks[i].ResourcePack;
        delete resourcePacks[i].SamplerPack;
    }
    delete swapchainPack.ResourcePack;
    delete swapchainPack.SamplerPack;
    delete sampler;
    delete swapSampler;
	for (int i = 0; i < 5; i++) {
		delete mvpBuffers[i];
	}
    delete vertexBuffer;
    delete indexBuffer;
    delete framebuffer;
    delete colorTexture;
    delete depthTexture;
    delete uploadBuffer;
    if (inputTexture) delete inputTexture;

    Info("Destroy RHI Done!");
    FISIR::RHICreator::destroyRenderInterface();
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();
    return 0;
}