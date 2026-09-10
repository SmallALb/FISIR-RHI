
#define NANITE_BVH_NODE_FANOUT_BITS 2
#define NANITE_BVH_NODE_FANOUT_MASK ((1<<NANITE_BVH_NODE_FANOUT_BITS)-1)
#define NANITE_BVH_NODE_FANOUT      ((1<<NANITE_BVH_NODE_FANOUT_BITS))
#define HIERARCHY_NODE_SLICE_SIZE ((4+4+4+1)*4*NANITE_BVH_NODE_FANOUT)

// 叶子节点 Misc2（组信息）位布局：
//   [0,8)   GroupPartSize：叶子包含的簇数量
//   [8,32)  组 LOD 等级 / 流式元数据（本 demo 未使用；簇的 page/offset 编码在 ChildStartReference 中）
//   0xFFFFFFFF -> 内部节点（子项是节点）；0 -> 空槽位
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
RWStructuredBuffer<uint> EnableClusterList : register(u3);
cbuffer InputData :register(b4){
  float4 Position;    // xyz + pad（std140 下 float3 被填充到 16 字节）
	float4 Direction;   // xyz + pad
	float	 LodScale;
	float  ZNear;
	uint   CountOfClusters;
	uint 	 TotalBVHNodes;      
	uint 	 TotalSlices;        
	uint 	 MaxClusters;        
};

// InputData Input : register(b2); 

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
RWStructuredBuffer<HierarchyNodeSlice> DebugBuffer : register(u5);

HierarchyNodeSlice UnPackHierarchyNodeSlice(uint4 RawData0, uint4 RawData1, uint4 RawData2, uint RawData3) {
	const uint4 Misc0 = RawData1;   // BoxBoundsCenter.xyz + MinLODError_MaxParentLODError（两个 half 打包）
	const uint4 Misc1 = RawData2;   // BoxBoundsExtent.xyz + ChildStartReference
	const uint  Misc2 = RawData3;   // 组信息：空 / 内部 / 叶子

	HierarchyNodeSlice Res = (HierarchyNodeSlice)0;
	Res.LODBounds = asfloat(RawData0);
	Res.BoxBoundsCenter = float4(asfloat(Misc0.xyz), 0.0);
	Res.BoxBoundsExtent = float4(asfloat(Misc1.xyz), 0.0);

	// LOD 误差打包为两个 half：低 16 位 = MinLODError，高 16 位 = MaxParentLODError
	Res.MinLODError = f16tof32(Misc0.w);
	Res.MaxParentLODError = f16tof32(Misc0.w >> 16);

	Res.ChildStartReference = Misc1.w;

	// 叶子 / 内部 / 空 判定（内部节点 Misc2 全 1，空槽位 Misc2 为 0）
	Res.bLeaf = ((Misc2 != NANITE_INTERNAL_NODE_MARKER) && (Misc2 != 0u)) ? 1u : 0u;
	Res.bEnabled = (Misc2 != 0u) ? 1u : 0u;
	Res.bLoaded = 1u;   // mitsuba 场景一次性全部加载，无按需流式，恒为已加载

	// 叶子：簇数量（低 8 位）；内部 / 空节点此值无意义
	Res.NumChildren = Misc2 & NANITE_GROUP_PART_SIZE_MASK;

	// 叶子：簇的寻址信息来自 ChildStartReference = (pageIndex << 8) | clusterOffset
	Res.StartPageIndex = Misc1.w >> 8;
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

float GetProjectionScales(float3 viewDir, float3 toCenter, float radius) {
	float DisToClusterS = dot(toCenter, toCenter);
	float z = dot(viewDir, toCenter);
	float xS = DisToClusterS - z * z;
	float tS = DisToClusterS - radius * radius;
	float t = sqrt(tS);
	float x = sqrt(xS);
	float DisToCluster = sqrt(DisToClusterS);
	
	if (DisToCluster < 0.000001f) return 0.0f;

	float Cos = t / DisToCluster;
	float Sin = radius / DisToCluster;

	float y = (-Sin*x + Cos*z) / DisToCluster;
	float MinZ = max(z - radius,  ZNear);
	if (z + radius >  ZNear) return y * MinZ;
	return 0.0f;
}

struct ClusterPayload {
    float4 LODBounds;      // 包围球 (xyz=中心, w=半径)
    float  LODError;       // LOD 误差
    float  EdgeLength;     // 边长度 (暂未使用)
    uint   PositionCount;  // 顶点数量
    uint   IndexCount;     // 索引数量
		 uint   BaseVertex;     
    uint   BaseIndex;      
};

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

	return payload;
}


void ProcessCluster(uint PageIndex, uint ClusterOffset) {
	ClusterPayload payload = LoadClusterPayload(PageIndex, ClusterOffset);

	float3 toCluster = payload.LODBounds.xyz -  Position.xyz;
	float projScale = GetProjectionScales( Direction.xyz, toCluster, payload.LODBounds.w);

	float threshold =  LodScale * payload.LODError;

	uint clusterID = (PageIndex << 8) | ClusterOffset;
	if (projScale > threshold) {
		uint write;
		InterlockedAdd(EnableClusterList[0], 1, write);
		// clusterID 是「页索引<<8 | 簇内偏移」的编码，并非线性簇号；
		// 直接写入即可，CPU 侧按 pageIndex/clusterOffset 解码。
		EnableClusterList[write + 1] = clusterID;
		// clusterDataBuffer 用线性选中序号 write 索引，与 NaniteRender 的
		// clusterDataBuffer[clusterIndex]（线性）一致，而非编码后的 clusterID。
		clusterDataBuffer[write].indexCount = payload.IndexCount;
		clusterDataBuffer[write].baseIndex = payload.BaseIndex;
		clusterDataBuffer[write].baseVertex = payload.BaseVertex;

	}
}

void ProcessLeafCluster(HierarchyNodeSlice slice) {
	if (!slice.bLeaf || slice.NumChildren == 0) return;

	uint pageIndex = slice.StartPageIndex;
	uint clusterStartOffset = slice.ChildStartReference & NANITE_CLUSTER_OFFSET_MASK;

	for (uint i=0; i<slice.NumChildren; ++i) {
		uint clusterOffset = clusterStartOffset + i;
		ProcessCluster(pageIndex, clusterOffset);
	}
}

[numthreads(64, 1, 1)]
void mainCS(uint3 dispatchThreadID : SV_DispatchThreadID) {

	// 诊断哨兵：thread 0 写入保留槽 DebugBuffer[100]，用于验证 shader 是否真正执行
	if (dispatchThreadID.x == 0) {
		DebugBuffer[100].LODBounds = float4(1.0f, 2.0f, 3.0f, 4.0f);
		DebugBuffer[100].bLeaf = 0xDEADBEEFu;
		DebugBuffer[100].NumChildren = 0x12345678u;
	}

	uint sliceIndex = dispatchThreadID.x;
	if (sliceIndex >= TotalSlices) return;   // 越界线程（dispatch(2,1,1)=128 线程 > 84 slice）直接退出

	uint nodeIndex = sliceIndex / 4;
	uint childIndex = sliceIndex % 4;

	HierarchyNodeSlice slice = GetHierarchyNodeSlice(clusterSelectionBuffer, nodeIndex, childIndex);
	DebugBuffer[sliceIndex] = slice;   // 逐 slice 写调试输出（每个线程独占一行，无竞争）

	if (!slice.bEnabled || !slice.bLoaded) return;

	float3 toCenter = slice.LODBounds.xyz -  Position.xyz;
	float projScale = GetProjectionScales( Direction.xyz, toCenter, slice.LODBounds.w);
	float threshold =  LodScale * slice.MaxParentLODError;

	if (projScale == 0.0f) return;

	if (projScale > threshold) {
		if (slice.bLeaf)  ProcessLeafCluster(slice); 
	}
	else if (slice.bLeaf)  ProcessLeafCluster(slice); 
}
