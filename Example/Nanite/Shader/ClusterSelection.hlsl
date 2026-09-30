
#define NANITE_BVH_NODE_FANOUT_BITS 2
#define NANITE_BVH_NODE_FANOUT_MASK ((1<<NANITE_BVH_NODE_FANOUT_BITS)-1)
#define NANITE_BVH_NODE_FANOUT      ((1<<NANITE_BVH_NODE_FANOUT_BITS))
#define HIERARCHY_NODE_SLICE_SIZE ((4+4+4+1)*4*NANITE_BVH_NODE_FANOUT)

// 槽的 Misc2（槽信息）位布局：
//   [0,8)   本槽包含的簇数量（NumChildren；0 同时表示空槽，所以可画槽至少 1 个簇）
//   [8,32)  保留（本 demo 未使用；簇的 page/offset 编码在 ChildStartReference 中）
//   0xFFFFFFFF -> 链接槽（下降到 ChildStartReference 高 16 位指向的节点）；0 -> 空槽位
#define NANITE_GROUP_PART_SIZE_MASK  0xFFu
#define NANITE_INTERNAL_NODE_MARKER  0xFFFFFFFFu

// 叶子节点 ChildStartReference 位布局：
//   [0,8)   clusterOffset：簇在资源页内的偏移
//   [8,32)  pageIndex：资源页索引
#define NANITE_CLUSTER_OFFSET_MASK   0xFFu


RWByteAddressBuffer 		clusterSelectionBuffer : register(u0);
RWByteAddressBuffer     clusterPagesBuffer : register(u1);
struct ClusterData {
  uint indexCount;
	uint baseVertex;
	uint baseIndex;
	uint padding;
};
RWStructuredBuffer<ClusterData> clusterDataBuffer : register(u2);
// 软光栅列表：[0] = 簇数，[1] = 三角形数（同时是三角形工作列表的分配计数器），[2..] = clusterID
RWStructuredBuffer<uint> EnableClusterListSw : register(u3);
cbuffer InputData :register(b4){
  float4 Position;    // xyz + pad（std140 下 float3 被填充到 16 字节）
	float4 Direction;   // xyz + pad
	float	 LodScale;
	float  ZNear;
	uint   CountOfClusters;
	uint 	 TotalBVHNodes;      
	uint 	 TotalSlices;        
	uint 	 MaxClusters;        
	// 软/硬光栅分配阈值：见 ShouldUseSoftwareRaster
	float  SwRasterThreshold;
	// 整个层次里最细一层的 LOD 误差（CPU 侧扫 BVH 得到）。给误差预算兜底用，见 GetProjectionScales。
	float  MinLODError;
};

struct HierarchyNodeSlice {
	float4 LODBounds;
	float4 BoxBoundsCenter;   // xyz = AABB 中心，w = 填充（float3 在结构化缓冲里也按 16 字节对齐，显式 float4 避免 Nsight 错位）
	float4 BoxBoundsExtent;   // xyz = AABB 半尺寸，w = 填充
	float  MinLODError;
	float  MaxParentLODError;
	uint   ChildStartReference;
	uint   NumChildren;
	uint   StartPageIndex;
	uint   NumPages;
	uint   bEnabled;
	uint   bLoaded;
	uint   bLeaf;
	uint   _Pad0;   // 显式填充：字段合计 96 字节，与 SPIR-V 数组步长（16 字节对齐）一致，
	uint   _Pad1;   // 否则 DXC 按 96 字节步长写、Nsight 按 84 字节读，错位每 8 行累积一次
	uint   _Pad2;
};

// 间接绘制缓冲：ProcessCluster 为每个选中的簇写一条 VkDrawIndirectCommand
// （16 字节：vertexCount/instanceCount/firstVertex/firstInstance）。
// 硬光栅据此派发 DrawIndirect，无需 CPU 回读选中结果。
struct DrawIndirectArgs {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};
RWStructuredBuffer<DrawIndirectArgs> IndirectDrawBuffer : register(u5);
// 硬光栅专用列表（软光栅那张是 u3 的 EnableClusterListSw）
RWStructuredBuffer<uint> EnableClusterListHw : register(u6);
// 软光栅的**三角形工作列表**：由本 pass 展开，软光栅一个线程画一个三角形。
// 每个三角形 3 个 uint：(clusterID, 簇数据区字节偏移 cb, 簇内三角形序号)。
// 容量 = g_ClusterCount × NANITE_MAX_CLUSTER_TRIANGLES（C++ 侧 ClusterSelection.h 同名常量，必须一致）。
RWStructuredBuffer<uint> SwTriList : register(u7);
#define NANITE_MAX_CLUSTER_TRIANGLES 128u

// ── HZB 遮挡剔除用的输入 ────────────────────────────────────────
// b8 = RenderParams（复用渲染那块，取 VPMatrix + screenSize 做屏幕投影）；
// u9 = HZB 深度金字塔（L0 是上一帧深度，L1..L7 由 HZBBuild.hlsl 取 min 建出）。
cbuffer RenderParams : register(b8) {
	float4x4 VPMatrix;
	float2   screenSize;
	float    ClearDepth;
	uint     ClearColor;
	uint     HzbColorBlockUnused;
	float    FarPlane;
};
RWByteAddressBuffer HZB : register(u9);
#define NANITE_HZB_MAX_LEVEL 7

// ── BVH 下降用的无锁队列（GPU 写、GPU 读）与去重表 ─────────────────────
// 队列布局（每份）：[0] = 本层条目数（写入端原子加 / 读取端读它），[1..] = 条目（节点下标）。
// 两份交替（ping-pong）：主机知道轮次的奇偶，所以用两个资源包换绑 in/out 即可，
// 不需要 push constant 去传轮次。
// 队列由**上一层**写、本层读；页按录制顺序提交 ⇒ 轮次有序。
#define NANITE_QUEUE_HEADER 1u
RWStructuredBuffer<uint> BvhQueueIn  : register(u10);
RWStructuredBuffer<uint> BvhQueueOut : register(u11);
// 去重表：这个 DAG 是**多父**的（同一个组会被多条路径到达），不判重就会重复入队 → 重复绘制。
// [0] = 本帧帧号（主机每帧 +1），[1 + node] = 最近一次访问它的帧号；命中即已访问（不需要每帧清缓冲）。
RWStructuredBuffer<uint> BvhVisited  : register(u12);

HierarchyNodeSlice UnPackHierarchyNodeSlice(uint4 RawData0, uint4 RawData1, uint4 RawData2, uint RawData3) {
	const uint4 Misc0 = RawData1;   // BoxBoundsCenter.xyz + MinLODError_MaxParentLODError（两个 half 打包）
	const uint4 Misc1 = RawData2;   // BoxBoundsExtent.xyz + ChildStartReference
	const uint  Misc2 = RawData3;   // 组信息：空 / 内部 / 叶子

	HierarchyNodeSlice Res = (HierarchyNodeSlice)0;
	Res.LODBounds = asfloat(RawData0);
	Res.BoxBoundsCenter = float4(asfloat(Misc0.xyz), 0.0);
	Res.BoxBoundsExtent = float4(asfloat(Misc1.xyz), 0.0);

	// LOD 误差打包为两个 half：低 16 位 = MinLODError（本槽这一层自身的误差 ε_d），
	// 高 16 位 = MaxParentLODError（本组被简化成的那一版、更粗一级的误差 ε_{d+1}
	// = clodGroup::simplified.error；最粗的终端组为 +Inf，表示「没有更粗的一级」）
	Res.MinLODError = f16tof32(Misc0.w);
	Res.MaxParentLODError = f16tof32(Misc0.w >> 16);

	Res.ChildStartReference = Misc1.w;

	// 叶子 / 内部 / 空 判定（内部节点 Misc2 全 1，空槽位 Misc2 为 0）
	Res.bLeaf = ((Misc2 != NANITE_INTERNAL_NODE_MARKER) && (Misc2 != 0u)) ? 1u : 0u;
	Res.bEnabled = (Misc2 != 0u) ? 1u : 0u;
	Res.bLoaded = 1u;   // mitsuba 场景一次性全部加载，无按需流式，恒为已加载

	// 叶子：簇数量（低 8 位）；内部 / 空节点此值无意义
	Res.NumChildren = Misc2 & NANITE_GROUP_PART_SIZE_MASK;

	// 叶子：簇的寻址信息来自 ChildStartReference = (childNode << 16) | (pageIndex << 8) | clusterOffset
	Res.StartPageIndex = (Misc1.w >> 8) & 0xFFu;
	Res.NumPages = 1;   // 本格式每个组都落在单个资源页内，无跨页组

	return Res;
}

HierarchyNodeSlice GetHierarchyNodeSlice(RWByteAddressBuffer hierarchyNodeBuffer, uint Index, uint ChildIndex) {
	const uint BaseAddress = Index * HIERARCHY_NODE_SLICE_SIZE;

	const uint4 RawData0 = hierarchyNodeBuffer.Load4(BaseAddress + 16 * ChildIndex);
	const uint4 RawData1 = hierarchyNodeBuffer.Load4(BaseAddress + (NANITE_BVH_NODE_FANOUT * 16) + 16 * ChildIndex);
	const uint4 RawData2 = hierarchyNodeBuffer.Load4(BaseAddress + (NANITE_BVH_NODE_FANOUT * 32) + 16 * ChildIndex);
	const uint  RawData3 = hierarchyNodeBuffer.Load (BaseAddress + (NANITE_BVH_NODE_FANOUT * 48) + 4 * ChildIndex);

	HierarchyNodeSlice Res = UnPackHierarchyNodeSlice(RawData0, RawData1, RawData2, RawData3);
	return Res;
}

// 返回「投影尺度」：近似等于相机到该簇最近点的距离，于是世界空间误差预算
// T = projScale / LodScale 随距离增大（远 → 允许更粗）。
//
// 相机逼近/进入包围球时这个量会失效，必须兜住，否则靠近模型时判据整体崩掉：
//   · tS = d² - r² < 0（相机在球内）→ 直接开方出 NaN，而 NaN 参与的比较**全部为 false**，
//     「ε(C) ≤ T」恒不成立 → 簇被静默丢掉，连组的提前返回也跟着失效（UE5 同名函数
//     在这里写的是 sqrt(max(tS, 0))）；
//   · 相机贴近球面时 t → 0，离轴簇的 y 甚至变成负数 → projScale 塌到 0/负，T = 0
//     等于要求零误差，任何簇都不满足 → 近处几何整片消失，屏幕上只剩更远、更粗的簇
//     （它们占屏巨大 → 逐像素原子解算的 overdraw 爆炸 → 掉帧）。
// 兜底：预算永远不低于「最细一层的误差」，于是相机再近也总能画出最细那层；
// 更粗的层仍然被左界 ε(C) ≤ T 挡住。MinLODError 为 0（扫描失败）时退化为原行为。
float GetProjectionScales(float3 viewDir, float3 toCenter, float radius) {
	float DisToClusterS = dot(toCenter, toCenter);
	float z = dot(viewDir, toCenter);
	float xS = DisToClusterS - z * z;
	float tS = DisToClusterS - radius * radius;
	const float budgetFloor = LodScale * MinLODError;   // T 的下界（换算回 projScale 尺度）
	float t = sqrt(max(tS, 0.0f));
	float x = sqrt(max(xS, 0.0f));
	float DisToCluster = sqrt(DisToClusterS);
	
	if (DisToCluster < 0.00001f) return budgetFloor;

	float Cos = t / DisToCluster;
	float Sin = radius / DisToCluster;

	float y = (-Sin*x + Cos*z) / DisToCluster;
	float MinZ = max(z - radius,  ZNear);
	if (z + radius >  ZNear) return max(y * MinZ, budgetFloor);
	return 0.0f;   // 整个簇在近平面之后：不画（这里不能兜底，否则会把身后的簇也画出来）
}

struct ClusterPayload {
    float4 LODBounds;      // 包围球 (xyz=中心, w=半径)
    float  LODError;       // LOD 误差
    float  EdgeLength;     // 边长度 (暂未使用)
    uint   PositionCount;  // 顶点数量
    uint   IndexCount;     // 索引数量
	uint   BaseVertex;     
    uint   BaseIndex;      
    uint   BaseAddress;    // 簇数据区在页缓冲里的字节偏移 cb（软光栅三角形列表要用）
};

// ── 选择在哪条光栅路径上画 ──────────────────────────────────────
// 判据：本簇的**角尺寸**（半径/投影尺度，单位弧度；越小说明它在屏幕上越小）小于阈值 →
// 交给软光栅（compute 软件光栅化）。阈值放在 InputData 里，不用重编着色器就能整体偏向一侧。
//
// ⚠ 注意这个量的单位是**弧度而不是像素**：直径 ≈ 2 × 阈值 × (screenH/2) / tan(fov/2)，
// 本工程（1041px / 60°）下约等于 1800 × 阈值。软光栅的 RasterizeTriangle 是逐像素重心
// 解算 + 两次全局原子，单位像素成本远高于硬件光栅，所以阈值必须开得很小才有意义 ——
// 实测把 0.08（≈144px）的簇交给它，mitsuba 相机推到 z=200 时 12 簇/1514 三角形就要 60ms。
bool ShouldUseSoftwareRaster(float radius, float projScale) {
	return (radius / max(projScale, 1e-6f)) < SwRasterThreshold;
}

// ══════════════════════════════════════════════════════════════════
// 包围球剔除：返回 true = 可以整块跳过（视锥外 或 被上一帧深度遮挡）
//
// 两处共用同一个函数：
//   · BVH 下降时判 link 槽 → 通过就**整棵子树不入队**（这才是真 BVH 省下来的东西）；
//   · 逐簇判定时判可画簇。
//
// 遮挡部分与 Nanite/UE 一致：把球投到屏幕得到像素矩形，选一层让**一个 texel 覆盖整个矩形**
// （该 texel 是该矩形超集上的 min 深度，偏保守），拿它和球的最近点深度比：最近点都比遮挡物
// 远 ⇒ 整块被挡住。一个 texel 判定一个球，只读 1 次。
//
// 保守性：球心在相机后方/贴近平面的不放行剔除；深度比较留 kHzbDepthBias 余量；
// 用的是上一帧深度，相机快速前推时新露出的面可能被误剔一帧（引擎的常见代价）。
// ══════════════════════════════════════════════════════════════════
#define kHzbDepthBias 0.0002f

uint HzbDim(uint full, uint level) { return (full + (1u << level) - 1u) >> level; }

uint HzbLevelBase(uint level) {
	uint base = 0u;
	for (uint l = 0u; l < level; ++l) base += HzbDim((uint)screenSize.x, l) * HzbDim((uint)screenSize.y, l);
	return base;
}

bool CullSphere(float4 LODBounds) {
	const float3 center = LODBounds.xyz;
	const float  radius = LODBounds.w;

	// VPMatrix 是行向量约定（与 NaniteRender.hlsl 一致）；clip.w = 沿视线的距离，正数在相机前方
	const float4 clip = mul(float4(center, 1.0f), VPMatrix);
	// 整个球都在相机背后 → 直接剔除。**这一条必须单独判**：背对物体时所有球的 clip.w 都是负数，
	// 若只写成 clip.w <= radius 就一律放行，结果整场景的簇都被选中并送去光栅化（实测背对时
	// hw 不为 0、帧率掉到 200）。球心的 w 加上半径仍 ≤ 0，说明连最靠近相机的那点也在背后。
	if (clip.w + radius <= 0.0f) return true;
	if (clip.w <= radius) return false;                 // 跨越相机平面：NDC 除法不稳定，保守放行

	const float2 ndc = clip.xy / clip.w;

	// 半径的 NDC 尺度：把球心沿相机右方向再推 radius 投一次，取分量较大者（偏保守）
	const float3 camRight = normalize(float3(VPMatrix[0].x, VPMatrix[0].y, VPMatrix[0].z));
	const float4 clipR = mul(float4(center + camRight * radius, 1.0f), VPMatrix);
	const float2 ndcR = clipR.xy / max(clipR.w, 1e-6f);
	const float2 d = abs(ndcR - ndc);
	const float radiusNdc = max(d.x, d.y);

	// 视锥剔除：整个球都在 NDC 之外
	if (ndc.x + radiusNdc < -1.0f || ndc.x - radiusNdc > 1.0f) return true;
	if (ndc.y + radiusNdc < -1.0f || ndc.y - radiusNdc > 1.0f) return true;

	const float radiusPx = max(radiusNdc * 0.5f * screenSize.x, 1.0f);

	// 选层：一个 texel 至少覆盖 2*radiusPx 的直径 ⇒ 取到的是超集的 min（保守）
	uint level = 0u;
	while (level < NANITE_HZB_MAX_LEVEL && (float)(1u << level) < radiusPx * 2.0f) ++level;

	const uint levelWidth = HzbDim((uint)screenSize.x, level);
	const uint levelHeight = HzbDim((uint)screenSize.y, level);
	const float2 pixelCenter = (ndc * float2(0.5f, -0.5f) + 0.5f) * screenSize;
	const uint tx = min((uint)max(pixelCenter.x, 0.0f) >> level, levelWidth - 1u);
	const uint ty = min((uint)max(pixelCenter.y, 0.0f) >> level, levelHeight - 1u);

	const uint hzbDepth = HZB.Load((HzbLevelBase(level) + ty * levelWidth + tx) * 4u);

	// 球最近点的深度：把球心沿相机前向推 radius 的那个点再投一次，取它的 z/w。
	// ⚠ 必须和两条光栅路径写进 VisBuffer 的深度用**同一套映射**：软光栅的 InterpolateDepth 与
	// 硬光栅 PS 都做了 [-1,1]→[0,1] 的 *0.5+0.5（GLM 默认是 GL/D3D 的 NDC z ∈ [-1,1]，
	// 而清屏值/VisBuffer 存的都是 [0,1]）。少乘这一次，深度会整体差半格 → 每个球都「比遮挡物远」
	// 而被全部剔掉（实测 hw 0）。
	const float3 camForward = normalize(float3(VPMatrix[2].x, VPMatrix[2].y, VPMatrix[2].z));
	const float4 clipNear = mul(float4(center - camForward * radius, 1.0f), VPMatrix);
	if (clipNear.w <= 0.0f) return false;               // 近端点到相机后方：保守放行
	const float nearDepth = clipNear.z / clipNear.w * 0.5f + 0.5f;
	return nearDepth > hzbDepth + kHzbDepthBias;
}

ClusterPayload LoadClusterPayload(uint pageIndex, uint clusterOffset) {
	ClusterPayload payload;

	uint pageBaseAddressOffset = clusterPagesBuffer.Load(4 + pageIndex * 4);
	uint clusterCount = clusterPagesBuffer.Load(pageBaseAddressOffset);

	uint clusterDataOffset = clusterPagesBuffer.Load(pageBaseAddressOffset + 4 + 4 * clusterOffset);

	uint clusterBaseAddress = pageBaseAddressOffset + 4 + clusterCount * 4 + clusterDataOffset;

	payload.LODBounds = asfloat(clusterPagesBuffer.Load4(clusterBaseAddress + 8));

	uint clusterLODErrorAndEdgeLength = clusterPagesBuffer.Load(clusterBaseAddress + 24);

	payload.LODError = f16tof32(clusterLODErrorAndEdgeLength & 0xFFFF);
	payload.EdgeLength = f16tof32(clusterLODErrorAndEdgeLength >> 16);

	payload.IndexCount = clusterPagesBuffer.Load(clusterBaseAddress + 4);
	payload.BaseVertex = clusterPagesBuffer.Load(clusterBaseAddress + 8);
	payload.BaseIndex = clusterPagesBuffer.Load(clusterBaseAddress + 12);


	uint indexDataOffset = clusterPagesBuffer.Load(clusterBaseAddress);
	payload.PositionCount = (indexDataOffset - 28) / 12;
	payload.BaseAddress = clusterBaseAddress;

	return payload;
}


void ProcessCluster(ClusterPayload payload, uint PageIndex, uint ClusterOffset, bool useSoftware) {
	uint clusterID = (PageIndex << 8) | ClusterOffset;

	// 软光栅列表按三角形展开（见下），硬光栅列表按簇；CPU 侧按 pageIndex/clusterOffset 解码。
	uint write;
	if (useSoftware) {
		InterlockedAdd(EnableClusterListSw[0], 1, write);
		EnableClusterListSw[write + 2] = clusterID;

		// 软光栅按**三角形**并行：这里把本簇的三角形展开进工作列表，软光栅一个线程画一个。
		// 按簇并行时只有一个线程干活（并行度只有几十条 lane）、还被覆盖最大的簇拖长尾。
		// 条目写 3 个 uint：(clusterID, 簇数据区字节偏移 cb, 簇内三角形序号) —— cb 在这里
		// 顺手带出去，软光栅就不必每个三角形都重新走一遍「页表 → 簇记录」的依赖链。
		// 上限保护：工作列表按「每簇 ≤ 128 三角形」分配（= NaniteBuilder 的 maxClusterTriangles，
		// mitsuba 数据集实测 126）。数据万一超限就只展开前 128 个 —— 宁可少画一点，也不能越界写。
		const uint triCount = min(payload.IndexCount / 3u, NANITE_MAX_CLUSTER_TRIANGLES);
		uint triBase;
		InterlockedAdd(EnableClusterListSw[1], triCount, triBase);   // [1] 兼作分配计数器
		for (uint k = 0; k < triCount; ++k) {
			const uint slot = (triBase + k) * 3u;
			SwTriList[slot + 0] = clusterID;
			SwTriList[slot + 1] = payload.BaseAddress;
			SwTriList[slot + 2] = k;
		}
		return;
	}

	InterlockedAdd(EnableClusterListHw[0], 1, write);
	EnableClusterListHw[write + 1] = clusterID;

	// 硬光栅还需要：间接绘制参数（VS 用 firstInstance 反查 EnableClusterListHw[write+1]，
	// vertexCount = 簇索引总数）。CPU 每帧把尾部残留项清零，使 write 超出选中数的 draw
	// 因 vertexCount=0 成为 no-op。clusterDataBuffer 按同一条线性序号索引。
	IndirectDrawBuffer[write].vertexCount = payload.IndexCount;
	IndirectDrawBuffer[write].instanceCount = 1;
	IndirectDrawBuffer[write].firstVertex = 0;
	IndirectDrawBuffer[write].firstInstance = write;

	clusterDataBuffer[write].indexCount = payload.IndexCount;
	clusterDataBuffer[write].baseIndex = payload.BaseIndex;
	clusterDataBuffer[write].baseVertex = payload.BaseVertex;
}

// ── 组内选择：逐簇两层界判定（clusterlod.h 的文档语义）────────────────────────
// clusterlod 的组关系是**多父 DAG，不是树**（实测 55 个组里 42 个有多个父组）。任何
// 「从最粗层沿层次下降、走到某个组就画整组」的判据在 DAG 上都不自洽：同一个组会被多条
// 路径访问 → 重复入队（实测绘制面积 = 真实面积的 2762%），而且祖先组与后代组可能同时
// 被画 → 同一片区域两个细节层重叠 = 斑点/闪烁。
//
// 正确的判据是**逐簇、与路径无关**的：
//   簇 C 属于组 H，C 的上一层（产生它的那一次简化）的误差记作 ε(C)，
//   组 H 被简化成的那一版（更粗一级）的误差记作 ε↑(H) = clodGroup::simplified.error，
//   C 要画 ⟺ ε(C) ≤ T < ε↑(H)，T = projScale / LodScale 是该距离下的世界空间误差预算。
//   · 本层够精细： ε(C) ≤ T     ⟺ projScale ≥ LodScale * C.LODError
//   · 更粗一层还不够精细：ε↑(H) > T    ⟺ projScale < LodScale * H.MaxParentLODError
//     （代码里写的是它的否定形式：ε↑(H) ≤ T 时整组跳过）
// 于是每片区域在每一层上恰好有一层被选中：即「最粗的、已经够精细的那一层」。
// 实测（offline 解码 built.bvh，相机 z=150/400/800/1600，LodScale 30…100000）：
// 绘制面积 / 真实面积 = 93.1%…100.0%，不再随 LodScale 膨胀。
//
// 注意 ε(C) 必须用**簇自己**的 LODError：同一组内各簇的 ε 不同，用组的最大值
// （MinLODError）当左界会把组内较粗的簇整个挡掉 → 那部分区域永远是空的（实测掉到 23%）。
void ProcessLeafCluster(HierarchyNodeSlice slice) {
	if (!slice.bLeaf || slice.NumChildren == 0) return;

	// 组一层：更粗的一层已经够精细（ε↑(H) ≤ T）→ 整组都不画（那片区域由更粗的组覆盖）
	// ε↑(H) ≤ T  ⟺  projScale ≥ LodScale * MaxParentLODError
	const float3 toGroup = slice.LODBounds.xyz - Position.xyz;
	const float  projGroup = GetProjectionScales(Direction.xyz, toGroup, slice.LODBounds.w);
	if (projGroup >= LodScale * slice.MaxParentLODError) return;

	const uint pageIndex = slice.StartPageIndex;
	const uint clusterStartOffset = slice.ChildStartReference & NANITE_CLUSTER_OFFSET_MASK;

	for (uint i = 0; i < slice.NumChildren; ++i) {
		const uint clusterOffset = clusterStartOffset + i;
		ClusterPayload payload = LoadClusterPayload(pageIndex, clusterOffset);

		// 剔除：视锥外 或 被上一帧深度挡住就跳过（读一次 HZB，见 CullSphere）
		if (CullSphere(payload.LODBounds)) continue;

		// 簇一层：本簇自身的误差已经够小（ε(C) ≤ T）→ 画它
		// （用簇自己的包围球算投影，与 Nanite 的 SmallEnoughToDraw 一致）
		const float3 toCluster = payload.LODBounds.xyz - Position.xyz;
		const float  projCluster = GetProjectionScales(Direction.xyz, toCluster, payload.LODBounds.w);
		if (projCluster >= LodScale * payload.LODError) {
			ProcessCluster(payload, pageIndex, clusterOffset,
			               ShouldUseSoftwareRaster(payload.LODBounds.w, projCluster));
		}
	}
}

// ══════════════════════════════════════════════════════════════════
// 沿 BVH 下降（无锁队列 GPU 写 / GPU 读）—— 唯一的选择路径
//
// 一层一次 dispatch：读 BvhQueueIn 里的条目（节点下标），展开它的 4 个槽 ——
//   link 槽  → 先 CullSphere 测子节点的包围球，通过才把 childNode 追加进 BvhQueueOut（子树剔除）；
//   可画组槽 → 直接调 ProcessLeafCluster（组级两层界判据）。
// in/out 两份队列 ping-pong：主机知道轮次奇偶，用两个资源包换绑即可，不需要传轮次进着色器。
//
// 队列计数器会跨轮累积（第 k 轮把更早几轮读过的槽位再算一遍前缀），那份「陈旧前缀」里的节点
// 之前已经访问过，被帧号去重挡掉 —— 只浪费一点判断，不会重复绘制。
// ══════════════════════════════════════════════════════════════════
[numthreads(64, 1, 1)]
void mainTraverse(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint i = dispatchThreadID.x;
	if (i >= BvhQueueIn[0]) return;                       // [0] = 本层条目数

	const uint nodeIndex = BvhQueueIn[NANITE_QUEUE_HEADER + i];
	if (nodeIndex >= TotalBVHNodes) return;

	// 去重：这个 DAG 是多父的，同一个节点会被多条路径到达；命中过就退出，否则标记本帧已访问。
	// 用帧号当代，所以不需要每帧清这张表。
	const uint epoch = BvhVisited[0];
	uint prev = 0u;
	InterlockedExchange(BvhVisited[1 + nodeIndex], epoch, prev);
	if (prev == epoch) return;

	for (uint c = 0u; c < NANITE_BVH_NODE_FANOUT; ++c) {
		HierarchyNodeSlice slice = GetHierarchyNodeSlice(clusterSelectionBuffer, nodeIndex, c);
		if (!slice.bEnabled) continue;                    // 空槽

		if (!slice.bLeaf) {
			if (slice.LODBounds.w > 0.0f && CullSphere(slice.LODBounds)) continue;
			uint writeIndex;
			InterlockedAdd(BvhQueueOut[0], 1u, writeIndex);
			BvhQueueOut[NANITE_QUEUE_HEADER + writeIndex] = slice.ChildStartReference >> 16;
			continue;
		}

		ProcessLeafCluster(slice);                        // 可画组：组级判据
	}
}

