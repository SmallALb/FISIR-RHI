// BunnyPBR.hlsl —— 兔子 PBR 渲染（GGX dielectric + 方向光 + 点光）
//
// 与 Simulation.hlsl 共享同一份 GPU 侧 Bunny 布局（96 字节，与 C++ GPUBunny 严格对齐）：
//   float4 position     位置 xyz + pad
//   float4 orientation  四元数 xyzw
//   float4 velocity     线速度 xyz + 角速度(w)
//   float4 spinAxis     自旋轴 xyz + pad
//   float4 halfExtents  OBB 半边长 xyz + pad
//   float4 color        反照率 rgb + 粗糙度(w)
//
// per-instance 数据经 StructuredBuffer + gl_InstanceIndex 读取（后端无 instance attribute）。
// 金属度锁 0（无 IBL/cubemap，金属会全黑），仅演示 GGX 高光 + 粗糙度。

struct Bunny {
    float4 position;
    float4 orientation;
    float4 velocity;
    float4 spinAxis;
    float4 halfExtents;
    float4 color;
};

StructuredBuffer<Bunny> Bunnies : register(t1);

cbuffer FrameUB : register(b0) {
    float4x4 ViewProj;
    float4   CameraPos;        // xyz + pad
    float4   LightDir;         // 光线行进方向 xyz + pad
    float4   LightColor;       // rgb + pad
    float4   PointLightPos;    // xyz + pad
    float4   PointLightColor;  // rgb + pad
};

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
    return o;
}

// ── Cook-Torrance GGX（dielectric，F0=0.04）──────────────────────
static const float PI = 3.14159265;
static const float F0 = 0.04;

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

float3 F_Schlick(float VoH) {
    return float3(F0, F0, F0) + (1.0 - F0) * pow(max(1.0 - VoH, 0.0), 5.0);
}

float3 shadeLight(float3 L, float3 radiance, float3 N, float3 V, float3 albedo, float roughness) {
    float3 H  = normalize(L + V);
    float NoL = saturate(dot(N, L));
    float NoV = saturate(dot(N, V));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    float  D = D_GGX(NoH, roughness);
    float  G = G_Smith(NoV, NoL, roughness);
    float3 F = F_Schlick(VoH);

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - F0);   // 金属度 = 0
    float3 spec = D * G * F / max(4.0 * NoV * NoL, 1e-4);
    float3 diff = kD * albedo / PI;

    return (diff + spec) * radiance * NoL;
}

float4 mainPS(VSOutput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    // 双面光照：背面法线翻转（CullMode=None）
    if (dot(N, V) < 0.0) N = -N;

    float3 color = 0.0;

    // 方向光
    float3 Ldir = normalize(-LightDir.xyz);
    color += shadeLight(Ldir, LightColor.rgb, N, V, input.albedo, input.roughness);

    // 点光（平方反比衰减）
    float3 Lp = PointLightPos.xyz - input.worldPos;
    float  dist = length(Lp);
    float3 Lpn = Lp / max(dist, 1e-4);
    float  attenuation = 1.0 / max(dist * dist, 1e-4);
    color += shadeLight(Lpn, PointLightColor.rgb * attenuation, N, V, input.albedo, input.roughness);

    // 环境光（无 IBL 的最小可见度）
    color += input.albedo * 0.04;

    // Reinhard 色调映射 + gamma
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);

    return float4(color, 1.0);
}
