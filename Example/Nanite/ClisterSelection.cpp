#include "ClusterSelection.h"
#include "ShaderComplier.h"
#include <cstring>
#include <fstream>
#include <string>
#include <iterator>

// 从磁盘读取整个文件为字符串（二进制读取，保留原始 UTF-8 字节）
static std::string LoadFileText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Error("Failed to open shader file: {}", path);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static FISIR::RHIShader* ClusterSelectionComputeShader = nullptr;
static FISIR::RHIShader* NaniteRenderComputeShader = nullptr;
static FISIR::RHIShader* ClearScreenComputeShader = nullptr;

static FISIR::RHIBuffer* ClusterSelectionBuffer = nullptr;
static FISIR::RHIBuffer* ClusterPageDataBuffer = nullptr;
static FISIR::RHIBuffer* ClusterDataBuffer = nullptr;  // outbuffer：簇选择结果
static FISIR::RHIBuffer* InputDataBuffer = nullptr;
static FISIR::RHIBuffer* EnableClusterListBuffer = nullptr;
static FISIR::RHIBuffer* DebugClusterListBuffer = nullptr;

static FISIR::RHIBuffer* FrameBuffer = nullptr;         // u2：逐像素 depth|color（8B/px）
static FISIR::RHIBuffer* FrameLock = nullptr;          // u3：逐像素自旋锁（4B/px）
static FISIR::RHIBuffer* RenderParamsBuffer = nullptr; // b2：RenderParams

static FISIR::RHIPipeline* ClusterSelectionPipeline = nullptr;
static FISIR::RHIPipeline* NaniteRenderPipeline = nullptr;
static FISIR::RHIPipeline* ClearScreenPipeline = nullptr;
static FISIR::RHIResourcePackResult ClusterSelectionResourcePack;
static FISIR::RHIResourcePackResult NaniteRenderResourcePack;
static FISIR::RHIResourcePackResult ClearScreenResourcePack;
static FISIR::RHIFence* ClusterSelectionFence = nullptr;

void InitClusterSelection(FISIR::DynamicRHI* rhi) {
		//Complie and Create Shader（从磁盘加载 HLSL 源文件）
	std::string csSource = LoadFileText("Shader/ClusterSelection.hlsl");
	FISIR::ShaderComplier* csCompiler = new FISIR::ShaderComplier();
	csCompiler->compileShader(csSource.data(), csSource.size(), "mainCS", "cs_6_7");
	ClusterSelectionComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainCS", csCompiler->getShaderData(), csCompiler->getShaderDataSize());
	if (!ClusterSelectionComputeShader) Error("[Nanite] ClusterSelectionComputeShader creation FAILED!");

	std::string RenderSource = LoadFileText("Shader/NaniteRender.hlsl");
	FISIR::ShaderComplier* renderCompiler = new FISIR::ShaderComplier();
	renderCompiler->compileShader(RenderSource.data(), RenderSource.size(), "mainRender", "cs_6_7");
	NaniteRenderComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainRender", renderCompiler->getShaderData(), renderCompiler->getShaderDataSize());
	if (!NaniteRenderComputeShader) Error("[Nanite] NaniteRenderComputeShader creation FAILED!");

	//1.reate Buffer
	// BVH：改用 host-visible + coherent 直接上传（仿 BunnyPBR），避免 transfer→compute
	// 异队列族的跨队可见性问题。
	FISIR::BufferInfo clusterSelectionBufferInfo{
		.size = 4 * 1024 * 1024,
		.bufferlayout = FISIR::RBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};

	ClusterSelectionBuffer = rhi->RHICreateBuffer(clusterSelectionBufferInfo);

	//2.Create OutBuffer
	FISIR::BufferInfo clusterDataBufferInfo{
		.size = 4 * 1024 * 1024,
		.bufferlayout = FISIR::RWBuffer | FISIR::TransferDstBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
	};
	ClusterDataBuffer = rhi->RHICreateBuffer(clusterDataBufferInfo);


	//3.Create PageDataBuffer
	// mesh 数据同样 host-visible 直接上传。
	FISIR::BufferInfo clusterPageDataBufferInfo{
		.size = 4 * 1024 * 1024,
		.bufferlayout = FISIR::RBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	ClusterPageDataBuffer = rhi->RHICreateBuffer(clusterPageDataBufferInfo);
	
	InputData iptData {
		.Position = {0.f, 0.f, 0.f, 0.f},
		.Direction = {0.f, 0.f, -1.f, 0.f},
		.LodScale = 1.f,
		.ZNear = 0.1f,
		.CountOfClusters = 932,   // mitsuba.nanitemesh 共 932 个簇
		.TotalBVHNodes = 21,      // mitsuba.bvh = 4368 字节 / 208 = 21 节点
		.TotalSlices = 84,        // 21 节点 × 4 子槽
		.MaxClusters = 932,
	};

	//4.Create InputDataBuffer
	FISIR::BufferInfo cameraBufferInfo{
		.data_CPU = &iptData,
		.size = sizeof(InputData),
		.bufferlayout = FISIR::UniformBuffer | FISIR::TransferDstBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	InputDataBuffer = rhi->RHICreateBuffer(cameraBufferInfo);

	//5.Create EnableClusterListBuffer
	// 主机可见 + 相干：compute 写完计数与 clusterID 列表后，CPU 经 getBufferData()
	// 直接读回选中结果（fence->wait() 保证写入完成、相干内存保证可见性）。
	FISIR::BufferInfo enableClusterListBufferInfo{
		.data_CPU = nullptr,
		.size = 4 * 1024,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	EnableClusterListBuffer = rhi->RHICreateBuffer(enableClusterListBufferInfo);
	
	//6.Create DebugClusterListBuffer（host-visible，便于诊断读回 slice 数据）
	FISIR::BufferInfo debugClusterListBufferInfo{
		.data_CPU = nullptr,
		.size = 4 * 1024 * 1024,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	DebugClusterListBuffer = rhi->RHICreateBuffer(debugClusterListBufferInfo);

	//7. Nanite 渲染用：FrameBuffer（8B/px）、FrameLock（4B/px）、RenderParams（96B）
	const uint32_t pixelCount = NANITE_RT_WIDTH * NANITE_RT_HEIGHT;
	// FrameBuffer 是唯一跨队列族的缓冲：compute（渲染）写、graphics（呈现）读。
	// 本机 graphics/compute 分属不同队列族（Graphics=0, Compute=2），若保持独占共享模式
	// 则跨族访问必须做所有权转移，否则触发校验错误甚至挂起。改为并发共享即可免转移；
	// 帧间仍由 fence->wait() 串行化保证写后读正确。
	FISIR::BufferInfo frameBufferInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)pixelCount * 8,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		.concurrentSharing = true,
	};
	FrameBuffer = rhi->RHICreateBuffer(frameBufferInfo);

	FISIR::BufferInfo frameLockInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)pixelCount * 4,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	FrameLock = rhi->RHICreateBuffer(frameLockInfo);

	RenderParams initParams {
		.VPMatrix = glm::mat4(1.0f),
		.screenSize = { (float)NANITE_RT_WIDTH, (float)NANITE_RT_HEIGHT },
		.ClearDepth = 1.0f,
		.ClearColor = 0xFF2E2A26,  //ABGR
		.NearPlane = 1.0f,
		.FarPlane = 2000.0f,
		._pad = { 0.0f, 0.0f },
	};
	FISIR::BufferInfo renderParamsInfo{
		.data_CPU = &initParams,
		.size = sizeof(RenderParams),
		.bufferlayout = FISIR::UniformBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	RenderParamsBuffer = rhi->RHICreateBuffer(renderParamsInfo);

	// FrameLock 必须全 0（否则锁初值为 1 → 对应像素永久自旋死锁）
	memset(FrameLock->getBufferData(), 0, (uint64_t)pixelCount * 4);

	ClusterSelectionResourcePack = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterPageDataBuffer, ClusterDataBuffer, EnableClusterListBuffer, InputDataBuffer, DebugClusterListBuffer });
	

	//Create Descriptor and Pipeline
	// ByteAddressBuffer 的 NonWritable 加在类型成员上，descriptor buffer 扩展不认，RBuffer(READ_ONLY) 读全 0。
	// 故 shader 里字节缓冲一律改用 RWByteAddressBuffer（READ_WRITE），并把所有资源重排到唯一 binding：
	// u0 BVH, u1 mesh, u2 clusterData, u3 EnableClusterList, b4 InputData, u5 DebugBuffer。
	FISIR::RHIPipelineDescribeInfo describeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u0 clusterSelectionBuffer
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u1 clusterPagesBuffer
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u2 clusterDataBuffer
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u3 EnableClusterList
		{4, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b4 InputData
		{5, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u5 DebugBuffer

	};


	FISIR::RHIPipelineState PipelineState {
		.describeInfo = describeInfo,
		.isComputePipeline = 1,
	};
	PipelineState.Shaders[FISIR::__COMPUTESHADER__] = ClusterSelectionComputeShader;
	ClusterSelectionPipeline = rhi->RHICreatePipeline(PipelineState);
	if (!ClusterSelectionPipeline) Error("[Nanite] ClusterSelectionPipeline creation FAILED!");

	// Nanite 渲染管线：字节缓冲用 RWByteAddressBuffer + 唯一 binding：
	// u0 BVH(未用), u1 mesh, u2 clusterData(未用), u3 EnableClusterList, u4 FrameBuffer, u5 FrameLock, b6 RenderParams
	FISIR::RHIPipelineDescribeInfo renderDescribeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{4, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{5, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{6, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
	};
	FISIR::RHIPipelineState renderPipelineState {
		.describeInfo = renderDescribeInfo,
		.isComputePipeline = 1,
	};
	renderPipelineState.Shaders[FISIR::__COMPUTESHADER__] = NaniteRenderComputeShader;
	NaniteRenderPipeline = rhi->RHICreatePipeline(renderPipelineState);

	NaniteRenderResourcePack = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterPageDataBuffer, ClusterDataBuffer, EnableClusterListBuffer, FrameBuffer, FrameLock, RenderParamsBuffer });

	// 清屏着色器：渲染前把 FrameBuffer 清为远深度 + 清屏色（GPU compute，替代 CPU std::fill）。
	// 绑定：u0 FrameBuffer、b1 RenderParams（复用 RenderParamsBuffer，与 NaniteRender 的 ClearDepth/ClearColor 同源）。
	std::string ClearSource = LoadFileText("Shader/ClearScreen.hlsl");
	FISIR::ShaderComplier* clearCompiler = new FISIR::ShaderComplier();
	clearCompiler->compileShader(ClearSource.data(), ClearSource.size(), "mainClear", "cs_6_7");
	ClearScreenComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainClear", clearCompiler->getShaderData(), clearCompiler->getShaderDataSize());
	if (!ClearScreenComputeShader) Error("[Nanite] ClearScreenComputeShader creation FAILED!");

	FISIR::RHIPipelineDescribeInfo clearDescribeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u0 FrameBuffer
		{1, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b1 RenderParams
	};
	FISIR::RHIPipelineState clearPipelineState {
		.describeInfo = clearDescribeInfo,
		.isComputePipeline = 1,
	};
	clearPipelineState.Shaders[FISIR::__COMPUTESHADER__] = ClearScreenComputeShader;
	ClearScreenPipeline = rhi->RHICreatePipeline(clearPipelineState);
	if (!ClearScreenPipeline) Error("[Nanite] ClearScreenPipeline creation FAILED!");

	ClearScreenResourcePack = rhi->RHICreateResourcePack({ FrameBuffer, RenderParamsBuffer });

	ClusterSelectionFence = rhi->RHICreateFence(false, "ClusterSelectionFence");
	if (!ClusterSelectionFence) Error("Failed to create ClusterSelectionFence");
}

void ExecuteClusterSelectionPass(FISIR::DynamicRHI* rhi) {
	// --- 2. 清屏（GPU compute，替代 CPU std::fill）---
	// 渲染前把 FrameBuffer 每个像素清为远深度(1.0) + 清屏色(0)，保证 AtomicDepthTest 有正确的初始深度。
	// 独立命令列表（page 只能提交一次，不能与渲染共用同一 cmdlist 对象）。
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetPipelineState(ClearScreenPipeline);
		cmdlist.SetResourcePack(ClearScreenResourcePack);
		cmdlist.dispatch((NANITE_RT_WIDTH + 15) / 16, (NANITE_RT_HEIGHT + 15) / 16, 1);
		cmdlist.End(ClusterSelectionFence);
	}
	ClusterSelectionFence->wait();
	ClusterSelectionFence->reset();
	// --- 1. 簇选择（独立命令列表，每个批次一个 fresh page）---
	*static_cast<int*>(EnableClusterListBuffer->getBufferData()) = 0;
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetPipelineState(ClusterSelectionPipeline);
		cmdlist.SetResourcePack(ClusterSelectionResourcePack);
		cmdlist.dispatch(2, 1, 1);
		cmdlist.End(ClusterSelectionFence);
	}
	ClusterSelectionFence->wait();
	ClusterSelectionFence->reset();

	// 诊断：读回 DebugBuffer（每个 slice 96 字节 = 24 uint32），检查 BVH 数据与叶子遍历
	static bool dbgDumpLogged = false;
	if (!dbgDumpLogged) {
		const uint32_t* dbg = static_cast<const uint32_t*>(DebugClusterListBuffer->getBufferData());
		if (dbg) {
			float lx = *reinterpret_cast<const float*>(&dbg[0]);
			float ly = *reinterpret_cast<const float*>(&dbg[1]);
			float lz = *reinterpret_cast<const float*>(&dbg[2]);
			float lw = *reinterpret_cast<const float*>(&dbg[3]);
			Info("[Nanite] slice0 LODBounds=({}, {}, {}, r={}) bLeaf={} NumChildren={} ChildStartRef={}",
				lx, ly, lz, lw, dbg[20], dbg[15], dbg[14]);
			uint32_t leafCount = 0, enabledCount = 0;
			for (uint32_t i = 0; i < 84; ++i) {
				if (dbg[i * 24 + 20] != 0) ++leafCount;
				if (dbg[i * 24 + 18] != 0) ++enabledCount;
			}
			Info("[Nanite] leaf slices = {} / 84, enabled slices = {}", leafCount, enabledCount);
			// 诊断哨兵：DebugBuffer[100]（偏移 100*24 uint32）
			const uint32_t* s100 = dbg + 100 * 24;
			Info("[Nanite] sentinel[100]: LODBounds.x=0x{:x} bLeaf=0x{:x} NumChildren=0x{:x}",
				s100[0], s100[20], s100[15]);
		}
		dbgDumpLogged = true;
	}

	//{
	//	const uint32_t* p = static_cast<const uint32_t*>(FrameBuffer->getBufferData());
	//	Info("[Nanite] after clear: p[0]=0x{:x} p[1]=0x{:x} (pixel0: low=0x{:x} high=0x{:x})",
	//		p[0], p[1], p[0], p[1]);
	//}

	// --- 3. Nanite 渲染（同一执行位置完成选择和渲染）---
	uint32_t total = GetEnabledClusterCount();
	static bool diagLogged = false;
	if (!diagLogged) {
		Info("[Nanite] selected clusters = {}", total);
		diagLogged = true;
	}
	if (total == 0) return;
	// 渲染必须用独立的新命令列表：同一 RHIComputeCommandList 复用会导致
	// 第二个 End 落在已提交的 page 上被静默丢弃（page 只能提交一次）。
	//Info("[Nanite] render: dispatch submitted");
	{
		FISIR::RHIComputeCommandList  cmdlist(rhi);
		cmdlist.SetPipelineState(NaniteRenderPipeline);
		cmdlist.SetResourcePack(NaniteRenderResourcePack);
		cmdlist.dispatch((total + 63) / 64, 1, 1);
		cmdlist.End(ClusterSelectionFence);
	}
	ClusterSelectionFence->wait();
	ClusterSelectionFence->reset();
	//{
	//	const uint32_t* p = static_cast<const uint32_t*>(FrameBuffer->getBufferData());
	//	// 找一个已知被三角形覆盖的像素，比如屏幕中心
	//	uint32_t cx = NANITE_RT_WIDTH / 2, cy = NANITE_RT_HEIGHT / 2;
	//	uint32_t idx = (cy * NANITE_RT_WIDTH + cx) * 2;
	//	Info("[Nanite] after render center: p[0]=0x{:x} p[1]=0x{:x}", p[idx], p[idx + 1]);
	//}
	//{
	//	uint64_t* p = static_cast<uint64_t*>(GetFrameBuffer()->getBufferData());
	//	for (uint32_t i = 0; i < 1024; ++i) {
	//		p[i] = (uint64_t(0x00000000u) << 32) | uint64_t(0x0000FF00u);  // 深度0，红色
	//	}
	//}

	// 诊断：统计渲染后颜色槽非零像素数，判断渲染是否真正写出
	static bool renderDiagLogged = false;
	if (!renderDiagLogged) {
		const uint32_t* p = static_cast<const uint32_t*>(FrameBuffer->getBufferData());
		uint32_t nonZero = 0;
		for (uint32_t i = 0; i < NANITE_RT_WIDTH * NANITE_RT_HEIGHT; ++i) {
			if (p[i * 2 + 0] != 0) ++nonZero;   // 颜色槽（高 32 位）
		}
		Info("[Nanite] render wrote {} / {} non-zero color pixels", nonZero, NANITE_RT_WIDTH * NANITE_RT_HEIGHT);
		renderDiagLogged = true;
		for (int i = 0; i < 5; ++i) {
			int base = (32 + i * 32) / 4;   // = 8 + i * 8 （uint32 索引）
			Info("[Nanite] cluster{}: cid=0x{:x} cb=0x{:x} w0=0x{:x} w4=0x{:x} w8=0x{:x} w12=0x{:x}",
				i, p[base + 0], p[base + 1], p[base + 2], p[base + 3], p[base + 4], p[base + 5]);
		}
		float px = *reinterpret_cast<const float*>(&p[68 / 4]);
		float py = *reinterpret_cast<const float*>(&p[72 / 4]);
		float pz = *reinterpret_cast<const float*>(&p[76 / 4]);
		float cx = *reinterpret_cast<const float*>(&p[80 / 4]);
		float cy = *reinterpret_cast<const float*>(&p[84 / 4]);
		float cz = *reinterpret_cast<const float*>(&p[88 / 4]);
		float cw = *reinterpret_cast<const float*>(&p[92 / 4]);
		Info("[Nanite] p0=({},{},{}) clip0=({},{},{},{}) ndc=({},{})", px, py, pz, cx, cy, cz, cw, cx / cw, cy / cw);
	}
}

void DestroyClusterResource(FISIR::DynamicRHI* rhi) {
	if (ClusterSelectionFence) {
		rhi->RHIDestroyFence(ClusterSelectionFence);
		ClusterSelectionFence = nullptr;
	}

	if (ClusterSelectionBuffer) {
		rhi->RHIDestroyBuffer(ClusterSelectionBuffer);
		ClusterSelectionBuffer = nullptr;
	}

	if (ClusterDataBuffer) {
		rhi->RHIDestroyBuffer(ClusterDataBuffer);
		ClusterDataBuffer = nullptr;
	}

	if (FrameBuffer) {
		rhi->RHIDestroyBuffer(FrameBuffer);
		FrameBuffer = nullptr;
	}
	if (FrameLock) {
		rhi->RHIDestroyBuffer(FrameLock);
		FrameLock = nullptr;
	}
	if (RenderParamsBuffer) {
		rhi->RHIDestroyBuffer(RenderParamsBuffer);
		RenderParamsBuffer = nullptr;
	}

	rhi->RHIDestroyResourcePack(ClusterSelectionResourcePack);
	rhi->RHIDestroyResourcePack(NaniteRenderResourcePack);
	rhi->RHIDestroyResourcePack(ClearScreenResourcePack);
}

FISIR::RHIBuffer* GetClusterSelectionBuffer() {
	return ClusterSelectionBuffer;
}

FISIR::RHIBuffer* GetClusterDataBuffer() {
	return ClusterDataBuffer;
}

uint32_t* GetEnabledClusterList() {
	return static_cast<uint32_t*>(EnableClusterListBuffer->getBufferData());
}

uint32_t GetEnabledClusterCount() {
	uint32_t* p = static_cast<uint32_t*>(EnableClusterListBuffer->getBufferData());
	return p ? p[0] : 0;
}

void SetClusterSelectionBuffer(FISIR::DynamicRHI* rhi) {
	//load BVH File Data
	FILE* fileBVH = nullptr;
	size_t sizeBVH = 0;
	fopen_s(&fileBVH, "Res/mitsuba.bvh", "rb");
	if (!fileBVH) { Error("Failed to open Res/mitsuba.bvh (working dir?)"); return; }
	fseek(fileBVH, 0, SEEK_END);
	sizeBVH = ftell(fileBVH);
	fseek(fileBVH, 0, SEEK_SET);
	unsigned char* data = new unsigned char[sizeBVH];
	fread(data, 1, sizeBVH, fileBVH);
	fclose(fileBVH);

	//load Mesh File Data
	FILE* fileMesh = nullptr;
	size_t sizeMesh = 0;
	fopen_s(&fileMesh, "Res/mitsuba.nanitemesh", "rb");
	if (!fileMesh) { Error("Failed to open Res/mitsuba.nanitemesh (working dir?)"); delete[] data; return; }
	fseek(fileMesh, 0, SEEK_END);
	sizeMesh = ftell(fileMesh);
	fseek(fileMesh, 0, SEEK_SET);
	unsigned char* meshData = new unsigned char[sizeMesh];
	fread(meshData, 1, sizeMesh, fileMesh);
	fclose(fileMesh);
	
	// 直接写入 host-visible 缓冲（仿 BunnyPBR），彻底绕开 transfer→compute 异队列族的
	// 跨队可见性问题。此处为初始化阶段、主循环尚未开始，CPU 写入完成后 compute 才读取，
	// 且缓冲为 host-coherent，无需 flush。
	memcpy(ClusterSelectionBuffer->getBufferData(), data, sizeBVH);
	memcpy(ClusterPageDataBuffer->getBufferData(), meshData, sizeMesh);

	// 诊断：读回验证写入是否确实落到 GPU 可读的映射内存
	const uint32_t* bvh = static_cast<const uint32_t*>(ClusterSelectionBuffer->getBufferData());
	const uint32_t* mesh = static_cast<const uint32_t*>(ClusterPageDataBuffer->getBufferData());
	Info("[Nanite] upload check: BVH ptr=0x{:x} [0..3]={} {} {} {} ; Mesh ptr=0x{:x} [0..3]={} {} {} {}",
		(size_t)bvh, bvh ? bvh[0] : 0, bvh ? bvh[1] : 0, bvh ? bvh[2] : 0, bvh ? bvh[3] : 0,
		(size_t)mesh, mesh ? mesh[0] : 0, mesh ? mesh[1] : 0, mesh ? mesh[2] : 0, mesh ? mesh[3] : 0);

	Info("[Nanite] nanitemesh[144..176] = 0x{:x} 0x{:x} 0x{:x} 0x{:x} 0x{:x} 0x{:x} 0x{:x} 0x{:x}",
		((uint32_t*)meshData)[36], ((uint32_t*)meshData)[37], ((uint32_t*)meshData)[38], ((uint32_t*)meshData)[39],
		((uint32_t*)meshData)[40], ((uint32_t*)meshData)[41], ((uint32_t*)meshData)[42], ((uint32_t*)meshData)[43]);
	delete[] data;
	delete[] meshData;
}

InputData& getInputData() {
	return *static_cast<InputData*>(InputDataBuffer->getBufferData());
}

FISIR::RHIBuffer* GetFrameBuffer() {
	return FrameBuffer;
}

RenderParams& getRenderParams() {
	return *static_cast<RenderParams*>(RenderParamsBuffer->getBufferData());
}

FISIR::RHIResourcePackResult& GetClusterSelectionResourcePack(){
	return ClusterSelectionResourcePack;
}
