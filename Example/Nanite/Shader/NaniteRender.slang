#define NANITE_CLUSTER_OFFSET_MASK 0xFFu

[[vk::binding(0, 0)]] RWByteAddressBuffer 		clusterSelectionBuffer  : register(u0);
[[vk::binding(1, 0)]] RWByteAddressBuffer     clusterPagesBuffer  : register(u1);
struct ClusterData {
  uint indexCount;
	uint baseVertex;
	uint baseIndex;
	uint padding;
};
[[vk::binding(2, 0)]] RWStructuredBuffer<ClusterData> clusterDataBuffer  : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> EnableClusterListSw  : register(u3);   // 软光栅列表（[1] = 三角形数）
// u4（FrameBuffer）不再由本文件使用：改由 FrameBufferWrite.slang 统一写入。
// 管线布局里仍保留 0..8 全部绑定，与共用的资源包一一对应才能 bind。
[[vk::binding(5, 0)]] cbuffer RenderParams  : register(b5) {
    float4x4 VPMatrix;
    float2 screenSize;
    float ClearDepth;
    float ClearColor;
    uint     ColorBlock;   // 本文件不用；保留声明以与 C++ RenderParams 布局一致
    float FarPlane;
};
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> EnableClusterListHw  : register(u6);   // 硬光栅专用选择列表
// 软光栅的三角形工作列表（Selection 展开）：每三角形 3 个 uint
//   [0] clusterID   [1] 簇数据区字节偏移 cb   [2] 簇内三角形序号
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> SwTriList  : register(u7);
// VisBuffer（可见性缓冲）：每像素 8 字节 = 一个 64 位打包字。
// 软光栅与硬光栅都只写这里、由 **64 位** InterlockedMin 决出最近的片元；颜色不在这里决定，
// 而是由 FrameBufferWrite.slang 的解析管线按载荷里的 clusterID/triIndex 统一着色。
[[vk::binding(8, 0)]] RWStructuredBuffer<uint64_t> VisBuffer  : register(u8);

// .nanitemesh 顶点：每个顶点为 float3 位置（12 字节），存于簇数据区 cb+28。
// 索引为簇内局部序号（0..posCount-1），直接索引簇内位置，无需 baseVertex/baseIndex
// （cb+8/cb+12 实为 LODBounds 的浮点分量，并非顶点/索引基准，见 .claude/Results 报告）。
float3 LoadVertexPosition(uint cb, uint vertexIndex) {
    return asfloat(clusterPagesBuffer.Load3(cb + 28 + vertexIndex * 12));
}

// ── VisBuffer 像素格式（本文件、ClearScreen.slang、FrameBufferWrite.slang 三处必须一致）──
// 每个像素一个 64 位整数（RWStructuredBuffer<uint64_t>，步长 8 字节）：
//   高 32 位 = 深度（float 位模式；深度 ∈ [0,1] 时正浮点的位模式与数值同序，所以整数比较就是深度比较）
//   低 32 位 = 载荷 (clusterID << 8) | (簇内三角形序号 & 0xFF)
// 为什么打包成一个字：32 位时代只能「先原子 min 深度、赢了再普通写 cb / 三角形序号」，
// 两次写之间有竞争 —— 可能出现「深度是 A 的、载荷却是 B 的」错配片元；打包成 64 位后
// 一次 OpAtomicUMin 同时决出「谁赢」和「赢的是谁」，这类竞争从根上消失。
// 载荷里放的是 clusterID（页号 << 8 | 簇内偏移，24 位）而不是 cb：它和 8 位三角形序号
// 正好塞满 32 位；解析管线用页表把 clusterID 还原成 cb（多两次读，换来无竞争的原子）。

float3 CalculateBarycentric(float2 p, float2 v0, float2 v1, float2 v2) {
    float2 v0v1 = v1 - v0;
    float2 v0v2 = v2 - v0;
    float2 pv0 = p - v0;
    
    float d00 = dot(v0v1, v0v1);
    float d01 = dot(v0v1, v0v2);
    float d11 = dot(v0v2, v0v2);
    float d20 = dot(pv0, v0v1);
    float d21 = dot(pv0, v0v2);
    
    float denom = d00 * d11 - d01 * d01;
    if (abs(denom) < 0.000001f) return float3(0, 0, 0);
    
    float u = (d11 * d20 - d01 * d21) / denom;
    float v = (d00 * d21 - d01 * d20) / denom;
    float w = 1.0f - u - v;
    
    return float3(u, v, w);
}

float InterpolateDepth(float3 bary, float4 v0, float4 v1, float4 v2) {
    float ndcZ = bary.x * v0.z / v0.w + bary.y * v1.z / v1.w + bary.z * v2.z / v2.w;
    // NDC z [-1,1] -> 深度 [0,1]（0=近, 1=远，与清屏值 1.0 一致）。
    // 硬光栅 PS 里对 SV_Position.z 做同样的重映射，两条路径写进 VisBuffer 的深度才是同一套。
    return ndcZ * 0.5f + 0.5f;
}

// 写一个像素的可见性：把 (深度, clusterID, 三角形序号) 打包成一个 64 位整数，
// 用一次 **64 位原子 min** 同时决出最近的片元与它的载荷 —— 深度与载荷不可能来自不同片元。
// 依赖设备的 shaderBufferInt64Atomics（VkPhysicalDeviceShaderAtomicInt64Features）；
// Slang 会为此自动生成 OpCapability Int64Atomics 与 64 位的 OpAtomicUMin。
void WriteVisBufferPixel(uint2 pixel, float depth, uint clusterID, uint triIndex) {
    const uint pixelIndex = pixel.y * (uint)screenSize.x + pixel.x;
    const uint payload = (clusterID << 8) | (triIndex & 0xFFu);
    const uint64_t packed = ((uint64_t)asuint(depth) << 32) | (uint64_t)payload;
    uint64_t previous = 0ull;
    InterlockedMin(VisBuffer[pixelIndex], packed, previous);
}

void RasterizeTriangle(float4 v0, float4 v1, float4 v2, uint clusterID, uint triIndex) {
    // 裁剪：检查是否在近远平面内（GL 约定 z ∈ [-w, w]）
    if (v0.z < -v0.w || v0.z > v0.w ||
        v1.z < -v1.w || v1.z > v1.w ||
        v2.z < -v2.w || v2.z > v2.w) {
        return;
    }
    
    // 透视除法
    float4 ndc0 = v0 / v0.w;
    float4 ndc1 = v1 / v1.w;
    float4 ndc2 = v2 / v2.w;
    
    // 裁剪：检查NDC范围
    if (ndc0.x < -1.0f || ndc0.x > 1.0f || ndc0.y < -1.0f || ndc0.y > 1.0f ||
        ndc1.x < -1.0f || ndc1.x > 1.0f || ndc1.y < -1.0f || ndc1.y > 1.0f ||
        ndc2.x < -1.0f || ndc2.x > 1.0f || ndc2.y < -1.0f || ndc2.y > 1.0f) {
        return;
    }
    
    // 计算包围盒
    float2 v0s = float2(
        (ndc0.x * 0.5f + 0.5f) * screenSize.x,
        (-ndc0.y * 0.5f + 0.5f) * screenSize.y
    );
    float2 v1s = float2(
        (ndc1.x * 0.5f + 0.5f) * screenSize.x,
        (-ndc1.y * 0.5f + 0.5f) * screenSize.y
    );
    float2 v2s = float2(
        (ndc2.x * 0.5f + 0.5f) * screenSize.x,
        (-ndc2.y * 0.5f + 0.5f) * screenSize.y
    );
    
    float2 minPos = floor(min(min(v0s, v1s), v2s));
    float2 maxPos = ceil(max(max(v0s, v1s), v2s));
    
    uint2 minPixel = uint2(max(0, minPos));
    uint2 maxPixel = uint2(min(screenSize - 1, maxPos));
    
    for (uint y = minPixel.y; y <= maxPixel.y; y++) {
        for (uint x = minPixel.x; x <= maxPixel.x; x++) {
            float2 pixelPos = float2(x, y) + 0.5f;
            
            float3 bary = CalculateBarycentric(pixelPos, v0s, v1s, v2s);
            
            float bias = 1e-6;
            if (bary.x < -bias || bary.y < -bias || bary.z < -bias) continue;
            float depth = InterpolateDepth(bary, v0, v1, v2);
            
            if (depth < 0.0f || depth > 1.0f) {
                continue;
            }
            
            uint pixelIndex = y * (uint)screenSize.x + x;
            uint curDepth = (uint)(VisBuffer[pixelIndex] >> 32);   // 高 32 位 = 当前最近深度
            if (asuint(depth) >= curDepth) continue;   // 已经更远，无需原子

            WriteVisBufferPixel(uint2(x, y), depth, clusterID, triIndex);
        }
    }
}

// 软光栅：光栅化一个三角形。cb（簇数据区字节偏移）由工作列表直接给出，
// 所以这里不再重复「页表 → 簇记录」的依赖链；clusterID 也由工作列表带进来，
// 用来填 VisBuffer 的载荷（解析管线要靠它还原 cb）。着色不在这里做 —— 只把
// (深度, clusterID, triIndex) 打包写进 VisBuffer，颜色交给 FrameBufferWrite.slang。
void ProcessTriangle(uint cb, uint clusterID, uint triIndex) {
    uint indexDataOffset = clusterPagesBuffer.Load(cb + 0);  // 索引数据相对 cb 的字节偏移
    uint indexCount = clusterPagesBuffer.Load(cb + 4);       // 索引总数（三角形列表）

    uint i = triIndex * 3u;
    if (i + 2u >= indexCount) return;                        // 防御：越界的残留条目

    uint i0 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 0) * 4);
    uint i1 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 1) * 4);
    uint i2 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 2) * 4);

    // 顶点为 float3 位置（12 字节），索引为簇内局部序号，直接索引 cb+28 处的位置
    float3 p0 = LoadVertexPosition(cb, i0);
    float3 p1 = LoadVertexPosition(cb, i1);
    float3 p2 = LoadVertexPosition(cb, i2);

    RasterizeTriangle(mul(float4(p0, 1.0f), VPMatrix),
                      mul(float4(p1, 1.0f), VPMatrix),
                      mul(float4(p2, 1.0f), VPMatrix),
                      clusterID, triIndex);
}

[numthreads(64, 1, 1)]
void mainRender(uint3 dispatchThreadID : SV_DispatchThreadID) {
    // 软光栅：**一个线程一个三角形**。工作列表由 Selection 展开（它已经逐簇在跑），
    // 派发上限 = 最大簇数 × 每簇三角形上限，线程用列表里的实际数量提前退出。
    // 列表条目是 (clusterID, cb, triIndex)：cb 直接用来取顶点，clusterID 进 VisBuffer 载荷。
    const uint i = dispatchThreadID.x;
    if (i >= EnableClusterListSw[1]) return;                  // [1] = 本帧软光栅三角形数
    const uint slot = i * 3u;
    ProcessTriangle(SwTriList[slot + 1], SwTriList[slot + 0], SwTriList[slot + 2]);
}

// ═══════════════════════════════════════════════════════════════════
// 硬光栅顶点/像素着色器。
// VS 用 SV_VertexID + SV_InstanceID 从簇页缓冲直接取顶点，无需顶点/索引缓冲：
//   - instanceID = 硬光栅列表里的簇序号（DrawIndirect 的 firstInstance）
//   - vertexID   = 簇内三角形列表的局部顶点序号（0..indexCount-1）
// PS 不写附件，也不着色，只把 (深度, clusterID, triIndex) 打包写进 VisBuffer —— 与软光栅
// 写的是同一个缓冲，两者靠那里的 64 位原子 min 决出可见性。最终颜色由
// FrameBufferWrite.slang 的解析管线统一产生（离屏附件只是占位，管线关掉了深度测试/写）。
// ═══════════════════════════════════════════════════════════════════

struct NaniteVSOutput {
    float4 clipPos : SV_Position;
    // 同一三角形的三个顶点 clusterID/triIndex 相同，用 nointerpolation 避免被插值
    nointerpolation uint clusterID : TEXCOORD0;
    nointerpolation uint triIndex  : TEXCOORD1;
};

NaniteVSOutput mainVS(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    NaniteVSOutput output = (NaniteVSOutput)0;

    // 防御性守卫：DrawIndirect 的尾部残留项已被 CPU 清零（vertexCount=0），
    // 理论上不会派发到 instanceID >= 选中簇数；此处仅保证极端情况输出退化顶点。
    uint totalClusters = EnableClusterListHw[0];
    if (instanceID >= totalClusters) {
        output.clipPos = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return output;
    }

    // 与软光栅一致地定位簇数据区 cb
    uint clusterID = EnableClusterListHw[instanceID + 1];
    uint pageIndex = clusterID >> 8;
    uint clusterOffset = clusterID & NANITE_CLUSTER_OFFSET_MASK;

    uint pageBaseAddressOffset = clusterPagesBuffer.Load(4 + pageIndex * 4);
    uint clusterCount = clusterPagesBuffer.Load(pageBaseAddressOffset);
    uint clusterDataOffset = clusterPagesBuffer.Load(pageBaseAddressOffset + 4 + 4 * clusterOffset);
    uint cb = pageBaseAddressOffset + 4 + clusterCount * 4 + clusterDataOffset;

    uint indexDataOffset = clusterPagesBuffer.Load(cb + 0);

    // 当前顶点所属三角形 = vertexID / 3；查询用到的顶点位置按 vertexID 取索引
    uint tri = vertexID / 3;
    uint idx = clusterPagesBuffer.Load(cb + indexDataOffset + vertexID * 4);
    float3 pos = LoadVertexPosition(cb, idx);

    output.clusterID = clusterID;
    output.triIndex = tri;
    output.clipPos = mul(float4(pos, 1.0f), VPMatrix);
    // 硬光栅 Y 翻转：GLM(RH_NO) 裁剪空间 Y 朝上，而 Vulkan 视口把 +Y 映射到帧缓冲下边缘，
    // 导致离屏纹理上下颠倒。取反等价于软光栅 RasterizeTriangle 里 -ndc.y 的显式翻转，
    // 使纹理第 0 行 = 场景上方，与呈现 PS 采样方向一致。
    output.clipPos.y = -output.clipPos.y;
    return output;
}

float4 mainPS(NaniteVSOutput input) : SV_Target {
    // 只写可见性；颜色留给解析管线。附件只是占位，返回什么颜色都无所谓。
    WriteVisBufferPixel((uint2)input.clipPos.xy, input.clipPos.z * 0.5 + 0.5, input.clusterID, input.triIndex);
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}