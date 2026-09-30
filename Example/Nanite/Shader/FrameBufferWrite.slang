// FrameBuffer 写入（解析）管线：把 VisBuffer 解析成最终帧缓冲。
//
// 分工：
//   · 光栅阶段（NaniteRender.slang 的软光栅 compute + 硬光栅 VS/PS）只写 **可见性** ——
//     VisBuffer 每像素一个 64 位打包字（高 32 位深度、低 32 位载荷），由 **64 位** InterlockedMin
//     一次决出最近的那个片元。深度与载荷同属一个字，所以「颜色和深度来自不同片元」不可能发生
//     （32 位时代「先原子 min 深度、再条件写载荷/颜色」的两步写正是这种竞争）。
//   · 着色只发生在本文件这一处：软硬两条路径不再各写一份相同的色块/Lambert 代码。
//
// 一个线程一个像素，**不用原子**：胜负已经在 VisBuffer 里决出来了。
// VisBuffer 布局与 ClearScreen.slang / NaniteRender.slang 必须一致：
//   每像素一个 64 位整数，高 32 位 = 深度位模式，低 32 位 = 载荷 (clusterID << 8) | 簇内三角形序号。
// 写出的 FrameBuffer 布局与呈现侧读法一致（低 32 位颜色、高 32 位深度）。

// ── 着色模式来自 RenderParams.ColorBlock（运行时开关，面板上可切）────────
//   1 = 色块：每个簇一个固定纯色（同一片区域出现两种颜色 = 两层重叠；始终是背景色 = 漏选）。
//   0 = Lambert 面着色（灰阶明暗）。

[[vk::binding(0, 0)]] RWStructuredBuffer<uint64_t> VisBuffer : register(u0);
[[vk::binding(1, 0)]] RWByteAddressBuffer     clusterPagesBuffer   : register(u1);
[[vk::binding(2, 0)]] RWByteAddressBuffer     FrameBuffer          : register(u2);
[[vk::binding(3, 0)]] cbuffer RenderParams  : register(b3) {
    float4x4 VPMatrix;
    float2   screenSize;
    float    ClearDepth;
    uint     ClearColor;
    uint     ColorBlock;   // 0 = Lambert；1 = 色块（偏移 80，与 C++ RenderParams 对齐）
    float    FarPlane;
};

// 顶点为 float3 位置（12 字节），存于簇数据区 cb+28（与 NaniteRender.slang 一致）
float3 LoadVertexPosition(uint cb, uint vertexIndex) {
    return asfloat(clusterPagesBuffer.Load3(cb + 28 + vertexIndex * 12));
}

float3 HsvToRgb(float3 c) {
    float3 p = abs(frac(c.xxx + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1.0, 1.0, 1.0), saturate(p - 1.0), c.y);
}

// 用 cb（簇数据区字节偏移，每个簇唯一且稳定）当哈希输入：同一簇每帧同色、相邻簇差异大
float3 ClusterColor(uint cb) {
    uint h = cb * 2654435761u;   // Knuth 乘法哈希
    h ^= h >> 16; h *= 2246822519u;
    h ^= h >> 13; h *= 3266489917u;
    h ^= h >> 16;
    float hue = float(h & 0x3FFu) * (1.0 / 1024.0);            // 1024 档色相
    float sat = 0.55 + float((h >> 10) & 1u) * 0.40;           // 0.55 / 0.95
    float val = 0.60 + float((h >> 11) & 3u) * (0.40 / 3.0);   // 0.60 … 1.00
    return HsvToRgb(float3(hue, sat, val));
}

uint PackColor(float3 c) {
    uint3 rgb = (uint3)(saturate(c) * 255.0 + 0.5);
    return rgb.r | (rgb.g << 8) | (rgb.b << 16);
}

[numthreads(16, 16, 1)]
void mainWriteFrameBuffer(uint3 tid : SV_DispatchThreadID) {
    uint2 size = uint2(screenSize);
    if (tid.x >= size.x || tid.y >= size.y) return;

    const uint pixelIndex = tid.y * size.x + tid.x;

    // 一次读到整个打包字：高 32 位深度、低 32 位载荷
    const uint64_t visibility = VisBuffer[pixelIndex];
    const uint depthBits = (uint)(visibility >> 32);
    // 清屏值 = 1.0f（0x3F800000）。正浮点数的位模式与数值同序，所以 >= 就是「没有几何」。
    if (depthBits >= 0x3F800000u) return;   // 该像素无几何，保留 ClearScreen 写的背景色

    const uint payload = (uint)visibility;
    const uint triIndex = payload & 0xFFu;
    const uint clusterID = payload >> 8;

    // 载荷里存的是 clusterID（页号 << 8 | 簇内偏移）而不是 cb，这里按页表把它还原成
    // 簇数据区的字节偏移 cb —— 与 NaniteRender.slang 的 mainVS 用的是同一套寻址。
    const uint pageIndex = clusterID >> 8;
    const uint clusterOffset = clusterID & 0xFFu;
    const uint pageBaseAddressOffset = clusterPagesBuffer.Load(4 + pageIndex * 4);
    const uint clusterCount = clusterPagesBuffer.Load(pageBaseAddressOffset);
    const uint clusterDataOffset = clusterPagesBuffer.Load(pageBaseAddressOffset + 4 + 4 * clusterOffset);
    const uint cb = pageBaseAddressOffset + 4 + clusterCount * 4 + clusterDataOffset;

    uint color;
    if (ColorBlock) {
        color = PackColor(ClusterColor(cb));
    }
    else {
        // Lambert 面着色：顶点无法线，用叉积求面法线 + 固定光照方向
        const uint indexDataOffset = clusterPagesBuffer.Load(cb + 0);
        const uint i = triIndex * 3u;

        const uint i0 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 0) * 4);
        const uint i1 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 1) * 4);
        const uint i2 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 2) * 4);

        const float3 p0 = LoadVertexPosition(cb, i0);
        const float3 p1 = LoadVertexPosition(cb, i1);
        const float3 p2 = LoadVertexPosition(cb, i2);

        const float3 faceNormal = normalize(cross(p2 - p0, p1 - p0));
        const float lambert = saturate(dot(faceNormal, normalize(float3(0.3f, 0.5f, 0.8f))));
        const float shade = (64.0f + lambert * 191.0f) / 255.0f;   // [64,255]，暗部不纯黑
        color = PackColor(float3(shade, shade, shade));
    }

    FrameBuffer.Store(pixelIndex * 8 + 0, color);          // 低 32 位 = 颜色
    FrameBuffer.Store(pixelIndex * 8 + 4, depthBits);      // 高 32 位 = 深度
}
