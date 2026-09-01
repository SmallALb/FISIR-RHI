
#define NANITE_BVH_NODE_FANOUT_BITS 2
#define NANITE_BVH_NODE_FANOUT_MASK ((1<<NANITE_BVH_NODE_FANOUT_BITS)-1)
#define NANITE_BVH_NODE_FANOUT      ((1<<NANITE_BVH_NODE_FANOUT_BITS))
#define NANITE_BVH_NODE_ENABLE_MASK ((1<<NANITE_BVH_NODE_FANOUT)-1)
#define HIERARCHY_NODE_SLICE_SIZE ((4+4+4+1)*4*NANITE_BVH_NODE_FANOUT)

#define NANITE_CLUSTERS_PER_GROUP_BITS 9
#define NANITE_GROUP_PARTS_BITS 5
#define NANITE_RESOURCE_PART_BITS 16


ByteAddressBuffer 		clusterSelectionBuffer : register(t0);
RWByteAddressBuffer  	clusterDataBuffer: register(u1);


uint BitFieldExtractU32(uint Data, uint Size, uint Offset) {
	Size &= 31u;
	Offset &= 31u;
	return (Data >> Offset) & ((1u << Size) - 1u);
}

struct HierarchyNodeSlice {
	float4 LODBounds;
	float3 BoxBoundsCenter;
	float3 BoxBoundsExtent;
	float  MinLODError;
	float  MaxParentLODError;
	uint   ChildStartReference;
	uint   NumChildren;
	uint   StartPageIndex;
	uint   NumPages;
	bool   bEnabled;
	bool   bLoaded;
	bool   bLeaf;
};

HierarchyNodeSlice UnPackHierarchyNodeSlice(uint4 RawData0, uint4 RawData1, uint4 RawData2, uint RawData3) {
	const uint4 Misc0 = RawData1;
	const uint4 Misc1 = RawData2;
	const uint  Misc2 = RawData3;

	HierarchyNodeSlice Res;
	Res.LODBounds = asfloat(RawData0);
	Res.BoxBoundsCenter = asfloat(Misc0.xyz);
	Res.BoxBoundsExtent = asfloat(Misc1.xyz);

	Res.MinLODError = f16tof32(Misc0.w >> 16);
	Res.MaxParentLODError = f16tof32(Misc0.w);
	Res.ChildStartReference = Misc1.w;
	Res.bLoaded = (Misc1.w != 0xFFFFFFFFu);

	Res.NumChildren = BitFieldExtractU32(Misc2, NANITE_CLUSTERS_PER_GROUP_BITS, 0);
	Res.NumPages = BitFieldExtractU32(Misc2, NANITE_GROUP_PARTS_BITS, NANITE_CLUSTERS_PER_GROUP_BITS);
	Res.StartPageIndex = BitFieldExtractU32(Misc2, NANITE_GROUP_PARTS_BITS, NANITE_CLUSTERS_PER_GROUP_BITS + NANITE_GROUP_PARTS_BITS);
	Res.bEnabled = ((Misc2 & NANITE_BVH_NODE_ENABLE_MASK) != 0u);
	Res.bLeaf = (Misc2 != 0xFFFFFFFFu);
	return Res;
}

HierarchyNodeSlice GetHierarchyNodeSlice(ByteAddressBuffer hierarchyNodeBuffer, uint Index, uint ChildIndex) {
	const uint BaseAddress = Index * HIERARCHY_NODE_SLICE_SIZE;

	const uint4 RawData0 = hierarchyNodeBuffer.Load4(BaseAddress + 16 * ChildIndex);
	const uint4 RawData1 = hierarchyNodeBuffer.Load4(BaseAddress + (NANITE_BVH_NODE_FANOUT * 16) + 16 * ChildIndex);
	const uint4 RawData2 = hierarchyNodeBuffer.Load4(BaseAddress + (NANITE_BVH_NODE_FANOUT * 32) + 16 * ChildIndex);
	const uint  RawData3 = hierarchyNodeBuffer.Load (BaseAddress + (NANITE_BVH_NODE_FANOUT * 48) + 4 * ChildIndex);

	return UnPackHierarchyNodeSlice(RawData0, RawData1, RawData2, RawData3);
}

[numthreads(1, 1, 1)]
void mainCS(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint offset = 0u;
	for (uint i=0; i<21; ++i) {
		HierarchyNodeSlice hierarchyNodeSlice = GetHierarchyNodeSlice(clusterSelectionBuffer, i, 0);
		for (int j=0; j<4; ++j) {
			hierarchyNodeSlice = GetHierarchyNodeSlice(clusterSelectionBuffer, i, j);
			uint isLeaf = (hierarchyNodeSlice.NumChildren == 0) ? 1u : 2u;
			clusterDataBuffer.Store4(offset*16, uint4(i, j, isLeaf, hierarchyNodeSlice.ChildStartReference));
	  	offset++;
		}
	}
}
