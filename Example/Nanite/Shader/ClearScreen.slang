// 清屏着色器：每帧先把每个像素清成
//   · HZB L0     ：**上一帧**的深度（在本 pass 里、清掉 VisBuffer 之前抄出来），供 HZBBuild 建金字塔；
//   · FrameBuffer：RenderParams 指定的「远深度 + 清屏色」；
//   · VisBuffer  ：远深度 + 空载荷（= 本像素没有几何）。
// FrameBuffer 布局与呈现侧读法一致（低 32 位 = 颜色 R|G<<8|B<<16、高 32 位 = 深度位模式）；
// VisBuffer 布局与 NaniteRender.slang / FrameBufferWrite.slang 一致：
//   每个像素一个 64 位整数（RWStructuredBuffer<uint64_t>，步长 8 字节），
//   高 32 位 = 深度位模式，低 32 位 = 载荷 (clusterID << 8) | 簇内三角形序号。
// HZB 每像素 1 个 uint（深度位模式），层级 1..7 由 HZBBuild.slang 接着建。
// 清屏值取自 RenderParams（ClearDepth / ClearColor），与渲染参数同源，避免额外缓冲。

[[vk::binding(0, 0)]] RWByteAddressBuffer FrameBuffer  : register(u0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint64_t> VisBuffer : register(u2);
[[vk::binding(3, 0)]] RWByteAddressBuffer HZB          : register(u3);   // L0：全分辨率
// 复用 RenderParamsBuffer（与 NaniteRender.slang 的 RenderParams 布局一致，仅取需要的字段）。
[[vk::binding(1, 0)]] cbuffer RenderParams  : register(b1) {
    float4x4 VPMatrix;
    float2   screenSize;
    float    ClearDepth;
    uint    ClearColor;
    uint     ColorBlock;   // 本文件不用；保留声明以与 C++ RenderParams 布局一致
    float    FarPlane;
};

[numthreads(16, 16, 1)]
void mainClear(uint3 tid : SV_DispatchThreadID) {
    uint2 size = uint2(screenSize);
    if (tid.x >= size.x || tid.y >= size.y) return;

    const uint pixelIndex = tid.y * size.x + tid.x;

    // 先把本像素上一帧的深度抄进 HZB L0 —— 必须早于下面清 VisBuffer
    HZB.Store(pixelIndex * 4 + 0, (uint)(VisBuffer[pixelIndex] >> 32));

    FrameBuffer.Store(pixelIndex * 8 + 0, ClearColor);   // 低 32 位 = 颜色
    FrameBuffer.Store(pixelIndex * 8 + 4, asuint(ClearDepth));   // 高 32 位 = 深度

    // VisBuffer 清成「远深度 + 空载荷」：解析管线只看深度是否等于远平面来判断有没有几何，
    // 载荷清零后即 (clusterID << 8) | triIndex == 0，不会被误当成几何。
    VisBuffer[pixelIndex] = (uint64_t)asuint(ClearDepth) << 32;
}
