// BunnyPBR.slang —— 兔子 PBR 渲染（GGX metallic-roughness + 方向光 + 点光 + IBL）
//
// 与 Simulation.slang 共享同一份 GPU 侧 Bunny 布局（96 字节，与 C++ GPUBunny 严格对齐）：
//   float4 position     位置 xyz + pad
//   float4 orientation  四元数 xyzw
//   float4 velocity     线速度 xyz + 角速度(w)
//   float4 spinAxis     自旋轴 xyz + pad
//   float4 halfExtents  OBB 半边长 xyz + **金属度(w)**
//   float4 color        反照率 rgb + 粗糙度(w)
//
// per-instance 数据经 StructuredBuffer + gl_InstanceIndex 读取（后端无 instance attribute）。
//
// 环境光照（IBL）：
//   · 漫反射：SH9 辐照度（SkyIBL.h 在 CPU 侧对程序化天空做余弦卷积前的投影），
//     9 个 float4 跟着 FrameUB 进来，无需额外纹理。
//   · 镜面  ：split-sum 近似 —— 预滤波环境立方体（mip ↔ 粗糙度）+ 解析 env-BRDF。
//   · 金属度取自 halfExtents.w（原 pad 字段），因此可以真正渲染金属：
//     金属的 F0 = 反照率、漫反射为 0，没有 IBL 时会全黑 —— 这正是本样例原先
//     把金属度锁 0 的原因，现在环境光照到位，解锁。

struct Bunny {
    float4 position;
    float4 orientation;
    float4 velocity;
    float4 spinAxis;
    float4 halfExtents;
    float4 color;
};

[[vk::binding(1, 0)]] StructuredBuffer<Bunny> Bunnies  : register(t1);

// 与 Main.cpp 的 FrameUB 逐字段一致（368 字节）
[[vk::binding(0, 0)]] cbuffer FrameUB  : register(b0) {
    float4x4 ViewProj;
    float4x4 InvViewProj;      // 天空盒用；本着色器未用但保持布局一致
    float4   CameraPos;        // xyz + pad
    float4   LightDir;         // 光线行进方向 xyz + pad
    float4   LightColor;       // rgb + pad
    float4   PointLightPos;    // xyz + pad
    float4   PointLightColor;  // rgb + pad
    float4   IBLParams;        // x=辐射亮度还原系数 y=预滤波最大 mip z=镜面 IBL 强度 w=天空盒亮度
    float4   SH[9];            // 环境辐射亮度 SH9（原始系数，余弦卷积在 SHIrradiance 里做）
};

[[vk::binding(2, 0)]] TextureCube  PrefilteredCube  : register(t2);
[[vk::binding(3, 0)]] SamplerState CubeSampler      : register(s3);

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput {
    float4 pos       : SV_POSITION;
    float3 worldPos  : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float3 albedo    : TEXCOORD2;
    float  roughness : TEXCOORD3;
    float  metalness : TEXCOORD4;
};

// 单位四元数 → 旋转矩阵（行 = 基向量）
float3x3 quatToMat(float4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float3x3 m;
    m[0] = float3(1.0 - 2.0*(y*y + z*z),     2.0*(x*y + w*z),     2.0*(x*z - w*y));
    m[1] = float3(    2.0*(x*y - w*z), 1.0 - 2.0*(x*x + z*z),     2.0*(y*z + w*x));
    m[2] = float3(    2.0*(x*z + w*y),     2.0*(y*z - w*x), 1.0 - 2.0*(x*x + y*y));
    return m;
}

VSOutput mainVS(VSInput input, uint instanceID : SV_InstanceID) {
    Bunny b = Bunnies[instanceID];
    float3x3 R = quatToMat(b.orientation);

    float3 worldPos = mul(R, input.pos) + b.position.xyz;
    float3 worldNrm = mul(R, input.normal);

    VSOutput o;
    o.pos       = mul(ViewProj, float4(worldPos, 1.0));
    o.worldPos  = worldPos;
    o.normal    = worldNrm;
    o.albedo    = b.color.rgb;
    o.roughness = b.color.a;
    o.metalness = b.halfExtents.w;      // 原 pad 字段，现承载金属度
    return o;
}

// ── Cook-Torrance GGX（metallic-roughness）───────────────────────
static const float PI = 3.14159265;
static const float DIELECTRIC_F0 = 0.04;

float D_GGX(float NoH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-4);
}

float G_SchlickGGX(float NoV, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return NoV / (NoV * (1.0 - k) + k);
}

float G_Smith(float NoV, float NoL, float roughness) {
    return G_SchlickGGX(NoV, roughness) * G_SchlickGGX(NoL, roughness);
}

float3 F_Schlick(float VoH, float3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - VoH, 0.0), 5.0);
}

// 粗糙度感知的 Fresnel：粗糙表面在掠射角不该有完整的镜面反射
float3 F_SchlickRoughness(float NoV, float3 F0, float roughness) {
    return F0 + (max(1.0 - roughness, F0) - F0) * pow(max(1.0 - NoV, 0.0), 5.0);
}

float3 shadeLight(float3 L, float3 radiance, float3 N, float3 V,
                  float3 albedo, float roughness, float3 F0, float metalness) {
    float3 H  = normalize(L + V);
    float NoL = saturate(dot(N, L));
    float NoV = saturate(dot(N, V));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    float  D = D_GGX(NoH, roughness);
    float  G = G_Smith(NoV, NoL, roughness);
    float3 F = F_Schlick(VoH, F0);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metalness);   // 金属没有漫反射
    float3 spec = D * G * F / max(4.0 * NoV * NoL, 1e-4);
    float3 diff = kD * albedo / PI;

    return (diff + spec) * radiance * NoL;
}

// ── 漫反射 IBL：SH9 的余弦卷积 ──────────────────────────────────
// E(n) = Σ_l A_l Σ_m L_lm Y_lm(n)，其中 A0 = π、A1 = 2π/3、A2 = π/4。
// 把 A_l 与 Y_lm 的归一化常数一起展开，就是 Ramamoorthi & Hanrahan 的
// 经典 c1..c5 形式（c1=0.429043 c2=0.511664 c3=0.743125 c4=0.886227 c5=0.247708）。
// 返回的是**辐照度** E(n)，漫反射出射辐射亮度还要再除 π。
float3 SHIrradiance(float3 n) {
    static const float c1 = 0.429043;
    static const float c2 = 0.511664;
    static const float c3 = 0.743125;
    static const float c4 = 0.886227;
    static const float c5 = 0.247708;

    return c4 * SH[0].rgb
         + 2.0 * c2 * (SH[1].rgb * n.y + SH[2].rgb * n.z + SH[3].rgb * n.x)
         + 2.0 * c1 * (SH[4].rgb * n.x * n.y + SH[5].rgb * n.y * n.z + SH[7].rgb * n.x * n.z)
         + c3 * SH[6].rgb * n.z * n.z
         - c5 * SH[6].rgb
         + c1 * SH[8].rgb * (n.x * n.x - n.y * n.y);
}

// ── 镜面 IBL 的解析 env-BRDF（Karis 2013 的 DFG 近似）───────────
// 等价于查一张 BRDF LUT（split-sum 的第二项），省掉那张纹理与它的烘焙。
float3 EnvBRDFApprox(float3 specularColor, float roughness, float NoV) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.040, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return specularColor * AB.x + AB.y;
}

float4 mainPS(VSOutput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    // 双面光照：背面法线翻转（CullMode=None）
    if (dot(N, V) < 0.0) N = -N;

    const float  roughness = input.roughness;
    const float  metalness = saturate(input.metalness);
    const float3 albedo    = input.albedo;
    // 金属的 F0 = 反照率，电介质固定 0.04
    const float3 F0        = lerp(DIELECTRIC_F0.xxx, albedo, metalness);

    float3 color = 0.0;

    // 方向光
    float3 Ldir = normalize(-LightDir.xyz);
    color += shadeLight(Ldir, LightColor.rgb, N, V, albedo, roughness, F0, metalness);

    // 点光（平方反比衰减）
    float3 Lp = PointLightPos.xyz - input.worldPos;
    float  dist = length(Lp);
    float3 Lpn = Lp / max(dist, 1e-4);
    float  attenuation = 1.0 / max(dist * dist, 1e-4);
    color += shadeLight(Lpn, PointLightColor.rgb * attenuation, N, V, albedo, roughness, F0, metalness);

    // ── IBL ────────────────────────────────────────────────────────
    const float NoV = saturate(dot(N, V)) + 1e-4;   // 避免除零与 pow(0,5) 的边界
    const float3 Fenv = F_SchlickRoughness(NoV, F0, roughness);
    const float3 kD   = (1.0 - Fenv) * (1.0 - metalness);

    // 漫反射：E(n)=SH 余弦卷积 → 出射辐射亮度 = albedo · E(n) / π
    float3 diffuseIBL = kD * albedo * SHIrradiance(N) / PI;

    // 镜面：预滤波环境（mip 由粗糙度选）· env-BRDF
    float3 R = reflect(-V, N);
    float3 prefiltered = PrefilteredCube.SampleLevel(CubeSampler, R, roughness * IBLParams.y).rgb;
    float3 specularIBL = prefiltered * EnvBRDFApprox(F0, roughness, NoV) * IBLParams.z;

    // 环境强度：IBLParams.x 把烘焙时的峰值归一化还原成真实辐射亮度（天空盒用同一个值）
    color += (diffuseIBL + specularIBL) * IBLParams.x;

    // Reinhard 色调映射 + gamma（与 Skybox.slang 完全一致）
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);

    return float4(color, 1.0);
}
