// HZB（Hierarchical Z-Buffer / 深度金字塔）构建：给 Selection 做遮挡剔除用。
//
// 数据来源是 **上一帧** 的深度：ClearScreen.hlsl 在清屏前把上一帧 VisBuffer 的深度逐像素
// 抄进 HZB 的 L0（全分辨率），本 pass 再把 L0 逐级 2×2 取 min 建出 L1..L7。
// min 规约得到的是「该矩形内最近的遮挡物深度」—— 簇的最近点都比它还远，就说明整簇被挡住。
//
// ── 层号走 push constant（取代原先「每层一个入口点 + 一条管线」）──────────────
// 层级之间必须**有序**：L(n) 要读 L(n-1)，所以一层一次 dispatch。而「本轮建第几层」
// 原先只能靠 7 个入口点 mainHzb1..mainHzb7 各配一条管线来表达 —— 因为那时 RHI 没有
// push constant，而 uniform buffer 会被「CPU 先录完所有 dispatch、GPU 后执行」读成同一个值。
// 现在直接 vkCmdPushConstants：**每次调用立即改当前值**，7 次 dispatch 各推各的层号。
//
// 所有层级连续存放在同一个 buffer 里，层级基址按 ceil 尺寸前缀和算（见 HzbLevelBase），
// 这样 Selection 端只要一个整数下标就能取任意层任意 texel，不需要 mip 视图。

#define NANITE_HZB_MAX_LEVEL 7   // L0 全分辨率，L7 ≈ 15×9（1920×1080）；0..7 共 8 层

// 与 C++ 侧的 HzbPushConstants 必须逐字节一致
struct HzbPushConstants {
    uint level;                  // 本轮要建的层级（1..NANITE_HZB_MAX_LEVEL）
};
[[vk::push_constant]] ConstantBuffer<HzbPushConstants> gPush;

RWByteAddressBuffer HZB : register(u0);
cbuffer RenderParams : register(b1) {
    float4x4 VPMatrix;
    float2   screenSize;
    float    ClearDepth;
    uint     ClearColor;
    uint     ColorBlock;
    float    FarPlane;
};

uint HzbDim(uint full, uint level) { return (full + (1u << level) - 1u) >> level; }

// 层级 L 的起始 texel 下标 = 前面所有层级的 texel 数之和
uint HzbLevelBase(uint level) {
    uint base = 0u;
    for (uint l = 0u; l < level; ++l) base += HzbDim((uint)screenSize.x, l) * HzbDim((uint)screenSize.y, l);
    return base;
}

// 一层：每个 texel = 上一层 2×2 的 min（越界处夹到上一层边界，min 无序、重复取也正确）
[numthreads(16, 16, 1)]
void mainHzb(uint3 tid : SV_DispatchThreadID) {
    const uint level  = gPush.level;
    const uint width  = (uint)screenSize.x;
    const uint height = (uint)screenSize.y;
    if (width == 0u || height == 0u || level == 0u || level > NANITE_HZB_MAX_LEVEL) return;

    const uint levelWidth  = HzbDim(width, level);
    const uint levelHeight = HzbDim(height, level);
    if (tid.x >= levelWidth || tid.y >= levelHeight) return;

    const uint prevWidth  = HzbDim(width, level - 1u);
    const uint prevHeight = HzbDim(height, level - 1u);
    const uint prevBase   = HzbLevelBase(level - 1u);

    const uint x0 = min(tid.x * 2u, prevWidth - 1u);
    const uint y0 = min(tid.y * 2u, prevHeight - 1u);
    const uint x1 = min(x0 + 1u, prevWidth - 1u);
    const uint y1 = min(y0 + 1u, prevHeight - 1u);

    uint m = HZB.Load((prevBase + y0 * prevWidth + x0) * 4u);
    m = min(m, HZB.Load((prevBase + y0 * prevWidth + x1) * 4u));
    m = min(m, HZB.Load((prevBase + y1 * prevWidth + x0) * 4u));
    m = min(m, HZB.Load((prevBase + y1 * prevWidth + x1) * 4u));

    HZB.Store((HzbLevelBase(level) + tid.y * levelWidth + tid.x) * 4u, m);
}
