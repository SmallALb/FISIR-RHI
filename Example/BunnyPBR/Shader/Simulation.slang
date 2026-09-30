// Simulation.slang —— 兔子刚体碰撞仿真（全在 compute shader 上跑）
//
// 每帧一个 dispatch，numthreads(64,1,1)，N 只兔子各占一个线程：
//   1. 积分：位置 += 速度·dt；绕自旋轴旋转角速度·dt（脚本自旋）
//   2. 碰撞：OBB-OBB SAT（15 轴）窄相，成对冲量 + 位置修正，10 次迭代松弛
//   3. 撞墙：大盒体边界按半边长夹取 + 速度反弹
//   4. 碰撞后自旋方向反转（脚本行为，每帧至多一次）
//
// 对称冲量：对无序对 {i,j}，双方各自只写自己，用同一条公式算冲量，
// 因此动量天然守恒（等质量、m=1，单边冲量 = (1+e)·vrel / 2）。
//
// 注意：AllMemoryBarrierWithGroupSync 只在单线程组内严格同步；
// 若 N > 64（跨线程组）存在良性读写竞态（糊弄法，先看出效果）。

struct Bunny {
    float4 position;
    float4 orientation;
    float4 velocity;
    float4 spinAxis;
    float4 halfExtents;
    float4 color;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<Bunny> Bunnies  : register(u1);

[[vk::binding(0, 0)]] cbuffer SimParams  : register(b0) {
    float dt;            // 帧步长（秒）
    float boxHalf;       // 大盒体半边长
    float restitution;   // 恢复系数 e
    float pad0;
    uint  numBunnies;
    uint  pad1, pad2, pad3;
};

static const int COLLIDE_ITERS = 10;

float4 quatMul(float4 a, float4 b) {
    return float4(
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z);
}

float4 axisAngle(float3 axis, float angle) {
    float halfAngle = angle * 0.5;
    float s = sin(halfAngle);
    return float4(axis * s, cos(halfAngle));
}

float3x3 quatToMat(float4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float3x3 m;
    m[0] = float3(1.0 - 2.0*(y*y + z*z),     2.0*(x*y + w*z),     2.0*(x*z - w*y));
    m[1] = float3(    2.0*(x*y - w*z), 1.0 - 2.0*(x*x + z*z),     2.0*(y*z + w*x));
    m[2] = float3(    2.0*(x*z + w*y),     2.0*(y*z - w*x), 1.0 - 2.0*(x*x + y*y));
    return m;
}

// OBB-OBB 分离轴检测：若重叠返回 true，输出最小穿透轴（从 a 指向 b）与穿透深度。
bool obbOverlap(
    float3 aPos, float3x3 aR, float3 aE,
    float3 bPos, float3x3 bR, float3 bE,
    out float3 axis, out float depth)
{
    axis  = float3(0.0, 0.0, 0.0);   // 提前返回路径也保证 out 参数有定义
    depth = 0.0;

    float3 T = bPos - aPos;
    float3 aAxis[3] = { aR[0], aR[1], aR[2] };
    float3 bAxis[3] = { bR[0], bR[1], bR[2] };

    float  minDepth = 1e30;
    float3 bestAxis = float3(0.0, 0.0, 0.0);
    float  sign = 1.0;

    for (int i = 0; i < 15; ++i) {
        float3 L;
        if (i < 3)          L = aAxis[i];
        else if (i < 6)     L = bAxis[i - 3];
        else {
            int ai = (i - 6) / 3;
            int bi = (i - 6) % 3;
            L = cross(aAxis[ai], bAxis[bi]);
            if (dot(L, L) < 1e-8) continue;   // 平行轴退化
        }
        L = normalize(L);

        float ra = aE.x*abs(dot(aAxis[0], L)) + aE.y*abs(dot(aAxis[1], L)) + aE.z*abs(dot(aAxis[2], L));
        float rb = bE.x*abs(dot(bAxis[0], L)) + bE.y*abs(dot(bAxis[1], L)) + bE.z*abs(dot(bAxis[2], L));
        float d = abs(dot(T, L));
        float overlap = ra + rb - d;
        if (overlap <= 0.0) return false;     // 找到分离轴 → 不重叠

        if (overlap < minDepth) {
            minDepth = overlap;
            bestAxis = L;
            sign = (dot(T, L) >= 0.0) ? 1.0 : -1.0;   // 法线从 a 指向 b
        }
    }

    axis  = bestAxis * sign;
    depth = minDepth;
    return true;
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    // 注意：绝不能在这里 `return`——AllMemoryBarrierWithGroupSync 要求线程组内
    // 全部 64 个线程都到达屏障，提前返回是未定义行为，会在 GPU 上随机挂死。
    // 用 active 标志保护内存读写，但让所有线程都走完每个屏障。
    bool active = i < numBunnies;

    // ── 1. 积分（每线程只读写自己的兔子，无竞态）────────────────
    if (active) {
        Bunny b = Bunnies[i];
        b.position.xyz += b.velocity.xyz * dt;
        float3 axis = normalize(b.spinAxis.xyz + 1e-6);
        b.orientation = quatMul(axisAngle(axis, b.velocity.w * dt), b.orientation);
        Bunnies[i] = b;
    }
    AllMemoryBarrierWithGroupSync();

    // ── 2. OBB 碰撞（成对冲量 + 位置修正，10 次迭代松弛）─────────
    bool collided = false;
    for (int iter = 0; iter < COLLIDE_ITERS; ++iter) {
        if (active) {
            Bunny b = Bunnies[i];
            float3x3 bR = quatToMat(b.orientation);

            float3 dv = 0.0;   // 速度增量
            float3 dp = 0.0;   // 位置修正增量

            for (uint j = 0; j < numBunnies; ++j) {
                if (j == i) continue;
                Bunny o = Bunnies[j];
                float3x3 oR = quatToMat(o.orientation);

                float3 n; float d;
                if (obbOverlap(b.position.xyz, bR, b.halfExtents.xyz,
                               o.position.xyz, oR, o.halfExtents.xyz, n, d)) {
                    collided = true;
                    float vrel = dot(b.velocity.xyz - o.velocity.xyz, n);
                    if (vrel > 0.0) {                   // n 指向 b→o，接近时 dot(v_b-v_o,n) > 0
                        dv -= (1.0 + restitution) * vrel * 0.5 * n;
                    }
                    dp -= n * d * 0.5;                  // 位置修正（双方各一半）
                }
            }

            b.velocity.xyz += dv;
            b.position.xyz += dp;
            Bunnies[i] = b;
        }
        AllMemoryBarrierWithGroupSync();
    }

    // ── 3. 撞墙（大盒体边界）───────────────────────────────────
    if (active) {
        Bunny b = Bunnies[i];
        float3 h = b.halfExtents.xyz;
        for (int k = 0; k < 3; ++k) {
            float lo = -boxHalf + h[k];
            float hi =  boxHalf - h[k];
            if (b.position[k] < lo) { b.position[k] = lo; b.velocity[k] = -b.velocity[k] * restitution; collided = true; }
            if (b.position[k] > hi) { b.position[k] = hi; b.velocity[k] = -b.velocity[k] * restitution; collided = true; }
        }

        // ── 4. 碰撞后自旋方向反转（脚本行为）────────────────────
        if (collided) b.velocity.w = -b.velocity.w;

        Bunnies[i] = b;
    }
}
