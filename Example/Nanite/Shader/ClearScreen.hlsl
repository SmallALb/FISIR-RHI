// 清屏着色器：每帧先把每个像素清成
//   · HZB L0     ：**上一帧**的深度（在本 pass 里、清掉 VisBuffer 之前抄出来），供 HZBBuild 建金字塔；
//   · FrameBuffer：RenderParams 指定的「远深度 + 清屏色」；
//   · VisBuffer  ：远深度 + 空 cb/triIndex（= 本像素没有几何）。
// FrameBuffer 布局与呈现侧读法一致（低 32 位 = 颜色 R|G<<8|B<<16、高 32 位 = 深度位模式）；
// VisBuffer 布局与 NaniteRender.hlsl / FrameBufferWrite.hlsl 一致（每像素 3 个 uint）；
// HZB 每像素 1 个 uint（深度位模式），层级 1..7 由 HZBBuild.hlsl 接着建。
// 清屏值取自 RenderParams（ClearDepth / ClearColor），与渲染参数同源，避免额外缓冲。

RWByteAddressBuffer FrameBuffer : register(u0);
RWByteAddressBuffer VisBuffer   : register(u2);
RWByteAddressBuffer HZB         : register(u3);   // L0：全分辨率
// 复用 RenderParamsBuffer（与 NaniteRender.hlsl 的 RenderParams 布局一致，仅取需要的字段）。
cbuffer RenderParams : register(b1) {
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

    uint pixelIndex = tid.y * size.x + tid.x;

    // 先把本像素上一帧的深度抄进 HZB L0 —— 必须早于下面清 VisBuffer
    HZB.Store(pixelIndex * 4 + 0, VisBuffer.Load(pixelIndex * 12 + 0));

    FrameBuffer.Store(pixelIndex * 8 + 0, ClearColor);   // 低 32 位 = 颜色
    FrameBuffer.Store(pixelIndex * 8 + 4, asuint(ClearDepth));   // 高 32 位 = 深度

    // VisBuffer 也清成远深度：解析管线用它判断「这个像素有没有几何」。
    VisBuffer.Store(pixelIndex * 12 + 0, asuint(ClearDepth));
    VisBuffer.Store(pixelIndex * 12 + 4, 0u);
    VisBuffer.Store(pixelIndex * 12 + 8, 0u);
}
