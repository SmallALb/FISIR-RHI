#pragma once
// NaniteBuilder.h —— 用 meshoptimizer + clusterlod 从三角形网格离线生成 Nanite 簇与层级 BVH。
//
// 输出两份字节块（与运行时读取方式逐字节一致）：
//   · BVH          -> ClusterSelection.hlsl 的 clusterSelectionBuffer (u0)
//   · nanitemesh   -> ClusterSelection.hlsl 的 clusterPagesBuffer (u1)
//
// ── 生成流程 ─────────────────────────────────────────────────────
//   1. clodBuild()（vendor/meshoptimizer/demo/clusterlod.h）聚簇 + 逐层合并简化，
//      产出 group DAG：每个 group 回调出若干簇，带保守包围球与累计简化误差；
//   2. 逐簇把顶点局部化（meshopt 的簇索引是原网格顶点号，而运行时按簇内局部序号寻址）；
//   3. 在簇之上建一棵 4 叉层级 BVH；每个叶槽打包一组连续簇，内部槽指向子节点；
//   4. 按「组」装箱到资源页：每页 ≤256 簇，且一个组完整落在一页内。
//
// ── BVH 节点（208 字节，4 叉转置 SoA）与 shader 的对应 ──────────────
//   [0,64)    LODBounds[4]        float4(包围球 center.xyz, radius)
//   [64,128)  Misc0[4]            xyz=BoxBoundsCenter(float3)，
//                                 w = MinLODError(ε_d) | MaxParentLODError(ε_{d+1})（两个 half）
//                                 ε_d   = 本槽这一层几何自身的误差（= 组内各簇 ownError 的最大值）
//                                 ε_{d+1} = 本组被简化成的那一版（更粗一级）的误差
//                                         = clodGroup::simplified.error（终端组 = +Inf）
//                                 运行时光栅化判据：ε_d ≤ T < ε_{d+1}（T = projScale/LodScale）
//                                 —— 逐簇做（簇自己的 LODError 参与左半边），DAG 多父下唯一自洽
//   [128,192) Misc1[4]            xyz=BoxBoundsExtent(float3)，w=ChildStartReference
//   [192,208) Misc2[4]            0xFFFFFFFF=内部节点 / 0=空槽 / 其他=叶子（低 8 位 = 该槽簇个数）
//
//   ChildStartReference 语义（NodePack 之后）：
//     · 叶子： (pageIndex << 8) | clusterOffset
//             —— 着色器读的是 StartPageIndex = csr >> 8、clusterOffset = csr & 0xFF，
//                且 clusterOffset 是「组首簇在本页偏移表里的下标」（不是字节偏移）。
//                offset 只有 8 位 ⇒ **每页簇数上限 256**。
//     · 内部： 子节点索引（节点按「父在前、子在后」分配，故根固定为 node 0）
//
// ── 簇记录（28 字节头，与 Res/mitsuba.nanitemesh 实测格式一致）──────
//   [0]  indexDataOffset  索引相对簇基址的字节偏移 = 28 + posCount*12
//   [4]  indexCount       索引总数（三角形列表）
//   [8]  LODBounds        float4（包围球 center.xyz + radius）
//   [24] LODError | EdgeLength（两个 half）
//   [28] positions        float3 × posCount（**簇内局部**顶点，索引 0..posCount-1）
//   [28+posCount*12] indices uint32 × indexCount
//
// ── 页表 ─────────────────────────────────────────────────────────
//   [0]              pageCount
//   [1..pageCount]   pageBase（各页起始字节偏移）
//   @pageBase        clusterCount（本页簇数）
//   @pageBase+4      clusterDataOffset × clusterCount（**相对本页簇数据区**的字节偏移）
//   @pageBase+4+4*N  本页簇数据区
//
// 关于分页：着色器里 ChildStartReference = (pageIndex << 8) | clusterOffset，
// clusterOffset 被 NANITE_CLUSTER_OFFSET_MASK(0xFF) 截到 8 位，所以真正的约束是
// **每页簇数 ≤ 256**，与簇数据的字节数无关（实测 mitsuba 的 page 8 有 118 簇、
// 跨 302456 字节 ≈ 295KB，page 1 有 100 簇）。页号在高 24 位，页数上限足够。
//
// !! 曾经的错误（导致 DEVICE_LOST，勿重犯）!!：以为低 16 位是页内字节偏移、每页 <64KB，
// 于是写成 (pageIndex << 16) | slotIndex。着色器右移 8 位就把 pageIndex<<8 当成了页号，
// page 1 被读成 256、page 2 被读成 512…… 直接读到缓冲外，簇记录全成垃圾，
// 派生出的 vertexCount 无界 ⇒ 853 条 DrawIndirect 时 GPU TDR。page 0 恰好读成 0，
// 所以现象是「部分正常、部分错乱」，极易误判。校验必须按着色器的方式解码读回，见文末。
//
// 另注：ClusterSelection.hlsl 的 EnableClusterList 是 [选中数][clusterID...] 的线性列表，
// 一帧选中总数受该缓冲容量限制（ClisterSelection.cpp 里按簇数分配，足够）。

#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <vector>

#include <meshoptimizer.h>
#include <clusterlod.h>

#include "Log/Logger.h"

namespace FISIR {

// float -> 位模式 uint32（把 float3 分量原样存入 uint32 字段，shader 侧 asfloat 读回）
static inline uint32_t floatToUint(float v) {
    uint32_t u; memcpy(&u, &v, sizeof(u)); return u;
}

// float -> half 位模式（BVH 里 LODError 以两个 half 打包进 Misc0.w）
static inline uint16_t floatToHalf(float v) {
    uint32_t f; memcpy(&f, &v, sizeof(f));
    const uint32_t sign = (f >> 16) & 0x8000u;
    int32_t exp = (int32_t)((f >> 23) & 0xFFu) - 127 + 15;
    uint32_t man = f & 0x7FFFFFu;

    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;              // 下溢 -> ±0
        man |= 0x800000u;                                  // 补回隐含的 1
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1u) h++;                // 就近舍入
        return (uint16_t)(sign | h);
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);      // 上溢 -> ±Inf
    uint32_t h = sign | ((uint32_t)exp << 10) | (man >> 13);
    if (man & 0x1000u) h++;                                // 就近舍入
    return (uint16_t)h;
}

// ── 与 ClusterSelection.hlsl 严格对应的常量 ──
constexpr uint32_t NANITE_BVH_FANOUT           = 4;   // 每节点 4 个子槽
constexpr uint32_t NANITE_BVH_NODE_SLICE_SIZE  = (4 + 4 + 4 + 1) * 4 * NANITE_BVH_FANOUT;   // 208
constexpr uint32_t NANITE_INTERNAL_NODE_MARKER = 0xFFFFFFFFu;
constexpr uint32_t NANITE_GROUP_PART_SIZE_MASK = 0xFFu;   // 叶槽低 8 位 = 簇个数
constexpr uint32_t NANITE_CLUSTER_OFFSET_MASK = 0xFFu;    // ChildStartReference 低 8 位 = 页内簇下标
                                                          // （与 ClusterSelection.hlsl 的宏同名同值）

// ── 槽 = 一个完整的 clusterlod group（LOD 判定的原子单位）──
// 判据「本层误差 ≤ 阈值 < 更粗一层误差」必须是**组一级原子**的：
//   每个 depth 上的 group 都是表面的一次完整划分，整组一起画 / 整组一起下降，
//   才能保证任何一个点恰好被一层覆盖。若按单簇判（一个粗簇覆盖多个误差不同的细簇），
//   选出来的既不是完整一层也不是完整分区 → 缺口 + 重叠（实测只覆盖 16.6%）。
// 所以建 BVH 时每个 group 只发一个「可画节点」，多父用「链接节点」指向它（见第 2 步）。
//
// 这里原本有 NANITE_GROUP_SLOTS / NANITE_MITSUBA_LIKE / NANITE_GROUP_PART_SIZE 三个宏在
// 「组槽」与「单簇槽的经典树」两种形态间切换。只有组槽这一种是正确的：经典树的内部节点
// **不持有任何簇**，唯一能原子画的单位是叶节点，而叶节点已是最细一层、判据恒成立 →
// 要么完全不做 LOD，要么没有子节点可降级而直接开洞。宏只会让人改错，已全部删除。


// 簇记录头固定 28 字节
constexpr uint32_t NANITE_CLUSTER_HEADER_BYTES = 28;

struct NaniteBuildResult {
    std::vector<uint8_t> bvh;    // BVH 字节块
    std::vector<uint8_t> mesh;   // nanitemesh 字节块（页表 + 簇数据）
    uint32_t clusterCount = 0;
    uint32_t totalBVHNodes = 0;
    uint32_t totalSlices = 0;
};

// ── 从平铺的 positions + 三角形索引生成 BVH 与 nanitemesh ──
static NaniteBuildResult BuildNaniteData(
    const std::vector<float>&    positions,       // xyz 平铺
    const std::vector<uint32_t>& indices,         // 三角形索引（3/面）
    uint32_t maxClusterTriangles = 128) {

    maxClusterTriangles = 126;   // 与 mitsuba 一致（其实测每簇 126 三角形）

    NaniteBuildResult result;
    const size_t vertexCount   = positions.size() / 3;
    const size_t triangleCount = indices.size() / 3;
    if (vertexCount == 0 || triangleCount == 0) {
        Error("[NaniteBuilder] Empty Mesh，Can Not Build Nanite Data");
        return result;
    }

    // ═══════════════════════════════════════════════════════════════
    // 1. clusterlod：聚簇 + 逐层合并简化
    // ═══════════════════════════════════════════════════════════════
    struct Cluster {
        std::vector<float>    positions;      // 簇内局部化顶点（float3 平铺）
        std::vector<uint32_t> indices;        // 簇内局部索引（三角形列表）
        meshopt_Bounds        bound{};        // 保守包围球
        float                 error = 0.f;    // 自身简化误差
        float                 lodError = 0.f; // 非分组槽发射路径用的层误差
        // ★ 两层界判据所需的一对误差（写进 Misc0.w / 簇记录的 LODError）★
        //   ownError    = 本簇自身那一层的误差 ε(C)（原始几何为 0；简化出来的 = 产生它的那次简化的误差）
        //   parentError = 本簇所属组被简化成的那一版（更粗一级）的误差 ε↑ = clodGroup::simplified.error
        float                 ownError = 0.f;
        float                 parentError = 0.f;
    };

    // 每个 group 的误差与 DAG 深度（clodBuild 保证「组先于由它简化出的簇」回调）
    struct GroupInfo { float error = 0.f; int depth = 0; };

    std::vector<Cluster>   clusters;
    std::vector<GroupInfo> groupInfo;
    // 由「组结束簇号」查该组的 simplified.error —— clusterlod 的 clodCluster::refined
    // 存的就是产生它的那个组的结束簇号（本 builder 的回调返回值），用它反查 ownError。
    std::vector<float>     groupEndError;

    // 每个 group 的簇区间与误差（一个 group = 一个槽）
    //   error = 本组被简化成的那一版（更粗一级）的误差 ε_{d+1}
    //           = clodGroup::simplified.error，也等于「本组所有簇的 parentError」
    //           —— 写进槽的 MaxParentLODError，是逐簇两层界判据的右界
    //   own   = 本层几何误差 ε_d（= 本组各簇 ownError 的最大值）—— 写进槽的 MinLODError
    struct GroupRec { uint32_t first = 0, count = 0; float error = 0.f; int depth = 0;
                      float own = 0.f; };
    std::vector<GroupRec>  recs;
    std::vector<int>       refinedOf;    // 逐簇的 clodCluster::refined
    // 父子关系（供可画 DAG 建树）：parentOf[g] = 父组号（根 = 0xFFFFFFFF）；childrenOf[g] = 子组列表
    std::vector<uint32_t>              parentOf;
    std::vector<std::vector<uint32_t>> childrenOf;

    clodConfig config = clodDefaultConfig(maxClusterTriangles);
    // 本 demo 只有位置属性，关掉需要 attribute_protect_mask 的 permissive 简化
    config.simplify_permissive = false;

    clodMesh input{};
    input.indices                 = indices.data();
    input.index_count             = indices.size();
    input.vertex_count            = vertexCount;
    input.vertex_positions        = positions.data();
    input.vertex_positions_stride = sizeof(float) * 3;

    // 逐簇顶点局部化：meshopt 的簇索引引用原网格顶点号，而 LoadVertexPosition(cb, i)
    // 按簇内局部序号寻址，所以这里做 全局号 -> 局部号 的重映射。
    // 用 stamp 数组标记「本簇是否已登记」，避免逐簇清空 O(顶点数) 的表。
    std::vector<uint32_t> localRemap;
    std::vector<uint32_t> localStamp;
    uint32_t localStampValue = 0;

    auto emitGroups = [&](clodGroup group, const clodCluster* groupClusters, size_t clusterCount) -> int {
        const uint32_t groupID = (uint32_t)groupInfo.size();
        groupInfo.push_back(GroupInfo{ group.simplified.error, group.depth });

        const uint32_t recFirst = (uint32_t)clusters.size();

        for (size_t i = 0; i < clusterCount; ++i) {
            const clodCluster& in = groupClusters[i];

            Cluster cl;
            cl.bound.center[0] = in.bounds.center[0];
            cl.bound.center[1] = in.bounds.center[1];
            cl.bound.center[2] = in.bounds.center[2];
            cl.bound.radius    = in.bounds.radius;
            cl.error           = in.bounds.error;
            cl.lodError        = in.bounds.error;
            // ownError：refined < 0 → 原始几何（误差 0）；否则查「产生它的那个组的误差」
            cl.ownError        = (in.refined < 0) ? 0.f
                               : ((size_t)in.refined < groupEndError.size() ? groupEndError[in.refined] : 0.f);
            // parentError：本组被简化成的那一版（更粗一级）的误差
            cl.parentError     = group.simplified.error;
            refinedOf.push_back(in.refined);

            if (localRemap.size() < vertexCount) {
                localRemap.assign(vertexCount, 0);
                localStamp.assign(vertexCount, 0);
            }
            if (++localStampValue == 0) ++localStampValue;   // 0 是「未登记」哨兵

            cl.indices.reserve(in.index_count);
            cl.positions.reserve(in.index_count * 3);
            for (size_t k = 0; k < in.index_count; ++k) {
                const uint32_t g = in.indices[k];
                if (localStamp[g] != localStampValue) {
                    localStamp[g] = localStampValue;
                    localRemap[g] = (uint32_t)(cl.positions.size() / 3);
                    cl.positions.push_back(positions[(size_t)g * 3 + 0]);
                    cl.positions.push_back(positions[(size_t)g * 3 + 1]);
                    cl.positions.push_back(positions[(size_t)g * 3 + 2]);
                }
                cl.indices.push_back(localRemap[g]);
            }
            clusters.push_back(std::move(cl));
        }
        // 记录「本组结束簇号 -> 本组的 simplified.error」，供下一层反查 ownError
        // （下一层簇的 clodCluster::refined 就是本回调的返回值 = 结束簇号）
        if (groupEndError.size() <= clusters.size()) groupEndError.resize(clusters.size() + 1, 0.f);
        groupEndError[clusters.size()] = group.simplified.error;
        recs.push_back(GroupRec{ recFirst, (uint32_t)clusters.size() - recFirst,
                                 group.simplified.error, group.depth });
        (void)groupID;
        return (int)clusters.size();
    };

    // clodBuild 的 C 重载需要函数指针，捕获 lambda 无法转换，故转发一层
    clodBuild(config, input, &emitGroups,
        [](void* ctx, clodGroup group, const clodCluster* groupClusters, size_t clusterCount) -> int {
            return (*static_cast<decltype(emitGroups)*>(ctx))(group, groupClusters, clusterCount);
        });

    if (clusters.empty()) {
        Error("[NaniteBuilder] clusterlod produced no clusters");
        return result;
    }

    // ── 计算每个 group 的 own（本层误差 ε_d）与簇↔组的 refined 映射 ──────────
    // 父组关系来自 clodCluster::refined：产生某个簇的那个组的结束簇号。
    // 该簇属于组 P，所以「源组」的父组就是 P。
    if ((uint32_t)refinedOf.size() < (uint32_t)clusters.size())
        refinedOf.resize(clusters.size(), -1);
    {
        // 结束簇号 -> 组号
        std::vector<uint32_t> groupByEnd(clusters.size() + 1, 0xFFFFFFFFu);
        for (uint32_t g = 0; g < (uint32_t)recs.size(); ++g)
            groupByEnd[recs[g].first + recs[g].count] = g;

        // own[g] = 组内各簇 ownError 的最大值 = 其源组 error 的最大值
        for (GroupRec& r : recs) {
            float m = 0.f;
            for (uint32_t k = 0; k < r.count; ++k) {
                const int ref = refinedOf[r.first + k];
                if (ref < 0) continue;                      // 原始几何：own = 0
                const uint32_t src = groupByEnd[(size_t)ref];
                if (src != 0xFFFFFFFFu) m = std::max(m, recs[src].error);
            }
            r.own = m;
        }

        // ── childrenOf / parentOf：逐边完整收集（DAG 多父）────────────────────
        // clusterlod 的组关系是 DAG，不是树：一个组被简化后产生的簇会被 clusterize 拆开、
        // 落进下一层**不同的组**里 → 一个组可以有**多个父组**，所以必须按边收完整子表，
        // 不能「记单值 parent 再反转」（那样会丢边，丢掉的父组就成了走不到更细层的死路）。
        // 每个组只有一个可画槽；多父通过「链接槽」指向同一个可画节点，簇数据不重复。
        childrenOf.assign(recs.size(), {});
        parentOf.assign(recs.size(), 0xFFFFFFFFu);
        for (uint32_t p = 0; p < (uint32_t)recs.size(); ++p) {
            for (uint32_t k = 0; k < recs[p].count; ++k) {
                const int ref = refinedOf[recs[p].first + k];
                if (ref < 0) continue;
                const uint32_t src = groupByEnd[(size_t)ref];
                if (src == 0xFFFFFFFFu || src == p) continue;
                parentOf[src] = p;
                auto& kids = childrenOf[p];
                if (std::find(kids.begin(), kids.end(), src) == kids.end()) kids.push_back(src);
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // 2. 建 4 叉可画 DAG
    //    节点「父在前、子在后」→ 根固定 node 0
    // ═══════════════════════════════════════════════════════════════
    struct TreeNode {
        bool     leaf = false;
        uint32_t begin = 0, count = 0;                      // 叶：簇区间
        uint32_t children[NANITE_BVH_FANOUT] = {0, 0, 0, 0};
        uint32_t childCount = 0;
        meshopt_Bounds bound{};
        float    error = 0.f;                               // 子树最大 LOD 误差
        float    aabbCenter[3] = {0.f, 0.f, 0.f};
        float    aabbExtent[3] = {0.f, 0.f, 0.f};
        // ── 可画 DAG 用的槽模型 ──
        // slotKind[c]: 0=空, 1=可画 group, 2=链接（下降到 slotChild[c]）
        // slotGroup[c]: 可画槽的 group 号；slotChild[c]: 可画槽的子节点 / 链接槽的目标节点
        uint32_t slotKind[4]  = {0, 0, 0, 0};
        uint32_t slotGroup[4] = {0, 0, 0, 0};
        uint32_t slotChild[4] = {0, 0, 0, 0};
    };

    // 合并一批簇的包围盒/包围球（可画槽发射用）
    std::vector<float>    scratchCenters, scratchRadii;

    auto mergeBounds = [&](uint32_t begin, uint32_t count, uint32_t stride,
                           float* outCenter, float* outExtent, meshopt_Bounds& outBound) {
        float mn[3] = { 1e30f,  1e30f,  1e30f };
        float mx[3] = {-1e30f, -1e30f, -1e30f };
        scratchCenters.resize((size_t)count * 3);
        scratchRadii.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const meshopt_Bounds& bnd = clusters[begin + (size_t)i * stride].bound;
            for (int k = 0; k < 3; ++k) {
                scratchCenters[i * 3 + k] = bnd.center[k];
                mn[k] = std::min(mn[k], bnd.center[k] - bnd.radius);
                mx[k] = std::max(mx[k], bnd.center[k] + bnd.radius);
            }
            scratchRadii[i] = bnd.radius;
        }
        for (int k = 0; k < 3; ++k) {
            outCenter[k] = (mn[k] + mx[k]) * 0.5f;
            outExtent[k] = (mx[k] - mn[k]) * 0.5f;
        }
        outBound = meshopt_computeSphereBounds(scratchCenters.data(), count,
                                               sizeof(float) * 3, scratchRadii.data(), sizeof(float));
    };

    std::vector<TreeNode> nodes;
    // ── 可画 DAG：节点 = 4 槽，槽可以是「可画 group」或「链接」──────
    //   可画槽：一个完整 group，ChildStartReference 的 bit[16,32) 存其子节点号（0 = 最细层）
    //   链接槽：Misc2 = 0xFFFFFFFF，ChildStartReference 高 16 位 = 目标节点
    //   根（没有父组的那一个 group）的节点 = node 0。
    {
        // 每个组有一个**唯一的可画节点**（slot 0 = 该组），多父通过**链接节点**指向它 ——
        // 于是「一个组的簇数据只发一次」（不会重复覆盖），而下降的边是完整的（不会漏区域）。
        std::vector<uint32_t> drawNode(recs.size(), 0xFFFFFFFFu);

        std::function<uint32_t(uint32_t)> ensureDrawNode;
        std::function<uint32_t(const std::vector<uint32_t>&)> buildLinkTree;

        // 一组链接槽组成的 4 叉子树；叶子槽指向各子组的可画节点
        buildLinkTree = [&](const std::vector<uint32_t>& kids) -> uint32_t {
            const uint32_t idx = (uint32_t)nodes.size();
            nodes.emplace_back();
            if (kids.size() <= NANITE_BVH_FANOUT) {
                for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                    nodes[idx].slotKind[i]  = 2;
                    nodes[idx].slotChild[i] = ensureDrawNode(kids[i]);
                }
            } else {
                for (uint32_t s = 0; s < NANITE_BVH_FANOUT; ++s) {
                    std::vector<uint32_t> chunk;
                    for (uint32_t i = s; i < (uint32_t)kids.size(); i += NANITE_BVH_FANOUT)
                        chunk.push_back(kids[i]);
                    if (chunk.empty()) continue;
                    nodes[idx].slotKind[s]  = 2;
                    nodes[idx].slotChild[s] = buildLinkTree(chunk);
                }
            }
            return idx;
        };

        ensureDrawNode = [&](uint32_t g) -> uint32_t {
            if (drawNode[g] != 0xFFFFFFFFu) return drawNode[g];
            const uint32_t idx = (uint32_t)nodes.size();
            nodes.emplace_back();
            drawNode[g] = idx;
            nodes[idx].slotKind[0]  = 1;                 // 可画槽 = 该组
            nodes[idx].slotGroup[0] = g;
            nodes[idx].slotChild[0] = childrenOf[g].empty() ? 0u : buildLinkTree(childrenOf[g]);
            return idx;
        };

        // 根 = 没有父组的那个组；先建它，保证 root 的可画节点 = node 0
        uint32_t root = 0xFFFFFFFFu;
        for (uint32_t g = 0; g < (uint32_t)recs.size(); ++g)
            if (parentOf[g] == 0xFFFFFFFFu) { root = g; break; }
        if (root != 0xFFFFFFFFu) ensureDrawNode(root);
    }

    // 注意：这里要用 clusters.size()，不能在 result.clusterCount 赋值之前读它（那是 0）
    const uint32_t clusterCount = (uint32_t)clusters.size();

    auto clusterBytes = [&](uint32_t i) -> uint64_t {
        const Cluster& cl = clusters[i];
        return (uint64_t)NANITE_CLUSTER_HEADER_BYTES
             + (uint64_t)(cl.positions.size() / 3) * 12
             + (uint64_t)cl.indices.size() * 4;
    };

    // ── 资源页打包 ──
    // shader 取一组簇是：for (i < NumChildren) ProcessCluster(pageIndex, clusterOffset + i)，
    // 而 clusterOffset 是拿去索引**本页偏移表**的下标
    // （Load(pageBase + 4 + 4*clusterOffset)）。因此硬约束是：
    //
    //   **一个叶槽引用的一组簇，必须全部落在同一页的偏移表里，且下标连续。**
    //
    // 另外 clusterOffset 被 NANITE_CLUSTER_OFFSET_MASK(0xFF) 截到 8 位，所以
    // **每页簇数 ≤ 256**（与字节数无关，见文件头「关于分页」）。做法：以「组」为单位装箱，
    // 一组完整落在一页内 → 天然满足上面两条约束。
    //
    // 必须放在 buildNode 之后 —— 树会把 clusters[] 重排，叶组的簇区间在重排后才最终确定。
    static constexpr uint32_t kPageClusterLimit = 256u;   // 8 位页内簇下标上限

    // 收集所有叶槽组（按簇号递增顺序，与 shader 遍历叶槽的顺序一致）
    struct ClusterGroupRef { uint32_t begin, count; };
    std::vector<ClusterGroupRef> groupsInOrder;
    // 一个 group 一个槽；groupsInOrder 下标 == group 号，emit 时直接用它索引
    for (const GroupRec& r : recs) groupsInOrder.push_back(ClusterGroupRef{ r.first, r.count });

    // 以「组」为单位装箱：一组完整落在一页内，且每页簇数 ≤ kPageClusterLimit
    std::vector<uint32_t> groupPage(groupsInOrder.size(), 0);
    std::vector<std::vector<uint32_t>> pageGroups;
    {
        uint32_t page = 0;
        uint32_t used = 0;
        pageGroups.emplace_back();
        for (uint32_t g = 0; g < (uint32_t)groupsInOrder.size(); ++g) {
            const uint32_t cnt = groupsInOrder[g].count;
            if (cnt > NANITE_GROUP_PART_SIZE_MASK)
                Warn("[NaniteBuilder] group {} has {} clusters > 255: shader NumChildren is only 8 bits", g, cnt);
            if (used > 0 && used + cnt > kPageClusterLimit) {
                ++page;
                used = 0;
                pageGroups.emplace_back();
            }
            groupPage[g] = page;
            used += cnt;
            pageGroups[page].push_back(g);
        }
    }
    const uint32_t pageCount = (uint32_t)pageGroups.size();

    // 每页的簇列表（组内簇号连续）
    std::vector<std::vector<uint32_t>> pageClusters(pageCount);
    for (uint32_t p = 0; p < pageCount; ++p)
        for (uint32_t g : pageGroups[p])
            for (uint32_t k = 0; k < groupsInOrder[g].count; ++k)
                pageClusters[p].push_back(groupsInOrder[g].begin + k);

    // 簇号 -> 页号；簇号 -> 在本页偏移表里的**下标**（不是字节偏移！）
    // 页内偏移表按「簇在本页内的序号」排列，表里存的才是字节偏移。
    std::vector<uint32_t> clusterSlotIndex(clusterCount, 0);
    for (uint32_t p = 0; p < pageCount; ++p)
        for (uint32_t k = 0; k < (uint32_t)pageClusters[p].size(); ++k)
            clusterSlotIndex[pageClusters[p][k]] = k;

    // 每个叶槽组在页内的起始**下标**（= 组首簇的下标），供 BVH 叶槽写 ChildStartReference
    std::vector<uint32_t> groupSlotIndex(groupsInOrder.size(), 0);
    for (uint32_t g = 0; g < (uint32_t)groupsInOrder.size(); ++g)
        groupSlotIndex[g] = clusterSlotIndex[groupsInOrder[g].begin];

    // 簇号 -> 所属组下标
    std::vector<uint32_t> groupIndexOfCluster(clusterCount, 0);
    for (uint32_t g = 0; g < (uint32_t)groupsInOrder.size(); ++g)
        for (uint32_t k = 0; k < groupsInOrder[g].count; ++k)
            groupIndexOfCluster[groupsInOrder[g].begin + k] = g;

    // ═══════════════════════════════════════════════════════════════
    // 3. 序列化 BVH
    // ═══════════════════════════════════════════════════════════════
    const uint32_t nodeCount = (uint32_t)nodes.size();
    result.clusterCount  = clusterCount;
    result.totalBVHNodes = nodeCount;
    result.totalSlices   = nodeCount * NANITE_BVH_FANOUT;
    result.bvh.assign((size_t)nodeCount * NANITE_BVH_NODE_SLICE_SIZE, 0);

    for (uint32_t n = 0; n < nodeCount; ++n) {
        const TreeNode& node = nodes[n];
        uint8_t* base   = result.bvh.data() + (size_t)n * NANITE_BVH_NODE_SLICE_SIZE;
        float*    lod   = reinterpret_cast<float*>(base);
        uint32_t* misc0 = reinterpret_cast<uint32_t*>(base + 64);
        uint32_t* misc1 = reinterpret_cast<uint32_t*>(base + 128);
        uint32_t* misc2 = reinterpret_cast<uint32_t*>(base + 192);

        // 可画 DAG：按槽类型发射
        for (uint32_t c = 0; c < NANITE_BVH_FANOUT; ++c) {
            const uint32_t kind = node.slotKind[c];
            if (kind == 0u) { misc2[c] = 0u; continue; }
            if (kind == 2u) {   // 链接槽：下降到目标节点
                misc2[c] = NANITE_INTERNAL_NODE_MARKER;
                misc1[c * 4 + 3] = node.slotChild[c] << 16;
                // 链接槽**也要发射目标节点的包围球**（与下面内部节点槽同样的写法）。
                // 少了这几行，着色器读到的就是 原点+半径 0 的退化球 —— 用它做子树剔除会把
                // 「原点这一个点」当成整棵子树，原点一旦在屏幕外/被挡，整棵树就被剔掉。
                const TreeNode& linkTarget = nodes[node.slotChild[c]];
                lod[c * 4 + 0] = linkTarget.bound.center[0];
                lod[c * 4 + 1] = linkTarget.bound.center[1];
                lod[c * 4 + 2] = linkTarget.bound.center[2];
                lod[c * 4 + 3] = linkTarget.bound.radius;

                misc0[c * 4 + 0] = floatToUint(linkTarget.aabbCenter[0]);
                misc0[c * 4 + 1] = floatToUint(linkTarget.aabbCenter[1]);
                misc0[c * 4 + 2] = floatToUint(linkTarget.aabbCenter[2]);
                misc0[c * 4 + 3] = (uint32_t)floatToHalf(0.0f) | ((uint32_t)floatToHalf(linkTarget.error) << 16);

                misc1[c * 4 + 0] = floatToUint(linkTarget.aabbExtent[0]);
                misc1[c * 4 + 1] = floatToUint(linkTarget.aabbExtent[1]);
                misc1[c * 4 + 2] = floatToUint(linkTarget.aabbExtent[2]);
                continue;
            }
            // 可画槽：一个完整 group；bit[16,32) 存子节点号（0 = 最细层，无子）
            const uint32_t g = node.slotGroup[c];
            const uint32_t groupStart = recs[g].first;
            const uint32_t parts      = recs[g].count;

            float center[3], extent[3];
            meshopt_Bounds bound{};
            mergeBounds(groupStart, parts, 1, center, extent, bound);

            lod[c * 4 + 0] = bound.center[0];
            lod[c * 4 + 1] = bound.center[1];
            lod[c * 4 + 2] = bound.center[2];
            lod[c * 4 + 3] = bound.radius;

            misc0[c * 4 + 0] = floatToUint(center[0]);
            misc0[c * 4 + 1] = floatToUint(center[1]);
            misc0[c * 4 + 2] = floatToUint(center[2]);
            // 低 half = MinLODError      = 本组几何自身的误差 ε_d
            // 高 half = MaxParentLODError = 本组被简化成的那一版（更粗一级）的误差 ε_{d+1}
            //                              = clodGroup::simplified.error（clusterlod 文档里
            //                              「the group it's in」对应的那个值）
            // 运行时逐簇判据（见 ClusterSelection.hlsl）：ε_d ≤ T < ε_{d+1}，
            // 即「本层够精细，且更粗一层还不够精细」——DAG 多父结构下唯一自洽的判据。
            // 终端组 ε_{d+1} = FLT_MAX → half 溢出成 +Inf → 右半边恒成立（最粗一层总能画）。
            misc0[c * 4 + 3] = (uint32_t)floatToHalf(recs[g].own) | ((uint32_t)floatToHalf(recs[g].error) << 16);

            misc1[c * 4 + 0] = floatToUint(extent[0]);
            misc1[c * 4 + 1] = floatToUint(extent[1]);
            misc1[c * 4 + 2] = floatToUint(extent[2]);
            // ChildStartReference = (childNode << 16) | (pageIndex << 8) | clusterOffset
            misc1[c * 4 + 3] = (node.slotChild[c] << 16) | (groupPage[g] << 8) | groupSlotIndex[g];

            misc2[c] = parts;   // shader 的 NumChildren
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // 4. 序列化 nanitemesh（多页）
    //    clusterSlotIndex / groupPage / pageCount 已在 BVH 序列化前算好。
    // ═══════════════════════════════════════════════════════════════
    const uint32_t pageTableEnd = (1 + pageCount) * 4;

    // 每页的簇列表 pageClusters 已在装箱阶段构建好；这里只算基址与写数据
    std::vector<uint32_t> pageBase(pageCount, 0);
    {
        uint32_t cursor = pageTableEnd;
        for (uint32_t p = 0; p < pageCount; ++p) {
            pageBase[p] = cursor;
            cursor += (1 + (uint32_t)pageClusters[p].size()) * 4;
            for (uint32_t ci : pageClusters[p]) cursor += (uint32_t)clusterBytes(ci);
        }
        result.mesh.assign(cursor, 0);
    }

    uint32_t* meshWords = reinterpret_cast<uint32_t*>(result.mesh.data());
    meshWords[0] = pageCount;
    for (uint32_t p = 0; p < pageCount; ++p) meshWords[1 + p] = pageBase[p];

    for (uint32_t p = 0; p < pageCount; ++p) {
        const std::vector<uint32_t>& list = pageClusters[p];
        const uint32_t dataRegion = pageBase[p] + (1 + (uint32_t)list.size()) * 4;

        // 页头：簇数 + 每簇相对簇数据区的字节偏移
        uint32_t* page = meshWords + pageBase[p] / 4;
        page[0] = (uint32_t)list.size();
        uint32_t running = 0;
        for (uint32_t k = 0; k < list.size(); ++k) {
            page[1 + k] = running;
            running += (uint32_t)clusterBytes(list[k]);
        }

        for (uint32_t k = 0; k < list.size(); ++k) {
            const uint32_t i = list[k];
            const Cluster& cl = clusters[i];
            const uint32_t posCount = (uint32_t)(cl.positions.size() / 3);
            const uint32_t ic       = (uint32_t)cl.indices.size();
            uint8_t* cb = result.mesh.data() + dataRegion + page[1 + k];
            uint32_t* cw = reinterpret_cast<uint32_t*>(cb);

            // indexDataOffset：索引相对簇基址的偏移；shader 用 (offset - 28)/12 反推 posCount
            const uint32_t indexDataOffset = NANITE_CLUSTER_HEADER_BYTES + posCount * 12;
            cw[0] = indexDataOffset;
            cw[1] = ic;
            memcpy(cb + 8, cl.bound.center, sizeof(float) * 3);
            memcpy(cb + 20, &cl.bound.radius, sizeof(float));
            // LODError | EdgeLength（两个 half）。EdgeLength 暂未使用，置 0。
            // 低 half = LODError（**本簇自身的几何误差**，与组槽的 MinLODError 同源：
            // 组槽的 MinLODError = 组内各簇 ownError 的最大值）；高 half = EdgeLength（暂未使用）。
            // UE 的 SmallEnoughToDraw 就是拿簇自己的 LODError 做最后一道剔除，
            // 并用 NANITE_CLUSTER_FLAG_STREAMING_LEAF（最细层）豁免，避免打洞。
            // 这里原始几何的 ownError = 0，正好充当那个豁免条件。
            cw[6] = (uint32_t)floatToHalf(cl.ownError) | ((uint32_t)floatToHalf(0.0f) << 16);

            memcpy(cb + NANITE_CLUSTER_HEADER_BYTES, cl.positions.data(), (size_t)posCount * 12);
            memcpy(cb + indexDataOffset, cl.indices.data(), (size_t)ic * 4);
        }
    }

    Info("[NaniteBuilder] {} Clusters / {} BVH Nodes / {} Slices / {} Pages | BVH {} B, mesh {} B",
        result.clusterCount, result.totalBVHNodes, result.totalSlices, pageCount,
        (uint32_t)result.bvh.size(), (uint32_t)result.mesh.size());

    // ═══ 端到端自检：完全按着色器的解码方式把两个缓冲读回来验一遍 ═══
    // 关键：**不能用「同一个公式重算再比对」**——那只是把写入公式抄第二遍，编码位宽错了
    // （例如把 pageIndex 写到 bit16 而 shader 读 >>8）也照样通过。这里改为按 shader 的
    // 位域与寻址方式解码自己写出的字节，再验证解出的簇记录真的落在页内、顶点/索引自洽，
    // 并确认所有簇都被叶槽覆盖到（checked == clusterCount）。
    {
        bool ok = true;
        uint32_t checked = 0;
        for (uint32_t n = 0; n < nodeCount && ok; ++n) {
            // 可画 DAG：逐槽解码（每个槽要么是可画 group、要么是链接/空）
            const uint8_t* base = result.bvh.data() + (size_t)n * NANITE_BVH_NODE_SLICE_SIZE;
            const uint32_t* misc1 = reinterpret_cast<const uint32_t*>(base + 128);   // [128,192)
            const uint32_t* misc2 = reinterpret_cast<const uint32_t*>(base + 192);   // [192,208)
            for (uint32_t c = 0; c < NANITE_BVH_FANOUT; ++c) {
                const uint32_t m2 = misc2[c];
                if (m2 == 0u || m2 == NANITE_INTERNAL_NODE_MARKER) continue;   // 空槽 / 链接槽

                // ↓↓↓ 这三行就是 shader 的 UnPackHierarchyNodeSlice / ProcessLeafCluster ↓↓↓
                // 注意 ChildStartReference 在 Misc1 数组里是第 c*4+3 个字（+128+16c+12），
                // 不是 misc1[c]；misc1[c*4+0..2] 是 BoxBoundsExtent。
                const uint32_t ref  = misc1[c * 4 + 3];
                const uint32_t num  = m2 & NANITE_GROUP_PART_SIZE_MASK;        // Res.NumChildren
                const uint32_t page = (ref >> 8) & NANITE_CLUSTER_OFFSET_MASK;  // Res.StartPageIndex（[8,16)）
                const uint32_t off  = ref & NANITE_CLUSTER_OFFSET_MASK;         // clusterOffset
                // ↑↑↑ 越界就说明编码错了，shader 会读到缓冲外 ↑↑↑

                if (num == 0u || page >= pageCount || off + num > (uint32_t)pageClusters[page].size()) {
                    Warn("[NaniteBuilder] self-check failed: node {} slot {}: page={} off={} num={} pages={} clustersInPage={}",
                         n, c, page, off, num, pageCount,
                         page < pageCount ? (uint32_t)pageClusters[page].size() : 0u);
                    ok = false;
                    break;
                }
                for (uint32_t i = 0; i < num && ok; ++i) {
                    const uint32_t pb = pageBase[page];
                    const uint32_t cc = meshWords[pb / 4];                 // 本页 clusterCount
                    const uint32_t cb = pb + 4 + cc * 4 + meshWords[pb / 4 + 1 + off + i];
                    const uint32_t ido = meshWords[cb / 4];
                    const uint32_t ic  = meshWords[cb / 4 + 1];
                    const uint32_t posCount = (ido - NANITE_CLUSTER_HEADER_BYTES) / 12;
                    if (ido != NANITE_CLUSTER_HEADER_BYTES + posCount * 12 || ic % 3u != 0u ||
                        (size_t)cb + ido + (size_t)ic * 4 > result.mesh.size()) {
                        Warn("[NaniteBuilder] self-check failed: cluster record page={} slot={} ido={} ic={}", page, off + i, ido, ic);
                        ok = false;
                        break;
                    }
                    for (uint32_t t = 0; t < ic; ++t) {
                        const uint32_t idx = meshWords[(cb + ido) / 4 + t];
                        if (idx >= posCount) {
                            Warn("[NaniteBuilder] self-check failed: index out of range page={} slot={} idx={} posCount={}",
                                 page, off + i, idx, posCount);
                            ok = false;
                            break;
                        }
                    }
                    if (ok) ++checked;
                }
            }
        }
        if (ok && checked != clusterCount) { ok = false; Warn("[NaniteBuilder] self-check failed: leaf slots cover only {} / {} clusters", checked, clusterCount); }
        if (ok) Info("[NaniteBuilder] end-to-end self-check passed: {} leaf slots decode to all {} clusters, in-page, vertices/indices consistent",
                     groupsInOrder.size(), checked);
        else     Warn("[NaniteBuilder] end-to-end self-check FAILED - the shader would read out of bounds, result unusable");
    }

    return result;
}

} // namespace FISIR
