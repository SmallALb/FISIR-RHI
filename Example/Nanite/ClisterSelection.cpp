#include "ClusterSelection.h"
#include "ShaderComplier.h"
#include "RHIFence.h"
#include "RHIFrameBuffer.h"
#include "RHISampler.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <string>
#include <iterator>
#include <vector>

// 数据来源：NANITE_OBJ_PATH 非空时导入模型、用 NaniteBuilder 现场生成簇与 BVH。
#include "OBJLoader.h"
#include "NaniteBuilder.h"

// 从磁盘读取整个文件为字符串（二进制读取，保留原始 UTF-8 字节）
static std::string LoadFileText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Error("Failed to open shader file: {}", path);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static FISIR::RHIShader* NaniteRenderComputeShader = nullptr;
static FISIR::RHIShader* ClearScreenComputeShader = nullptr;
static FISIR::RHIShader* FrameBufferWriteComputeShader = nullptr;   // VisBuffer → FrameBuffer 解析
static FISIR::RHIShader* HZBBuildComputeShader = nullptr;   // 层号走 push constant，一条管线即可

static FISIR::RHIBuffer* ClusterSelectionBuffer = nullptr;
static FISIR::RHIBuffer* ClusterPageDataBuffer = nullptr;
static FISIR::RHIBuffer* ClusterDataBuffer = nullptr;  // outbuffer：簇选择结果
static FISIR::RHIBuffer* InputDataBuffer = nullptr;
static FISIR::RHIBuffer* EnableClusterListBufferSw = nullptr;   // u3：[0]簇数 [1]三角形数 [2..]clusterID
static FISIR::RHIBuffer* EnableClusterListBufferHw = nullptr;   // u6：[0]簇数 [1..]clusterID
static FISIR::RHIBuffer* SwTriListBuffer = nullptr;             // u7：软光栅三角形工作列表

static FISIR::RHIBuffer* FrameBuffer = nullptr;         // u4：逐像素 颜色|深度（8B/px）
static FISIR::RHIBuffer* RenderParamsBuffer = nullptr;  // b5：RenderParams
// u8：VisBuffer（每像素一个 64 位打包字 = 高 32 位深度 | 低 32 位载荷 (clusterID<<8)|triIndex）。
// 软光栅与硬光栅只写这里（64 位原子 min 决出最近片元），再由 FrameBufferWritePipeline 解析成 FrameBuffer。
static FISIR::RHIBuffer* VisBuffer = nullptr;
// HZB（深度金字塔）：L0 = 上一帧深度，L1..L7 = 逐级 2×2 min。Selection 用它做遮挡剔除。
static FISIR::RHIBuffer* HzbBuffer = nullptr;

// ── BVH 下降用的无锁队列（GPU 写、GPU 读）与去重表 ──────────────────────
// 两份队列 ping-pong：第 k 轮读 [k&1]、写 [!(k&1)]，主机知道轮次奇偶就能换绑资源包，
// 不需要把轮次传进着色器。布局：[0] = 本层条目数，[1..] = 条目（节点下标）。
// 都是 host-visible：种子（根节点）与帧号由 CPU 直接写，和现有的列表/间接缓冲一致。
static FISIR::RHIBuffer* BvhQueueBuffers[2] = {};
static FISIR::RHIBuffer* BvhVisitedBuffer = nullptr;   // [0] = 帧号，[1+node] = 最近访问它的帧号
static uint32_t g_BvhQueueCapacity = 0;                // 每份队列能放多少条目
static uint32_t g_BvhFrameEpoch = 0;                   // 每帧 +1，当去重的“代”
static std::vector<uint32_t> g_BvhRootNodes;           // DAG 的根（入度 0），下降的种子

#define NANITE_QUEUE_HEADER 1u                          // 与 ClusterSelection.slang 同名同值
// 沿 BVH 下降的轮数上界（= BVH 最大深度）。**有界**是刻意的：不做 persistent thread + 活跃计数
// 那套（终止条件写错就是 GPU 死循环、窗口卡死），分层 BFS 轮数封顶就不可能挂住。
static constexpr uint32_t kNaniteBvhMaxLevels = 16;

// 唯一的簇选择路径：沿 BVH 下降（无锁队列 ping-pong）。平扫「一线程一槽」已移除。

static FISIR::RHIPipeline* NaniteRenderPipeline = nullptr;   // 软光栅（compute）
static FISIR::RHIPipeline* ClearScreenPipeline = nullptr;    // 清 FrameBuffer + VisBuffer
static FISIR::RHIPipeline* FrameBufferWritePipeline = nullptr;   // VisBuffer → FrameBuffer（着色）
static FISIR::RHIPipeline* HzbBuildPipeline = nullptr;   // 建塔：层号走 push constant
static FISIR::RHIPipeline* BvhTraversePipeline = nullptr;                 // 沿 BVH 下降（无锁队列）
static FISIR::RHIResourcePackResult ClusterSelectionResourcePack;
static FISIR::RHIResourcePackResult NaniteRenderResourcePack;
static FISIR::RHIResourcePackResult ClearScreenResourcePack;
static FISIR::RHIResourcePackResult FrameBufferWriteResourcePack;
static FISIR::RHIResourcePackResult HzbBuildResourcePack;
static FISIR::RHIResourcePackResult BvhTraversePacks[2];   // [0]: in=q0 out=q1；[1]: in=q1 out=q0

// ── 硬光栅资源（只做几何光栅化，像素由 PS 写进 FrameBuffer）──
static FISIR::RHIShader* NaniteRenderVSShader = nullptr;   // NaniteRender.slang 的 mainVS
static FISIR::RHIShader* NaniteRenderPSShader = nullptr;   // NaniteRender.slang 的 mainPS
static FISIR::RHIBuffer* IndirectDrawBuffer = nullptr;      // u5：硬光栅每条选中簇 16B DrawIndirect
static FISIR::RHIPipeline* NaniteGraphicsPipeline = nullptr;
static FISIR::RHITexture* OffscreenColorTexture = nullptr;  // 离屏颜色附件（占位，不再被采样）
static FISIR::RHITexture* OffscreenDepthTexture = nullptr;  // 离屏深度附件（占位，管线已关闭深度读写）
static FISIR::RHIRenderPass* OffscreenRenderPass = nullptr;
static FISIR::RHIFrameBuffer* OffscreenFrameBuffer = nullptr;
static FISIR::RHISampler* OffscreenSampler = nullptr;

// ── 帧同步信号量 ──────────────────────────────────────────────
// 帧内依赖全部由信号量在 GPU 侧串联：clear → {soft, hard} → 解析 → present，
// select → {soft, hard}。binary 信号量一次 signal 只能被一次 wait 消费，而两个光栅器
// 要并行（各画各的列表），所以 clear/select 各自信号两次，软/硬各拿一份。
static FISIR::RHISemaphore* ClearDoneSoftSemaphore  = nullptr;   // 清屏 → 软光栅
static FISIR::RHISemaphore* ClearDoneHardSemaphore  = nullptr;   // 清屏 → 硬光栅
static FISIR::RHISemaphore* SelectDoneSoftSemaphore = nullptr;   // 簇选择 → 软光栅
static FISIR::RHISemaphore* SelectDoneHardSemaphore = nullptr;   // 簇选择 → 硬光栅
static FISIR::RHISemaphore* SoftDoneSemaphore       = nullptr;   // 软光栅 → 解析
static FISIR::RHISemaphore* RenderDoneSemaphore     = nullptr;   // 硬光栅 → 解析
static FISIR::RHISemaphore* FrameBufferWriteDoneSemaphore = nullptr;   // 解析 → 呈现
static FISIR::RHISemaphore* ClearDoneHzbSemaphore   = nullptr;   // 清屏 → HZB 建塔
static FISIR::RHISemaphore* HzbDoneSemaphore        = nullptr;   // HZB 建塔 → 簇选择
static FISIR::RHISemaphore* FrameDoneSemaphore      = nullptr;   // 呈现 → 下帧帧首 CPU 等待（timeline）



static const char* NANITE_OBJ_PATH = nullptr;   // 例："Res/bunny_hi.obj"
static float NANITE_OBJ_SCALE = 30.0f;


// ── 数据来源：内置 mitsuba 或现场导入的模型 ──────────────────────────────
// NANITE_OBJ_PATH（ClusterSelection.h）为空 → 一律用 Res/mitsuba.*（默认，已验证）。
// 非空 → 尝试导入并用 NaniteBuilder 生成簇+BVH；打不开或构建失败同样回退 mitsuba。
// 规模字段与几个缓冲尺寸都取自这里，所以 Init / Upload 都读 g_* 而非硬编码的 mitsuba 数。
static uint32_t g_ClusterCount = NANITE_MAX_CLUSTERS;
static uint32_t g_BVHNodes     = 21;   // mitsuba.bvh = 4368 B / 208 B(每节点)
static uint32_t g_Slices       = 84;   // 21 节点 × 4 子槽
static std::vector<uint8_t> g_BuiltBVH;    // 非空 = 用 builder 的结果，不读文件
static std::vector<uint8_t> g_BuiltMesh;

static void TryBuildFromObj() {
	if (!NANITE_OBJ_PATH) return;                  // 未指定模型 → mitsuba
	FISIR::ObjMesh mesh = FISIR::LoadObj(NANITE_OBJ_PATH);
	if (!mesh.loaded) { Error("[Nanite] failed to open {} → back to mitsuba", NANITE_OBJ_PATH); return; }

	// 缩放：LoadObj 已把模型归一化到最大边长 2.0，这里统一放大到与 mitsuba 同量级
	// （bunny 归一化后包围球半径仅 1.346，mitsuba 是 248，所以看着会很小）。
	// 必须在 BuildNaniteData 之前 —— 包围球与 LOD 误差都由位置算出，缩放要一起进去；
	// 只有顶点被放大而 bounds/error 没放大，簇剔除就会全错。
	for (float& v : mesh.positions) v *= -NANITE_OBJ_SCALE;

	FISIR::NaniteBuildResult r = FISIR::BuildNaniteData(mesh.positions, mesh.indices, 128);
	if (r.bvh.empty() || r.mesh.empty()) { Error("[Nanite] failed to build → back to mitsuba"); return; }
	g_ClusterCount = r.clusterCount;
	g_BVHNodes     = r.totalBVHNodes;
	g_Slices       = r.totalSlices;
	g_BuiltBVH.swap(r.bvh);
	g_BuiltMesh.swap(r.mesh);
	Info("[Nanite] imported {} (scale {}) -> {} clusters / {} nodes / {} slices",
		NANITE_OBJ_PATH, NANITE_OBJ_SCALE, g_ClusterCount, g_BVHNodes, g_Slices);
}


void SetModelPath(const char* path, float scale) {
	NANITE_OBJ_PATH = path;
	NANITE_OBJ_SCALE = scale;
}

void InitClusterSelection(FISIR::DynamicRHI* rhi) {
	TryBuildFromObj();   // 必须在建缓冲与填 InputData 之前

		//Complie and Create Shader（从磁盘加载 Slang 源文件）
	// 簇选择只有一个入口点 mainTraverse（沿 BVH 下降），着色器源在下面和管线一起编。
	std::string csSource = LoadFileText("Shader/ClusterSelection.slang");

	std::string RenderSource = LoadFileText("Shader/NaniteRender.slang");
	FISIR::ShaderComplier* renderCompiler = new FISIR::ShaderComplier();
	renderCompiler->compileShader(RenderSource.data(), RenderSource.size(), "mainRender", "cs_6_7");
	NaniteRenderComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainRender", renderCompiler->getShaderData(), renderCompiler->getShaderDataSize());
	if (!NaniteRenderComputeShader) Error("[Nanite] NaniteRenderComputeShader creation FAILED!");

	// 硬光栅 VS/PS：与软光栅 compute 同源（NaniteRender.slang 新增入口 mainVS/mainPS），
	// VS 复用顶部的 clusterPagesBuffer(u1)/EnableClusterList(u3)/RenderParams(b6)。
	FISIR::ShaderComplier* vsCompiler = new FISIR::ShaderComplier();
	vsCompiler->compileShader(RenderSource.data(), RenderSource.size(), "mainVS", "vs_6_0");
	NaniteRenderVSShader = rhi->RHICreateShader(FISIR::ShaderTYP::__VERTEXSHADER__, "mainVS", vsCompiler->getShaderData(), vsCompiler->getShaderDataSize());
	if (!NaniteRenderVSShader) Error("[Nanite] NaniteRenderVSShader creation FAILED!");

	FISIR::ShaderComplier* psCompiler = new FISIR::ShaderComplier();
	psCompiler->compileShader(RenderSource.data(), RenderSource.size(), "mainPS", "ps_6_0");
	NaniteRenderPSShader = rhi->RHICreateShader(FISIR::ShaderTYP::__FRAGMENTSHADER__, "mainPS", psCompiler->getShaderData(), psCompiler->getShaderDataSize());
	if (!NaniteRenderPSShader) Error("[Nanite] NaniteRenderPSShader creation FAILED!");

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
	// mesh 数据同样 host-visible 直接上传。硬光栅后此缓冲被 compute（簇选择）与
	// graphics（VS 读顶点）两个队列族同时读，须并发共享（异队列族 EXCLUSIVE 需所有权转移）。
	FISIR::BufferInfo clusterPageDataBufferInfo{
		.size = 40 * 1024 * 1024,
		.bufferlayout = FISIR::RBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		.concurrentSharing = true,
	};
	ClusterPageDataBuffer = rhi->RHICreateBuffer(clusterPageDataBufferInfo);
	
	InputData iptData {
		.Position = {0.f, 0.f, 0.f, 0.f},
		.Direction = {0.f, 0.f, -1.f, 0.f},
		.LodScale = 1.f,
		.ZNear = 0.1f,
		.CountOfClusters = g_ClusterCount,
		.TotalBVHNodes = g_BVHNodes,
		.TotalSlices = g_Slices,
		.MaxClusters = g_ClusterCount,
	};

	//4.Create InputDataBuffer
	FISIR::BufferInfo cameraBufferInfo{
		.data_CPU = &iptData,
		.size = sizeof(InputData),
		.bufferlayout = FISIR::UniformBuffer | FISIR::TransferDstBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	InputDataBuffer = rhi->RHICreateBuffer(cameraBufferInfo);

	//5.Create EnableClusterListBuffer（软光栅/硬光栅各一条，见 shader 的 u3/u6）
	// 主机可见 + 相干：compute 写完计数与 clusterID 列表后，CPU 经 getBufferData()
	// 直接读回选中结果（WaitFrameGPUIdle() 之后读，相干内存保证可见性，比实际帧滞后一帧）。
	FISIR::BufferInfo enableClusterListBufferInfo{
		.data_CPU = nullptr,
		.size = 8 + (uint64_t)(g_ClusterCount + 1) * 4,   // 软光栅多一格：[0]簇数 [1]三角形数 [2..]ID
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		.concurrentSharing = true,   // compute 写、graphics VS 读，异队列族需并发共享
	};
	EnableClusterListBufferSw = rhi->RHICreateBuffer(enableClusterListBufferInfo);
	EnableClusterListBufferHw = rhi->RHICreateBuffer(enableClusterListBufferInfo);

	//5b. 软光栅的三角形工作列表（u7）：容量 = 最大簇数 × 每簇三角形上限，每三角形 3 个 uint
	// (clusterID, 簇数据区字节偏移 cb, 簇内三角形序号)。device-local：写（Selection）与读
	// （软光栅）都在 compute 队列且由信号量排序，不需要主机可见（每帧几万条走 PCIe 反而慢）。
	// 帧计数器借用软光栅列表的 [1]。
	FISIR::BufferInfo swTriListBufferInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)g_ClusterCount * NANITE_MAX_CLUSTER_TRIANGLES * 3 * 4,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
	};
	SwTriListBuffer = rhi->RHICreateBuffer(swTriListBufferInfo);

	//6. 间接绘制缓冲（硬光栅用）：簇选择 compute 写入 DrawIndirect 参数（16B/簇），graphics 读取。
	// compute 写、graphics 读，异队列族并发共享；host-visible 以便 CPU 每帧清零尾部残留项。
	FISIR::BufferInfo indirectDrawBufferInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)g_ClusterCount * 16,
		.bufferlayout = FISIR::IndirectBuffer | FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		.concurrentSharing = true,
	};
	IndirectDrawBuffer = rhi->RHICreateBuffer(indirectDrawBufferInfo);
	memset(IndirectDrawBuffer->getBufferData(), 0, (uint64_t)g_ClusterCount * 16);

	//7. 渲染用：FrameBuffer（8B/px）、RenderParams
	const uint32_t pixelCount = NANITE_RT_WIDTH * NANITE_RT_HEIGHT;
	// 软光栅（compute）与硬光栅 PS（graphics）都写 FrameBuffer，呈现 pass（graphics）读；
	// 跨队列族共享用 CONCURRENT 免掉所有权转移，帧内可见性由信号量建立，帧间由帧首 CPU 等待。
	// device-local（CPU 不读不写）：每像素几十万次原子/写在主机内存上会走 PCIe，太贵。
	FISIR::BufferInfo frameBufferInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)pixelCount * 8,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
		.concurrentSharing = true,
	};
	FrameBuffer = rhi->RHICreateBuffer(frameBufferInfo);

	//7b. VisBuffer：软光栅（compute）与硬光栅 PS 都写、解析 pass（compute）读，
	// 同样跨队列族共享；device-local，逐像素几百万次原子在主机内存上会走 PCIe。
	// 每像素 8 字节 = 一个 64 位打包字（高 32 位深度、低 32 位载荷），
	// 软/硬两条路径都用 64 位原子 min 更新它 —— 深度与载荷同属一个字，不会串。
	FISIR::BufferInfo visBufferInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)pixelCount * 8,   // 每像素 1 个 uint64：深度 | 载荷
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
		.concurrentSharing = true,
	};
	VisBuffer = rhi->RHICreateBuffer(visBufferInfo);

	//7c. HZB（深度金字塔）：L0 由 ClearScreen 从上一帧 VisBuffer 抄出，L1..L7 由 HZBBuild 建。
	// 各层连续存放（索引 = 前几层 texel 数之和），Selection 端算个整数下标就能取任意层。
	FISIR::BufferInfo hzbBufferInfo{
		.data_CPU = nullptr,
		.size = GetHzbTexelCount(NANITE_RT_WIDTH, NANITE_RT_HEIGHT) * 4,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = FISIR::MemTypeDeviceLocal,
		.concurrentSharing = true,   // 清屏/建塔/选择都在 compute 族，仍与图形族共享以防万一
	};
	HzbBuffer = rhi->RHICreateBuffer(hzbBufferInfo);

	//7d. BVH 下降用的两份 ping-pong 队列 + 去重表（见文件头的常量说明）。
	// 容量：一个节点最多 push 4 个 link 子节点、且同一节点可能被多个父节点重复入队（去重在出队时做），
	// 所以按 4×节点数留余量；条目都是 uint，几千个节点也就几十 KB。
	g_BvhQueueCapacity = g_BVHNodes * 4u + 64u;
	for (uint32_t q = 0; q < 2; ++q) {
		FISIR::BufferInfo queueInfo{
			.data_CPU = nullptr,
			.size = (uint64_t)(NANITE_QUEUE_HEADER + g_BvhQueueCapacity) * 4,
			.bufferlayout = FISIR::RWBuffer,
			.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		};
		BvhQueueBuffers[q] = rhi->RHICreateBuffer(queueInfo);
	}
	{
		FISIR::BufferInfo visitedInfo{
			.data_CPU = nullptr,
			.size = (uint64_t)(1 + g_BVHNodes) * 4,
			.bufferlayout = FISIR::RWBuffer,
			.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		};
		BvhVisitedBuffer = rhi->RHICreateBuffer(visitedInfo);
	}
	if (!BvhQueueBuffers[0] || !BvhQueueBuffers[1] || !BvhVisitedBuffer)
		Error("[Nanite] BVH traversal queue creation FAILED!");

	RenderParams initParams {
		.VPMatrix = glm::mat4(1.0f),
		.screenSize = { (float)NANITE_RT_WIDTH, (float)NANITE_RT_HEIGHT },
		.ClearDepth = 1.0f,
		.ClearColor = 0xFF2E2A26,  // 低 24 位 = R|G<<8|B<<16（与 FrameBuffer / 呈现一致）
		.ColorBlock = 1,       // 0 = Lambert；1 = 色块（面板上可切）
		.FarPlane = 2000.0f,
	};
	FISIR::BufferInfo renderParamsInfo{
		.data_CPU = &initParams,
		.size = sizeof(RenderParams),
		.bufferlayout = FISIR::UniformBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	RenderParamsBuffer = rhi->RHICreateBuffer(renderParamsInfo);

	//Create Descriptor and Pipeline
	// ByteAddressBuffer 的 NonWritable 加在类型成员上，descriptor buffer 扩展不认，RBuffer(READ_ONLY) 读全 0。
	// 故 shader 里字节缓冲一律改用 RWByteAddressBuffer（READ_WRITE），并把所有资源重排到唯一 binding：
	// u0 BVH, u1 mesh, u2 clusterData, u3 软光栅列表, b4 InputData, u5 硬光栅间接参数,
	// u6 硬光栅列表, u7 软光栅三角形工作列表, b8 RenderParams, u9 HZB, u10/u11 队列, u12 去重表。
	FISIR::RHIPipelineDescribeInfo describeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u0 clusterSelectionBuffer
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u1 clusterPagesBuffer
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u2 clusterDataBuffer
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u3 软光栅列表
		{4, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b4 InputData
		{5, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u5 IndirectDrawBuffer
		{6, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u6 硬光栅列表
		{7, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u7 三角形工作列表
		{8, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b8 RenderParams（VPMatrix/screenSize）
		{9, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u9 HZB 深度金字塔
		{10, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},     // u10 BVH 队列（读）
		{11, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},     // u11 BVH 队列（写）
		{12, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},     // u12 去重表

	};


	// ── 沿 BVH 下降（compute）：无锁队列 ping-pong 的**唯一**选择路径 ──
	// in/out 靠两个资源包换绑（第 k 轮读 [k&1]、写 [!(k&1)]），所以不需要把轮次传进着色器。
	{
		std::string traverseSource = csSource;
		FISIR::ShaderComplier* traverseCompiler = new FISIR::ShaderComplier();
		traverseCompiler->compileShader(traverseSource.data(), traverseSource.size(), "mainTraverse", "cs_6_7");
		FISIR::RHIShader* traverseShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainTraverse",
			traverseCompiler->getShaderData(), traverseCompiler->getShaderDataSize());
		if (!traverseShader) Error("[Nanite] mainTraverse shader creation FAILED!");

		FISIR::RHIPipelineState traverseState {
			.describeInfo = describeInfo,
			.isComputePipeline = 1,
		};
		traverseState.Shaders[FISIR::__COMPUTESHADER__] = traverseShader;
		BvhTraversePipeline = rhi->RHICreatePipeline(traverseState);
		if (!BvhTraversePipeline) Error("[Nanite] BvhTraversePipeline creation FAILED!");

		BvhTraversePacks[0] = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterPageDataBuffer, ClusterDataBuffer, EnableClusterListBufferSw, InputDataBuffer, IndirectDrawBuffer, EnableClusterListBufferHw, SwTriListBuffer, RenderParamsBuffer, HzbBuffer, BvhQueueBuffers[0], BvhQueueBuffers[1], BvhVisitedBuffer });
		BvhTraversePacks[1] = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterPageDataBuffer, ClusterDataBuffer, EnableClusterListBufferSw, InputDataBuffer, IndirectDrawBuffer, EnableClusterListBufferHw, SwTriListBuffer, RenderParamsBuffer, HzbBuffer, BvhQueueBuffers[1], BvhQueueBuffers[0], BvhVisitedBuffer });
	}

	// 软光栅管线（compute）：u0 BVH(未用), u1 mesh, u2 clusterData(未用), u3 软光栅列表,
	// u4 FrameBuffer(未用，改写 VisBuffer), b5 RenderParams, u6 硬光栅列表(未用),
	// u7 三角形工作列表, u8 VisBuffer
	FISIR::RHIPipelineDescribeInfo renderDescribeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{4, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{5, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{6, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{7, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
		{8, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},
	};
	FISIR::RHIPipelineState renderPipelineState {
		.describeInfo = renderDescribeInfo,
		.isComputePipeline = 1,
	};
	renderPipelineState.Shaders[FISIR::__COMPUTESHADER__] = NaniteRenderComputeShader;
	NaniteRenderPipeline = rhi->RHICreatePipeline(renderPipelineState);

	// 软光栅 compute 与硬光栅 VS/PS 共用这个资源包（顺序 = binding）
	NaniteRenderResourcePack = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterPageDataBuffer, ClusterDataBuffer, EnableClusterListBufferSw, FrameBuffer, RenderParamsBuffer, EnableClusterListBufferHw, SwTriListBuffer, VisBuffer });

	// 清屏着色器：渲染前把 FrameBuffer 清为远深度 + 清屏色（GPU compute，替代 CPU std::fill）。
	// 绑定：u0 FrameBuffer、b1 RenderParams（复用 RenderParamsBuffer，与 NaniteRender 的 ClearDepth/ClearColor 同源）。
	std::string ClearSource = LoadFileText("Shader/ClearScreen.slang");
	FISIR::ShaderComplier* clearCompiler = new FISIR::ShaderComplier();
	clearCompiler->compileShader(ClearSource.data(), ClearSource.size(), "mainClear", "cs_6_7");
	ClearScreenComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainClear", clearCompiler->getShaderData(), clearCompiler->getShaderDataSize());
	if (!ClearScreenComputeShader) Error("[Nanite] ClearScreenComputeShader creation FAILED!");

	FISIR::RHIPipelineDescribeInfo clearDescribeInfo {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u0 FrameBuffer
		{1, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b1 RenderParams
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u2 VisBuffer
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u3 HZB L0
	};
	FISIR::RHIPipelineState clearPipelineState {
		.describeInfo = clearDescribeInfo,
		.isComputePipeline = 1,
	};
	clearPipelineState.Shaders[FISIR::__COMPUTESHADER__] = ClearScreenComputeShader;
	ClearScreenPipeline = rhi->RHICreatePipeline(clearPipelineState);
	if (!ClearScreenPipeline) Error("[Nanite] ClearScreenPipeline creation FAILED!");

	ClearScreenResourcePack = rhi->RHICreateResourcePack({ FrameBuffer, RenderParamsBuffer, VisBuffer, HzbBuffer });

	// ── HZB 建塔（compute）：把 ClearScreen 抄出来的上一帧深度逐级 2×2 取 min ──────
	// u0 HZB、b1 RenderParams（要 screenSize 才知各层尺寸）。
	// 层级必须有序（L(n) 读 L(n-1)），而同一次 dispatch 内没有跨工作组同步，所以一层一次
	// dispatch；「本轮建第几层」走 push constant（vkCmdPushConstants 每次调用立即改当前值，
	// 所以同一条命令列表里 7 次 dispatch 可以各推各的层号）。原先为传这个层号要 7 个入口点
	// 配 7 条管线；现在一个入口点、一条管线。页按录制顺序提交 ⇒ 层级天然有序。
	{
		// 与 HZBBuild.slang 的 HzbPushConstants 必须逐字节一致
		struct HzbPushConstants { uint32_t level; };

		std::string hzbSource = LoadFileText("Shader/HZBBuild.slang");
		FISIR::ShaderComplier* hzbCompiler = new FISIR::ShaderComplier();
		hzbCompiler->compileShader(hzbSource.data(), hzbSource.size(), "mainHzb", "cs_6_7");
		HZBBuildComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainHzb",
			hzbCompiler->getShaderData(), hzbCompiler->getShaderDataSize());
		if (!HZBBuildComputeShader) Error("[Nanite] HZBBuildComputeShader creation FAILED!");

		FISIR::RHIPipelineDescribeInfo hzbDescribeInfo {
			{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u0 HZB
			{1, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b1 RenderParams
		};
		FISIR::RHIPipelineState hzbPipelineState {
			.describeInfo = hzbDescribeInfo,
			.isComputePipeline = 1,
			.pushConstantRange = { FISIR::ComputeShaderStage, 0, (uint32_t)sizeof(HzbPushConstants) },
		};
		hzbPipelineState.Shaders[FISIR::__COMPUTESHADER__] = HZBBuildComputeShader;
		HzbBuildPipeline = rhi->RHICreatePipeline(hzbPipelineState);
		if (!HzbBuildPipeline) Error("[Nanite] HzbBuildPipeline creation FAILED!");

		HzbBuildResourcePack = rhi->RHICreateResourcePack({ HzbBuffer, RenderParamsBuffer });
	}

	// ── 解析管线（compute）：把 VisBuffer 解析成 FrameBuffer ──────────────
	// u0 VisBuffer、u1 mesh（Lambert 要取顶点）、u2 FrameBuffer、b3 RenderParams。
	// 一个线程一个像素、不用原子：可见性已经在 VisBuffer 的 64 位原子深度里决出来了。
	{
		std::string writeSource = LoadFileText("Shader/FrameBufferWrite.slang");
		FISIR::ShaderComplier* writeCompiler = new FISIR::ShaderComplier();
		writeCompiler->compileShader(writeSource.data(), writeSource.size(), "mainWriteFrameBuffer", "cs_6_7");
		FrameBufferWriteComputeShader = rhi->RHICreateShader(FISIR::ShaderTYP::__COMPUTESHADER__, "mainWriteFrameBuffer",
			writeCompiler->getShaderData(), writeCompiler->getShaderDataSize());
		if (!FrameBufferWriteComputeShader) Error("[Nanite] FrameBufferWriteComputeShader creation FAILED!");

		FISIR::RHIPipelineDescribeInfo writeDescribeInfo {
			{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u0 VisBuffer
			{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u1 clusterPagesBuffer
			{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u2 FrameBuffer
			{3, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::ComputeShaderStage}, // b3 RenderParams
		};
		FISIR::RHIPipelineState writePipelineState {
			.describeInfo = writeDescribeInfo,
			.isComputePipeline = 1,
		};
		writePipelineState.Shaders[FISIR::__COMPUTESHADER__] = FrameBufferWriteComputeShader;
		FrameBufferWritePipeline = rhi->RHICreatePipeline(writePipelineState);
		if (!FrameBufferWritePipeline) Error("[Nanite] FrameBufferWritePipeline creation FAILED!");

		FrameBufferWriteResourcePack = rhi->RHICreateResourcePack({ VisBuffer, ClusterPageDataBuffer, FrameBuffer, RenderParamsBuffer });
	}

	// ── 硬光栅离屏渲染目标 + 渲染通道 + 帧缓冲 ─────────────────────
	// 传统光栅把结果写入离屏颜色纹理，呈现 PS 再采样它（enableTextureInput）。
	FISIR::TextureInfo colorTexInfo{
		.size = {NANITE_RT_HEIGHT, NANITE_RT_WIDTH, 1},   // TextureSize 字段序为 {height, width, depth}
		.colorType = FISIR::TextureCOLORType::RGBA_8,
		.type = FISIR::TextureType::TEXTURE2D,
		.useFor = FISIR::TextureUseForColorAttachment | FISIR::TextureUseForShaderReadOnly,
		.mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,
	};
	OffscreenColorTexture = rhi->RHICreateTexture(colorTexInfo);

	// 深度附件（硬光栅深度测试用；每帧由 render pass 清成远平面）
	FISIR::TextureInfo depthTexInfo{
		.size = {NANITE_RT_HEIGHT, NANITE_RT_WIDTH, 1},
		.colorType = FISIR::TextureCOLORType::Depth24_Stencil8,
		.type = FISIR::TextureType::TEXTURE2D,
		.useFor = FISIR::TextureUseForDepthStencilAttachment,
		.mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,
	};
	OffscreenDepthTexture = rhi->RHICreateTexture(depthTexInfo);

	FISIR::ColorEntry colorEntry{ {.loadOp = FISIR::RenderTargetLoadAction::Clear, .storeOp = FISIR::RenderTargetStoreAction::Store, .dstLayout = FISIR::TextureLayout::ShaderReadOnlyOptimal, .colorType = FISIR::TextureCOLORType::RGBA_8, .sampleCount = 0} };
	FISIR::DepthStencilEntry depthStencilEntry{ .sampleCount = 0, .dstLayout = FISIR::TextureLayout::DepthStencilAttachmentOptimal, .exeit = true };
	depthStencilEntry.depthAction.setDWAndSW(true, true);
	FISIR::SubPassInfo subPassInfo{ .ColorEntryMask = 1, .UseDepthStencil = true, .ReadDepthAsInput = false };
	FISIR::RHIRenderPassInfo renderPassInfo({ {0, colorEntry} }, depthStencilEntry, { subPassInfo });
	OffscreenRenderPass = rhi->RHICreateRenderPass(renderPassInfo);
	OffscreenFrameBuffer = rhi->RHICreateFrameBuffer(NANITE_RT_WIDTH, NANITE_RT_HEIGHT, { OffscreenColorTexture, OffscreenDepthTexture }, renderPassInfo);

	FISIR::SamplerInfo samplerInfo{};
	OffscreenSampler = rhi->RHICreateSampler(samplerInfo);

	// ── 硬光栅管线（VS + PS，无顶点/索引缓冲，靠 SV_VertexID/SV_InstanceID）──
	// 复用同一份 NaniteRenderResourcePack（顺序 = binding 0..8）。
	// PS 只写 VisBuffer（u8），所以 u8 必须挂到片元阶段；VS 用 u1(顶点)、u6(硬光栅列表)、
	// b5(VP 矩阵)。u2/u3/u4/u7 两条路径都不用，但 layout 必须与资源包的绑定数一致才能 bind。
	FISIR::RHIPipelineDescribeInfo graphicsDescribe {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{4, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{5, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{6, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{7, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
		{8, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage | FISIR::RHIUsingStage::FragmentShaderStage},
	};
	FISIR::RHIPipelineState graphicsState{
		.describeInfo = graphicsDescribe,
		.topologyType = FISIR::TopologyType::Triangle,
		.rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
		// 深度测试/写入全关：深度由 PS 写进 FrameBuffer（与软光栅共用同一套原子深度），
		// 离屏的颜色/深度附件只是占位。
		.depthStencilState = { false, false, false, 0.0f, 1.0f, FISIR::_Less_ },
		.colorblendState = { .UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_) },
		.renderpass = OffscreenFrameBuffer->getFrameRenderPass(),
	};
	graphicsState.Shaders[FISIR::__VERTEXSHADER__] = NaniteRenderVSShader;
	graphicsState.Shaders[FISIR::__FRAGMENTSHADER__] = NaniteRenderPSShader;
	NaniteGraphicsPipeline = rhi->RHICreatePipeline(graphicsState);
	if (!NaniteGraphicsPipeline) Error("[Nanite] NaniteGraphicsPipeline creation FAILED!");

	// ── 帧同步信号量 ──
	// 依赖链：clear → {soft, hard}，select → {soft, hard}，{soft, hard} → 解析 → present。
	// 两个光栅器互不依赖（各画各的列表、写同一个 VisBuffer），可并行。
	ClearDoneSoftSemaphore			= rhi->RHICreateSemaphore("NaniteClearDoneSoftSemaphore");
	ClearDoneHardSemaphore			= rhi->RHICreateSemaphore("NaniteClearDoneHardSemaphore");
	SelectDoneSoftSemaphore			= rhi->RHICreateSemaphore("NaniteSelectDoneSoftSemaphore");
	SelectDoneHardSemaphore			= rhi->RHICreateSemaphore("NaniteSelectDoneHardSemaphore");
	SoftDoneSemaphore				= rhi->RHICreateSemaphore("NaniteSoftDoneSemaphore");
	RenderDoneSemaphore				= rhi->RHICreateSemaphore("NaniteRenderDoneSemaphore");
	FrameBufferWriteDoneSemaphore	= rhi->RHICreateSemaphore("NaniteFrameBufferWriteDoneSemaphore");
	ClearDoneHzbSemaphore			= rhi->RHICreateSemaphore("NaniteClearDoneHzbSemaphore");
	HzbDoneSemaphore				= rhi->RHICreateSemaphore("NaniteHzbDoneSemaphore");
	// 等待阶段决定该信号量的 wait 挂在管线的哪一级：
	// 软光栅 / 解析都是 compute → ComputeShaderStage；
	// 硬光栅是 graphics，等待点须早于 DrawIndirect/VS → TOP_OF_PIPE 最保险；
	// 解析完成的信号量由呈现 pass 的片元着色器等待（它读 FrameBuffer）。
	ClearDoneSoftSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	ClearDoneHzbSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	HzbDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	SelectDoneSoftSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	SoftDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	RenderDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	ClearDoneHardSemaphore->setWaitingStage(FISIR::RHIUsingStage::PipelinTopStage);
	SelectDoneHardSemaphore->setWaitingStage(FISIR::RHIUsingStage::PipelinTopStage);
	FrameBufferWriteDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::FragmentShaderStage);

	// 帧完成信号量必须是时间线类型：binary 信号量无法在 CPU 侧 wait()。
	// 值从 1 起（见 VulkanSemaphore 的 nextSignalValue/nextWaitValue 初值），
	// 每帧末端 signal 一次、下帧帧首 wait 一次，严格配对。
	FrameDoneSemaphore = rhi->RHICreateSemaphore("NaniteFrameDoneSemaphore", FISIR::FenceType::TimeLine);

	if (!ClearDoneSoftSemaphore || !ClearDoneHardSemaphore || !SelectDoneSoftSemaphore ||
	    !SelectDoneHardSemaphore || !SoftDoneSemaphore || !RenderDoneSemaphore ||
	    !FrameBufferWriteDoneSemaphore || !ClearDoneHzbSemaphore || !HzbDoneSemaphore || !FrameDoneSemaphore)
		Error("[Nanite] Frame sync semaphore creation FAILED!");
}

void EndFramePresentPass(FISIR::RHIRenderCommandList& cmdList, const FISIR::SwapChainGetImageInfo& info) {
	// waits  ：acquire 信号量（等交换链图像可用）+ FrameBufferWriteDone（等解析 pass 写完 FrameBuffer）
	// signals：renderFinish（供呈现等待）+ FrameDone（供下帧帧首 CPU 等待）
	// fence  ：info.finishFence 不用于等待 GPU，而是「提交握手」：调用方在本函数之后等它被
	//          RHI 线程提交（见 Main.cpp），只作节流。呈现已指令化 ——
	//          cmdList.Present(swapchain, frameID) 写在本次 End 之前，由 RHI 线程在本页
	//          vkQueueSubmit 之后执行 vkQueuePresentKHR，因此 renderFinish 必然已有对应提交。
	cmdList.End(info.finishFence,
		{ info.avaliable, FrameBufferWriteDoneSemaphore },
		{ info.renderFinish, FrameDoneSemaphore });
}

void WaitFrameGPUIdle() {
	static bool start = false;
	if (start) FrameDoneSemaphore->wait();
	start = true;
}

void ExecuteClusterSelectionPass(FISIR::DynamicRHI* rhi) {
	// 每帧指令流程：Clear → Selection → { 软光栅(compute) ‖ 硬光栅(graphics) }
	//              → FrameBuffer 写入（解析 VisBuffer）→ 呈现。
	// 两个光栅器各画各的选择列表、都只写 VisBuffer（内部做原子深度），彼此没有依赖；
	// 解析 pass 同时等两者的完成信号量，把 VisBuffer 着色成 FrameBuffer，呈现再等解析。
	// 各 pass 独立命令列表（page 只能提交一次），依赖全部由信号量在 GPU 侧建立。
	// 前置条件：调用方已执行 WaitFrameGPUIdle()，上一帧 GPU 工作已完成。

	// --- 1. 清屏（compute 把 FrameBuffer 清成远深度 + 背景色）---
	// 软/硬各信号一次：binary 信号量一次 signal 只能被一次 wait 消费。
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetPipelineState(ClearScreenPipeline);
		cmdlist.SetResourcePack(ClearScreenResourcePack);
		cmdlist.dispatch((NANITE_RT_WIDTH + 15) / 16, (NANITE_RT_HEIGHT + 15) / 16, 1);
		cmdlist.End(nullptr, {}, { ClearDoneSoftSemaphore, ClearDoneHardSemaphore, ClearDoneHzbSemaphore });
	}

	// --- 1b. HZB 建塔：把 ClearScreen 抄出来的上一帧深度逐级 2×2 取 min（L1..L7）---
	// 一层一次 dispatch（层级必须有序）；每层的工作量按该层尺寸派发，越界线程自己提前退出。
	// ⚠ 层与层之间**必须插屏障**：同一个命令列表里连续 dispatch，compute 的写后读不会自动
	// 保证可见性（Vulkan 只在提交顺序上有保证，内存可见性要显式 barrier）。少了这一步，
	// 上一层的结果可能还没落地就被下一层读走 → 金字塔随机缺级 → 遮挡剔除结果逐帧跳变。
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetResourcePack(HzbBuildResourcePack);
		cmdlist.SetPipelineState(HzbBuildPipeline);
		for (uint32_t level = 1; level <= NANITE_HZB_MAX_LEVEL; ++level) {
			FISIR::RHIBuffer* hzb = HzbBuffer;
			cmdlist.TransitionBuffers(&hzb, 1,
				FISIR::ResourceAccess::ShaderReadWrite, FISIR::ResourceAccess::ShaderReadWrite,
				FISIR::RHIUsingStage::ComputeShaderStage, FISIR::RHIUsingStage::ComputeShaderStage);
			cmdlist.PushConstant(level, FISIR::ComputeShaderStage);   // 本轮建第几层
			const uint32_t w = (NANITE_RT_WIDTH + (1u << level) - 1u) >> level;
			const uint32_t h = (NANITE_RT_HEIGHT + (1u << level) - 1u) >> level;
			cmdlist.dispatch((w + 15u) / 16u, (h + 15u) / 16u, 1);
		}
		cmdlist.End(nullptr, { ClearDoneHzbSemaphore }, { HzbDoneSemaphore });
	}

	// --- 2. 簇选择：两条列表（软/硬各一条）+ 软光栅三角形工作列表 + 硬光栅 DrawIndirect ---
	uint32_t* swList = static_cast<uint32_t*>(EnableClusterListBufferSw->getBufferData());
	swList[0] = 0;   // 软光栅簇数
	swList[1] = 0;   // 软光栅三角形数（兼作三角形工作列表的分配计数器）
	*static_cast<int*>(EnableClusterListBufferHw->getBufferData()) = 0;
	// 清空间接绘制缓冲：尾部残留项置 0（vertexCount=0）→ 派发到未选中簇的 draw 为 no-op。
	memset(IndirectDrawBuffer->getBufferData(), 0, (uint64_t)g_ClusterCount * 16);
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);

		// 沿 BVH 下降：种子（根节点）+ ≤16 轮分层 BFS，每轮一次 dispatch，两份队列 ping-pong。
		// 种子与帧号由 CPU 直接写 host-visible 缓冲（帧首 WaitFrameGPUIdle 已保证上一帧 GPU 做完）。
		{
			uint32_t* queue0 = static_cast<uint32_t*>(BvhQueueBuffers[0]->getBufferData());
			uint32_t* queue1 = static_cast<uint32_t*>(BvhQueueBuffers[1]->getBufferData());
			const size_t queueBytes = (NANITE_QUEUE_HEADER + g_BvhQueueCapacity) * sizeof(uint32_t);
			memset(queue0, 0, queueBytes);
			memset(queue1, 0, queueBytes);
			// 种子 = 全部根节点（入度 0；不能假定是节点 0，见 ScanBvhAndCollectRoots 的注释）
			const uint32_t rootCount = (uint32_t)std::min<size_t>(
				g_BvhRootNodes.size(), (size_t)g_BvhQueueCapacity);
			queue0[0] = rootCount;
			for (uint32_t r = 0; r < rootCount; ++r)
				queue0[NANITE_QUEUE_HEADER + r] = g_BvhRootNodes[r];
			static_cast<uint32_t*>(BvhVisitedBuffer->getBufferData())[0] = ++g_BvhFrameEpoch;

			const uint32_t groups = (g_BvhQueueCapacity + 63u) / 64u;   // 线程多于实际条目时自己提前退出
			cmdlist.SetPipelineState(BvhTraversePipeline);
			for (uint32_t level = 0; level < kNaniteBvhMaxLevels; ++level) {
				// ⚠ 轮与轮之间必须插屏障：本轮要读上一轮写进队列的节点，还要写另一份队列。
				// 同一个命令列表里连续 dispatch 没有自动的内存可见性保证，少了这一步就会
				// 「某些节点这帧漏掉、下一帧又赶上」—— 表现为 Cluster 逐帧闪烁（实测踩过）。
				FISIR::RHIBuffer* queues[3] = { BvhQueueBuffers[0], BvhQueueBuffers[1], BvhVisitedBuffer };
				cmdlist.TransitionBuffers(queues, 3,
					FISIR::ResourceAccess::ShaderReadWrite, FISIR::ResourceAccess::ShaderReadWrite,
					FISIR::RHIUsingStage::ComputeShaderStage, FISIR::RHIUsingStage::ComputeShaderStage);
				cmdlist.SetResourcePack(BvhTraversePacks[level & 1u]);
				cmdlist.dispatch(groups, 1, 1);
			}
		}

		cmdlist.End(nullptr, { HzbDoneSemaphore }, { SelectDoneSoftSemaphore, SelectDoneHardSemaphore });
	}

	// --- 3. 软光栅（compute：**一个线程一个三角形**，工作列表由 Selection 展开）---
	// 派发上限 = 最大簇数 × 每簇三角形上限；线程用列表里的实际数量提前退出。
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetPipelineState(NaniteRenderPipeline);
		cmdlist.SetResourcePack(NaniteRenderResourcePack);
		cmdlist.dispatch((g_ClusterCount * NANITE_MAX_CLUSTER_TRIANGLES + 63) / 64, 1, 1);
		cmdlist.End(nullptr, { ClearDoneSoftSemaphore, SelectDoneSoftSemaphore }, { SoftDoneSemaphore });
	}

	// --- 4. 硬光栅（graphics：DrawIndirect 派发硬光栅列表，像素由 PS 写进 FrameBuffer）---
	// 离屏颜色/深度附件只作占位（管线已关闭深度测试与写入）。
	{
		FISIR::RHIRenderCommandList cmdlist(rhi);
		FISIR::ClearValue clearOffscreen{
			.ColorClear = 1,
			.colorinfo = {0.149f, 0.165f, 0.180f, 1.0f},
			.DepthStencilClear = 1,
			.depthclearval = 1.0f,
		};
		cmdlist.BeginRenderPass(OffscreenFrameBuffer, 0, clearOffscreen);
		cmdlist.SetPipelineState(NaniteGraphicsPipeline);
		cmdlist.SetResourcePack(NaniteRenderResourcePack);
		cmdlist.SetViewPort(0, 0, NANITE_RT_WIDTH, NANITE_RT_HEIGHT, 1.0f, 0.0f);
		cmdlist.SetScissor(NANITE_RT_WIDTH, NANITE_RT_HEIGHT);
		cmdlist.DrawIndirect(IndirectDrawBuffer, 0, g_ClusterCount, 16);
		cmdlist.EndRenderPass();
		cmdlist.End(nullptr, { ClearDoneHardSemaphore, SelectDoneHardSemaphore }, { RenderDoneSemaphore });
	}

	// --- 5. FrameBuffer 写入：把 VisBuffer 解析成最终帧缓冲（着色只发生在这里）---
	// 一个线程一个像素、无原子。等两个光栅器都写完 VisBuffer 才能开始。
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetPipelineState(FrameBufferWritePipeline);
		cmdlist.SetResourcePack(FrameBufferWriteResourcePack);
		cmdlist.dispatch((NANITE_RT_WIDTH + 15) / 16, (NANITE_RT_HEIGHT + 15) / 16, 1);
		cmdlist.End(nullptr, { SoftDoneSemaphore, RenderDoneSemaphore }, { FrameBufferWriteDoneSemaphore });
	}
}

void DestroyClusterResource(FISIR::DynamicRHI* rhi) {
	// 信号量在其上所有提交完成前不可销毁，而此处已退出主循环、最后一帧的呈现提交
	// 未必完成；先等一次帧完成信号量即可保证安全（有未等待的提交时才等）。
	WaitFrameGPUIdle();
	if (HzbDoneSemaphore) {
		rhi->RHIDestroySemaphore(HzbDoneSemaphore);
		HzbDoneSemaphore = nullptr;
	}
	if (ClearDoneHzbSemaphore) {
		rhi->RHIDestroySemaphore(ClearDoneHzbSemaphore);
		ClearDoneHzbSemaphore = nullptr;
	}
	if (FrameBufferWriteDoneSemaphore) {
		rhi->RHIDestroySemaphore(FrameBufferWriteDoneSemaphore);
		FrameBufferWriteDoneSemaphore = nullptr;
	}
	if (FrameDoneSemaphore) {
		rhi->RHIDestroySemaphore(FrameDoneSemaphore);
		FrameDoneSemaphore = nullptr;
	}
	if (RenderDoneSemaphore) {
		rhi->RHIDestroySemaphore(RenderDoneSemaphore);
		RenderDoneSemaphore = nullptr;
	}
	if (SoftDoneSemaphore) {
		rhi->RHIDestroySemaphore(SoftDoneSemaphore);
		SoftDoneSemaphore = nullptr;
	}
	if (SelectDoneHardSemaphore) {
		rhi->RHIDestroySemaphore(SelectDoneHardSemaphore);
		SelectDoneHardSemaphore = nullptr;
	}
	if (SelectDoneSoftSemaphore) {
		rhi->RHIDestroySemaphore(SelectDoneSoftSemaphore);
		SelectDoneSoftSemaphore = nullptr;
	}
	if (ClearDoneHardSemaphore) {
		rhi->RHIDestroySemaphore(ClearDoneHardSemaphore);
		ClearDoneHardSemaphore = nullptr;
	}
	if (ClearDoneSoftSemaphore) {
		rhi->RHIDestroySemaphore(ClearDoneSoftSemaphore);
		ClearDoneSoftSemaphore = nullptr;
	}

	if (ClusterSelectionBuffer) {
		rhi->RHIDestroyBuffer(ClusterSelectionBuffer);
		ClusterSelectionBuffer = nullptr;
	}

	if (ClusterDataBuffer) {
		rhi->RHIDestroyBuffer(ClusterDataBuffer);
		ClusterDataBuffer = nullptr;
	}

	if (EnableClusterListBufferSw) {
		rhi->RHIDestroyBuffer(EnableClusterListBufferSw);
		EnableClusterListBufferSw = nullptr;
	}
	if (EnableClusterListBufferHw) {
		rhi->RHIDestroyBuffer(EnableClusterListBufferHw);
		EnableClusterListBufferHw = nullptr;
	}

	if (FrameBuffer) {
		rhi->RHIDestroyBuffer(FrameBuffer);
		FrameBuffer = nullptr;
	}
	if (VisBuffer) {
		rhi->RHIDestroyBuffer(VisBuffer);
		VisBuffer = nullptr;
	}
	if (HzbBuffer) {
		rhi->RHIDestroyBuffer(HzbBuffer);
		HzbBuffer = nullptr;
	}
	for (uint32_t q = 0; q < 2; ++q) {
		if (BvhQueueBuffers[q]) {
			rhi->RHIDestroyBuffer(BvhQueueBuffers[q]);
			BvhQueueBuffers[q] = nullptr;
		}
	}
	if (BvhVisitedBuffer) {
		rhi->RHIDestroyBuffer(BvhVisitedBuffer);
		BvhVisitedBuffer = nullptr;
	}
	if (RenderParamsBuffer) {
		rhi->RHIDestroyBuffer(RenderParamsBuffer);
		RenderParamsBuffer = nullptr;
	}

	// ── 硬光栅资源清理 ──
	if (IndirectDrawBuffer) {
		rhi->RHIDestroyBuffer(IndirectDrawBuffer);
		IndirectDrawBuffer = nullptr;
	}
	if (OffscreenFrameBuffer) {
		rhi->RHIDestroyFrameBuffer(OffscreenFrameBuffer);
		OffscreenFrameBuffer = nullptr;
	}
	if (OffscreenColorTexture) {
		rhi->RHIDestroyTexture(OffscreenColorTexture);
		OffscreenColorTexture = nullptr;
	}
	if (OffscreenDepthTexture) {
		rhi->RHIDestroyTexture(OffscreenDepthTexture);
		OffscreenDepthTexture = nullptr;
	}
	if (OffscreenSampler) {
		rhi->RHIDestroySampler(OffscreenSampler);
		OffscreenSampler = nullptr;
	}

	rhi->RHIDestroyResourcePack(ClusterSelectionResourcePack);
	rhi->RHIDestroyResourcePack(NaniteRenderResourcePack);
	rhi->RHIDestroyResourcePack(ClearScreenResourcePack);
	rhi->RHIDestroyResourcePack(FrameBufferWriteResourcePack);
	rhi->RHIDestroyResourcePack(HzbBuildResourcePack);
	rhi->RHIDestroyResourcePack(BvhTraversePacks[0]);
	rhi->RHIDestroyResourcePack(BvhTraversePacks[1]);
}

FISIR::RHIBuffer* GetClusterSelectionBuffer() {
	return ClusterSelectionBuffer;
}

FISIR::RHIBuffer* GetClusterDataBuffer() {
	return ClusterDataBuffer;
}

uint32_t* GetEnabledClusterList() {
	return static_cast<uint32_t*>(EnableClusterListBufferSw->getBufferData());
}

void GetSelectedClusterCounts(uint32_t& outSoftwareClusters, uint32_t& outHardwareClusters,
                              uint32_t& outSoftwareTriangles) {
	uint32_t* sw = EnableClusterListBufferSw ? static_cast<uint32_t*>(EnableClusterListBufferSw->getBufferData()) : nullptr;
	uint32_t* hw = EnableClusterListBufferHw ? static_cast<uint32_t*>(EnableClusterListBufferHw->getBufferData()) : nullptr;
	outSoftwareClusters = sw ? sw[0] : 0;
	outSoftwareTriangles = sw ? sw[1] : 0;   // [1] 兼作三角形工作列表的分配计数器，末值即总数
	outHardwareClusters = hw ? hw[0] : 0;
}

void SetClusterSelectionBuffer(FISIR::DynamicRHI* rhi) {
	// 数据来源二选一：builder 现场生成（g_Built*，见 TryBuildFromObj）或内置 mitsuba 文件。
	// 两者都是「BVH 字节块 + nanitemesh 字节块」，下游完全一样。
	const uint8_t* bvhData = g_BuiltBVH.data();
	size_t sizeBVH = g_BuiltBVH.size();
	const uint8_t* meshData = g_BuiltMesh.data();
	size_t sizeMesh = g_BuiltMesh.size();
	std::vector<uint8_t> fileBVH, fileMesh;

	if (g_BuiltBVH.empty()) {
		// 回退：读 mitsuba。读失败就直接返回，保持 mitsuba 默认规模。
		FILE* fp = nullptr;
		fopen_s(&fp, "Res/mitsuba.bvh", "rb");
		if (!fp) { Error("Failed to open Res/mitsuba.bvh (working dir?)"); return; }
		fseek(fp, 0, SEEK_END); sizeBVH = ftell(fp); fseek(fp, 0, SEEK_SET);
		fileBVH.resize(sizeBVH); fread(fileBVH.data(), 1, sizeBVH, fp); fclose(fp);

		fopen_s(&fp, "Res/mitsuba.nanitemesh", "rb");
		if (!fp) { Error("Failed to open Res/mitsuba.nanitemesh (working dir?)"); return; }
		fseek(fp, 0, SEEK_END); sizeMesh = ftell(fp); fseek(fp, 0, SEEK_SET);
		fileMesh.resize(sizeMesh); fread(fileMesh.data(), 1, sizeMesh, fp); fclose(fp);

		bvhData = fileBVH.data();
		meshData = fileMesh.data();
	}

	// 直接写入 host-visible 缓冲（仿 BunnyPBR），彻底绕开 transfer→compute 异队列族的
	// 跨队可见性问题。此处为初始化阶段、主循环尚未开始，CPU 写入完成后 compute 才读取，
	// 且缓冲为 host-coherent，无需 flush。
	if (sizeBVH > ClusterSelectionBuffer->getSize() || sizeMesh > ClusterPageDataBuffer->getSize()) {
		Error("[Nanite] Over Buffer(BVH {} / mesh {} B, capacity {} / {} B)",
			sizeBVH, sizeMesh, ClusterSelectionBuffer->getSize(), ClusterPageDataBuffer->getSize());
		return;
	}
	memcpy(ClusterSelectionBuffer->getBufferData(), bvhData, sizeBVH);
	memcpy(ClusterPageDataBuffer->getBufferData(), meshData, sizeMesh);

	const uint32_t* mesh = static_cast<const uint32_t*>(ClusterPageDataBuffer->getBufferData());
	Info("[Nanite] {} clusters / {} nodes / {} slices / {} pages ({}: BVH {}B, mesh {}B)",
		g_ClusterCount, g_BVHNodes, g_Slices, (sizeMesh >= 4 ? mesh[0] : 0u),
		g_BuiltBVH.empty() ? "mitsuba" : "built", sizeBVH, sizeMesh);

	// BVH 已经就位：算出最细一层的误差给选择 shader 兜底（见 InputData::MinLODError）
	UpdateMinLODError();
}

InputData& getInputData() {
	return *static_cast<InputData*>(InputDataBuffer->getBufferData());
}

// ── half → float（BVH 槽里 LODError 用两个 half 打包在 Misc0.w）──────────────
static float HalfToFloat(uint16_t h) {
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exponent = (h >> 10) & 0x1Fu;
	uint32_t mantissa = h & 0x3FFu;
	uint32_t bits;
	if (exponent == 0) {
		if (mantissa == 0) {
			bits = sign;                       // ±0
		}
		else {                                 // 非规格化数
			exponent = 127 - 15 + 1;
			while ((mantissa & 0x400u) == 0) { mantissa <<= 1; --exponent; }
			mantissa &= 0x3FFu;
			bits = sign | (exponent << 23) | (mantissa << 13);
		}
	}
	else if (exponent == 31) {
		bits = sign | 0x7F800000u | (mantissa << 13);   // Inf / NaN
	}
	else {
		bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
	}
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

// 扫一遍 BVH，一次拿到两样东西（下降要用）：
//   1) 「可画组」里最小的 MinLODError —— 层次里最细一层的误差，给误差预算兜底；
//   2) **DAG 的根节点列表** —— 入度为 0 的节点。
// 槽布局（与 ClusterSelection.slang 的 GetHierarchyNodeSlice 一致）：
//   节点 208 字节 = 4 槽；槽 c 的 Misc0 在节点内 64 + 16c（低 16 位 = MinLODError(half)）、
//   Misc1 在 128 + 16c（.w = ChildStartReference，高 16 位 = 子节点号）、
//   Misc2 在 192 + 4c（0 = 空槽、0xFFFFFFFF = 链接槽、其余 = 可画组）。
//
// ⚠ 根不能假定是节点 0：这个 DAG 是多父的，且有多个源。平扫时无所谓（不看根），
// 一旦改成沿 BVH 下降，种错根就会一个簇都选不出来（实测 hw 0）。
static float ScanBvhAndCollectRoots(const uint8_t* bvh, uint32_t nodeCount) {
	std::vector<uint8_t> hasParent(nodeCount, 0);
	float best = FLT_MAX;

	for (uint32_t n = 0; n < nodeCount; ++n) {
		const uint8_t* node = bvh + (size_t)n * 208;
		for (uint32_t c = 0; c < 4; ++c) {
			uint32_t misc2;
			memcpy(&misc2, node + 192 + 4 * c, sizeof(misc2));

			if (misc2 == 0xFFFFFFFFu) {                       // 链接槽：记下子节点的入度
				uint32_t misc1w;
				memcpy(&misc1w, node + 128 + 16 * c + 12, sizeof(misc1w));
				const uint32_t childNode = misc1w >> 16;
				if (childNode < nodeCount) hasParent[childNode] = 1;
				continue;
			}
			if (misc2 == 0u) continue;                        // 空槽

			uint32_t misc0w;
			memcpy(&misc0w, node + 64 + 16 * c + 12, sizeof(misc0w));
			const float err = HalfToFloat((uint16_t)(misc0w & 0xFFFFu));
			if (err > 0.0f && err < best) best = err;
		}
	}

	g_BvhRootNodes.clear();
	for (uint32_t n = 0; n < nodeCount; ++n)
		if (!hasParent[n]) g_BvhRootNodes.push_back(n);
	if (g_BvhRootNodes.empty()) g_BvhRootNodes.push_back(0u);   // 兜底：整张图是个环的话至少还能跑

	return (best == FLT_MAX) ? 0.0f : best;
}

void UpdateMinLODError() {
	if (!ClusterSelectionBuffer || !InputDataBuffer || g_BVHNodes == 0) return;
	const float minError = ScanBvhAndCollectRoots(
		static_cast<const uint8_t*>(ClusterSelectionBuffer->getBufferData()), g_BVHNodes);
	getInputData().MinLODError = minError;
	Info("[Nanite] finest LOD error = {:.6f}; {} BVH roots (descent seeds, {} nodes scanned)",
	     minError, (uint32_t)g_BvhRootNodes.size(), g_BVHNodes);
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

FISIR::RHITexture* GetOffscreenColorTexture() {
	return OffscreenColorTexture;
}

FISIR::RHISampler* GetOffscreenSampler() {
	return OffscreenSampler;
}
