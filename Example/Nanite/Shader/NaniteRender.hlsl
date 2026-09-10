
#define NANITE_CLUSTER_OFFSET_MASK 0xFFu

RWByteAddressBuffer 		clusterSelectionBuffer : register(u0);
RWByteAddressBuffer     clusterPagesBuffer : register(u1);
struct ClusterData {
  uint indexCount;
	uint baseVertex;
	uint baseIndex;
	uint padding;
};
RWStructuredBuffer<ClusterData> clusterDataBuffer : register(u2);
RWStructuredBuffer<uint> EnableClusterList : register(u3);
RWByteAddressBuffer FrameBuffer : register(u4);
// 遗留的自旋锁缓冲（现已不再使用：DXC 不支持 64 位原子，改用 32 位 depth 原子，见 AtomicDepthTest）
RWByteAddressBuffer FrameLock : register(u5);
cbuffer RenderParams : register(b6) {
    float4x4 VPMatrix;
    float2 screenSize;
    float ClearDepth;
    float ClearColor;
    float NearPlane;
    float FarPlane;
};

// .nanitemesh 顶点：每个顶点为 float3 位置（12 字节），存于簇数据区 cb+28。
// 索引为簇内局部序号（0..posCount-1），直接索引簇内位置，无需 baseVertex/baseIndex
// （cb+8/cb+12 实为 LODBounds 的浮点分量，并非顶点/索引基准，见 .claude/Results 报告）。
float3 LoadVertexPosition(uint cb, uint vertexIndex) {
    return asfloat(clusterPagesBuffer.Load3(cb + 28 + vertexIndex * 12));
}

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
    return ndcZ * 0.5f + 0.5f;   // NDC z [-1,1] -> 深度 [0,1]（0=近, 1=远，与清屏值 1.0 一致）
}

void AtomicDepthTest(uint2 pixel, float depth, uint color) {
    uint pixelIndex = pixel.y * (uint)screenSize.x + pixel.x;
    uint offset = pixelIndex * 8;

    // DXC 的 SPIR-V 后端不支持 64 位原子：InterlockedMin64 会被静默截断为 32 位
    // OpAtomicUMin（只作用于低 32 位 = color），导致高 32 位的 depth 被丢弃、深度测试失效。
    // 退化为 32 位原子 min 只作用于高 32 位的 depth（float-as-uint 单调，小者近）。
    uint oldDepthBits;
    FrameBuffer.InterlockedMin(offset + 4, asuint(depth), oldDepthBits);

    // 本次更近（新值 < 旧值）才写颜色（低 32 位）。颜色写与深度原子不是同一原子操作，
    // 极端重叠像素可能有颜色竞争，但深度值始终正确。
    if (asuint(depth) < oldDepthBits) {
        uint dummy;
        FrameBuffer.InterlockedExchange(offset + 0, color, dummy);
    }
}

void RasterizeTriangle(float4 v0, float4 v1, float4 v2, uint color) {
    // 裁剪：检查是否在近远平面内
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
            
            uint2 pixel = uint2(x, y);
            AtomicDepthTest(pixel, depth, color);
        }
    }
}

void ProcessCluster(uint clusterIndex) {
    // 选中簇编码 ID：高 8 位 pageIndex，低 8 位 clusterOffset
    uint clusterID = EnableClusterList[clusterIndex + 1];
    uint pageIndex = clusterID >> 8;
    uint clusterOffset = clusterID & NANITE_CLUSTER_OFFSET_MASK;

    // 定位簇数据区 cb（与 ClusterSelection.hlsl 的 LoadClusterPayload 逐字节一致）
    uint pageBaseAddressOffset = clusterPagesBuffer.Load(4 + pageIndex * 4);
    uint clusterCount = clusterPagesBuffer.Load(pageBaseAddressOffset);
    uint clusterDataOffset = clusterPagesBuffer.Load(pageBaseAddressOffset + 4 + 4 * clusterOffset);
    uint cb = pageBaseAddressOffset + 4 + clusterCount * 4 + clusterDataOffset;

    uint indexDataOffset = clusterPagesBuffer.Load(cb + 0);  // 索引数据相对 cb 的字节偏移
    uint indexCount = clusterPagesBuffer.Load(cb + 4);       // 索引总数（三角形列表）


    for (uint i = 0; i < indexCount; i += 3) {
        uint i0 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 0) * 4);
        uint i1 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 1) * 4);
        uint i2 = clusterPagesBuffer.Load(cb + indexDataOffset + (i + 2) * 4);

        // 顶点为 float3 位置（12 字节），索引为簇内局部序号，直接索引 cb+28 处的位置
        float3 p0 = LoadVertexPosition(cb, i0);
        float3 p1 = LoadVertexPosition(cb, i1);
        float3 p2 = LoadVertexPosition(cb, i2);

        // 简单 Lambert 面着色：顶点无法线，用叉积求面法线 + 固定光照方向，
        // 使网格呈现立体明暗而非纯色色块。
        float3 faceNormal = normalize(cross(p2 - p0, p1 - p0));
        float lambert = saturate(dot(faceNormal, normalize(float3(0.3f, 0.5f, 0.8f))));
        uint shade = uint(64.0f + lambert * 191.0f);   // [64,255]，暗部不纯黑以便与黑背景区分
        uint color = (shade<<16) | (shade << 8) | (shade);

        float4 clip0 = mul(float4(p0, 1.0f), VPMatrix);
        float4 clip1 = mul(float4(p1, 1.0f), VPMatrix);
        float4 clip2 = mul(float4(p2, 1.0f), VPMatrix);

        RasterizeTriangle(clip0, clip1, clip2, color);
    }
}

[numthreads(64, 1, 1)]
void mainRender(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint clusterIndex = dispatchThreadID.x;
    uint totalClusters = EnableClusterList[0];
    
    if (clusterIndex >= totalClusters) return;
    
    
    ProcessCluster(clusterIndex);

}
