// Skybox.slang —— 全屏三角形绘制天空盒背景
//
// 顶点阶段不用顶点缓冲：靠 SV_VertexID 生成一个覆盖全屏的大三角形
// （比两个三角形的 quad 少一次三角形 setup，且没有对角线上的重复着色）。
//
// SV_POSITION.z 直接写 1.0（远平面），配合：
//   · 深度测试 LESS_EQUAL（清屏值为 1.0，故「没有不透明几何」的像素通过）
//   · 深度写**关闭**（天空盒不参与深度，也不影响后续任何东西）
// 因此天空盒必须在不透明几何**之后**、同一个 render pass 内绘制。
//
// 方向由 InvViewProj 反投影远平面点得到，与几何用的是同一套 ViewProj，
// 所以背景与地物的视线天然对齐（不需要单独传相机基向量，也不会有 Y 翻转问题）。

// 与 Main.cpp 的 FrameUB 逐字段一致（368 字节）；天空盒只用到 InvViewProj /
// CameraPos / IBLParams.w，但整段声明保持一致以免偏移漂移。
[[vk::binding(0, 0)]] cbuffer FrameUB  : register(b0) {
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4   CameraPos;
    float4   LightDir;
    float4   LightColor;
    float4   PointLightPos;
    float4   PointLightColor;
    float4   IBLParams;        // x=辐射亮度还原系数 y=预滤波最大 mip z=镜面 IBL 强度 w=天空盒亮度
    float4   SH[9];            // 环境辐射亮度 SH9（本着色器不用，仅为保持布局一致）
};

[[vk::binding(2, 0)]] TextureCube  EnvCube      : register(t2);
[[vk::binding(0, 0)]] SamplerState CubeSampler  : register(s0);

struct VSOutput {
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;
};

VSOutput mainVS(uint vertexID : SV_VertexID) {
    // 全屏三角形：id 0/1/2 → (-1,-1) (3,-1) (-1,3)
    float2 uv  = float2((vertexID << 1) & 2, vertexID & 2);
    float2 ndc = uv * 2.0 - 1.0;

    VSOutput o;
    o.pos = float4(ndc, 1.0, 1.0);   // z/w = 1.0 → 最远

    // 反投影远平面点；方向与「投影用的是哪套深度约定」无关，
    // 只要是同一个像素上落在相机前方的点即可。
    float4 world = mul(InvViewProj, float4(ndc, 1.0, 1.0));
    o.dir = world.xyz / world.w - CameraPos.xyz;
    return o;
}

float4 mainPS(VSOutput input) : SV_TARGET {
    float3 dir = normalize(input.dir);

    // 环境立方体贴图第 0 级就是烘焙好的天空；IBLParams.x 把烘焙时的峰值归一化还原成
    // 真实辐射亮度（HDR：太阳圆盘远大于 1），w 是背景的独立倍率。
    float3 color = EnvCube.SampleLevel(CubeSampler, dir, 0).rgb * IBLParams.x * IBLParams.w;

    // 与 BunnyPBR.slang 完全一致的 Reinhard 色调映射 + gamma：
    // 背景与被照亮的地物必须走同一条曝光曲线，否则两者对不上。
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);

    return float4(color, 1.0);
}
