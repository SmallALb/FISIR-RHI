// 清屏着色器：渲染前把 FrameBuffer 每个像素清为 RenderParams 指定的「远深度 + 清屏色」。
// 布局与 NaniteRender.hlsl 的 AtomicDepthTest 完全一致：低 32 位 = depth，高 32 位 = color。
// 清屏值取自 RenderParams（ClearDepth / ClearColor），与渲染参数同源，避免额外缓冲。

RWByteAddressBuffer FrameBuffer : register(u0);
// RWByteAddressBuffer Debug : register(u7);
// 复用 RenderParamsBuffer（与 NaniteRender.hlsl 的 RenderParams 布局一致，仅取需要的字段）。
cbuffer RenderParams : register(b1) {
    float4x4 VPMatrix;
    float2   screenSize;
    float    ClearDepth;
    uint    ClearColor;
    float    NearPlane;
    float    FarPlane;
};

[numthreads(16, 16, 1)]
void mainClear(uint3 tid : SV_DispatchThreadID) {
    uint2 size = uint2(screenSize);
    if (tid.x >= size.x || tid.y >= size.y) return;

    uint pixelIndex = tid.y * size.x + tid.x;
    FrameBuffer.Store(pixelIndex * 8 + 0, ClearColor);   // 低 32 位 = 颜色
    FrameBuffer.Store(pixelIndex * 8 + 4, asuint(ClearDepth));   // 高 32 位 = 深度
    // Debug.Store(pixelIndex * 8 + 0, 0xCAFEBABE);
}
