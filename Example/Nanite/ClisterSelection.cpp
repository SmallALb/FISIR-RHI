#include "ClusterSelection.h"
#include "ShaderComplier.h"
#include "RHIFence.h"
#include "RHIFrameBuffer.h"
#include "RHISampler.h"
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

// ── 硬光栅资源（传统光栅管线渲染到离屏纹理，替代软光栅 compute mainRender）──
static FISIR::RHIShader* NaniteRenderVSShader = nullptr;   // NaniteRender.hlsl 的 mainVS
static FISIR::RHIShader* NaniteRenderPSShader = nullptr;   // NaniteRender.hlsl 的 mainPS
static FISIR::RHIBuffer* IndirectDrawBuffer = nullptr;      // u6：每条选中簇 16B DrawIndirect 参数
static FISIR::RHIPipeline* NaniteGraphicsPipeline = nullptr;
static FISIR::RHITexture* OffscreenColorTexture = nullptr;  // 离屏颜色附件（呈现 PS 采样）
static FISIR::RHITexture* OffscreenDepthTexture = nullptr;  // 离屏深度附件
static FISIR::RHIRenderPass* OffscreenRenderPass = nullptr;
static FISIR::RHIFrameBuffer* OffscreenFrameBuffer = nullptr;
static FISIR::RHISampler* OffscreenSampler = nullptr;

// ── 帧同步信号量 ──────────────────────────────────────────────
// 帧内的 clear/select/render/present 全部由信号量在 GPU 侧串联，不再使用围栏；
// CPU 只在帧首（WaitFrameGPUIdle）等待 FrameDone 一次。
static FISIR::RHISemaphore* ClearDoneSemaphore  = nullptr;   // 清屏 → 渲染（binary）
static FISIR::RHISemaphore* SelectDoneSemaphore = nullptr;   // 簇选择 → 渲染（binary）
static FISIR::RHISemaphore* RenderDoneSemaphore = nullptr;   // 渲染(compute) → 呈现(graphics)（binary）
static FISIR::RHISemaphore* FrameDoneSemaphore  = nullptr;   // 呈现(graphics) → 下帧帧首 CPU 等待（timeline）

static FISIR::RHIFence* DebugFence = nullptr; // 用于调试：GPU 侧等待帧完成，CPU 侧检查帧内各阶段是否完成。


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

	// 硬光栅 VS/PS：与软光栅 compute 同源（NaniteRender.hlsl 新增入口 mainVS/mainPS），
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
		.size = 4 * 1024 * 1024,
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
		.CountOfClusters = NANITE_MAX_CLUSTERS,   // mitsuba.nanitemesh 簇总数
		.TotalBVHNodes = 21,      // mitsuba.bvh = 4368 字节 / 208 = 21 节点
		.TotalSlices = 84,        // 21 节点 × 4 子槽
		.MaxClusters = NANITE_MAX_CLUSTERS,
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
	// 直接读回选中结果（WaitFrameGPUIdle() 之后读，相干内存保证可见性，比实际帧滞后一帧）。
	FISIR::BufferInfo enableClusterListBufferInfo{
		.data_CPU = nullptr,
		.size = 4 * 1024,
		.bufferlayout = FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		.concurrentSharing = true,   // compute 写、graphics VS 读，异队列族需并发共享
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

	//7. 间接绘制缓冲：簇选择 compute 写入 DrawIndirect 参数（16B/簇），graphics 用 DrawIndirect 读取。
	// compute 写、graphics 读，异队列族并发共享；host-visible 以便 CPU 每帧清零尾部残留项。
	FISIR::BufferInfo indirectDrawBufferInfo{
		.data_CPU = nullptr,
		.size = (uint64_t)NANITE_MAX_CLUSTERS * 16,
		.bufferlayout = FISIR::IndirectBuffer | FISIR::RWBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
		.concurrentSharing = true,
	};
	IndirectDrawBuffer = rhi->RHICreateBuffer(indirectDrawBufferInfo);
	memset(IndirectDrawBuffer->getBufferData(), 0, (uint64_t)NANITE_MAX_CLUSTERS * 16);

	//7. Nanite 渲染用：FrameBuffer（8B/px）、FrameLock（4B/px）、RenderParams（96B）
	const uint32_t pixelCount = NANITE_RT_WIDTH * NANITE_RT_HEIGHT;
	// FrameBuffer 是唯一跨队列族的缓冲：compute（渲染）写、graphics（呈现）读。
	// 本机 graphics/compute 分属不同队列族（Graphics=0, Compute=2），若保持独占共享模式
	// 则跨族访问必须做所有权转移，否则触发校验错误甚至挂起。改为并发共享即可免转移；
	// 帧间由「帧首 CPU 等待 FrameDone」串行化，帧内由 RenderDone 信号量建立跨队列可见性。
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

	ClusterSelectionResourcePack = rhi->RHICreateResourcePack({ ClusterSelectionBuffer, ClusterPageDataBuffer, ClusterDataBuffer, EnableClusterListBuffer, InputDataBuffer, DebugClusterListBuffer, IndirectDrawBuffer });
	

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
		{6, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::ComputeShaderStage},      // u6 IndirectDrawBuffer

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
	// 复用软光栅 NaniteRenderResourcePack：DXC 编译 VS 时不会剔除未用全局资源，
	// VS 的 SPIR-V 仍声明 u0..u5/b6 全部 7 个 binding，故 describeInfo 必须逐一列出
	// （与软光栅 renderDescribeInfo 一致），仅把阶段从 Compute 改为 Vertex。
	FISIR::RHIPipelineDescribeInfo graphicsDescribe {
		{0, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{1, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{2, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{3, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{4, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{5, 1, FISIR::RHIDescriptorTyp::RWBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{6, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage},
	};
	FISIR::RHIPipelineState graphicsState{
		.describeInfo = graphicsDescribe,
		.topologyType = FISIR::TopologyType::Triangle,
		.rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
		.depthStencilState = { true, true, false, 0.0f, 1.0f, FISIR::_Less_ },
		.colorblendState = { .UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_) },
		.renderpass = OffscreenFrameBuffer->getFrameRenderPass(),
	};
	graphicsState.Shaders[FISIR::__VERTEXSHADER__] = NaniteRenderVSShader;
	graphicsState.Shaders[FISIR::__FRAGMENTSHADER__] = NaniteRenderPSShader;
	NaniteGraphicsPipeline = rhi->RHICreatePipeline(graphicsState);
	if (!NaniteGraphicsPipeline) Error("[Nanite] NaniteGraphicsPipeline creation FAILED!");

	// ── 帧同步信号量 ──
	// clear → render 与 select → render 各一枚 binary 信号量（渲染 pass 需同时等待两者：
	// FrameBuffer 来自清屏、EnableClusterList 来自簇选择）。
	ClearDoneSemaphore  = rhi->RHICreateSemaphore("NaniteClearDoneSemaphore");
	SelectDoneSemaphore = rhi->RHICreateSemaphore("NaniteSelectDoneSemaphore");
	RenderDoneSemaphore = rhi->RHICreateSemaphore("NaniteRenderDoneSemaphore");
	// 等待阶段决定该信号量的 wait 挂在管线的哪一级：
	// 前两者由渲染 compute 等待（ComputeShaderStage），后者由呈现 pass 的片元着色器等待。
	ClearDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::ComputeShaderStage);
	// 硬光栅：SelectDone 由 graphics 渲染 pass 等待，等待点须早于 DrawIndirect（读间接缓冲）
	// 与 VS（读 EnableClusterList/mesh）。TOP_OF_PIPE 是最早阶段，覆盖两者。
	SelectDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::PipelinTopStage);
	RenderDoneSemaphore->setWaitingStage(FISIR::RHIUsingStage::FragmentShaderStage);

	// 帧完成信号量必须是时间线类型：binary 信号量无法在 CPU 侧 wait()。
	// 值从 1 起（见 VulkanSemaphore 的 nextSignalValue/nextWaitValue 初值），
	// 每帧末端 signal 一次、下帧帧首 wait 一次，严格配对。
	FrameDoneSemaphore = rhi->RHICreateSemaphore("NaniteFrameDoneSemaphore", FISIR::FenceType::TimeLine);

	if (!ClearDoneSemaphore || !SelectDoneSemaphore || !RenderDoneSemaphore || !FrameDoneSemaphore)
		Error("[Nanite] Frame sync semaphore creation FAILED!");

	DebugFence = rhi->RHICreateFence(false, "NaniteDebugFence");
}

void EndFramePresentPass(FISIR::RHIRenderCommandList& cmdList, const FISIR::SwapChainGetImageInfo& info) {
	// waits  ：acquire 信号量（等交换链图像可用）+ RenderDone（等渲染 compute 写完 FrameBuffer）
	// signals：renderFinish（供 present() 等待）+ FrameDone（供下帧帧首 CPU 等待）
	// fence  ：info.finishFence 不用于等待 GPU，而是「提交握手」：调用方在 present() 前等它被
	//          RHI 线程提交（见 Main.cpp），保证 vkQueuePresentKHR 等待的 renderFinish 已经有
	//          对应的 signal 提交在队列里，否则校验层会报 "... has no way to be signaled"。
	cmdList.End(info.finishFence,
		{ info.avaliable, RenderDoneSemaphore },
		{ info.renderFinish, FrameDoneSemaphore });
}

void WaitFrameGPUIdle() {
	static bool start = false;
	if (start) FrameDoneSemaphore->wait();
	start = true;
}

void ExecuteClusterSelectionPass(FISIR::DynamicRHI* rhi) {
	// 硬光栅版：仅保留「簇选择」compute，渲染改为传统光栅（DrawIndirect 渲染到离屏纹理）。
	// 软光栅的清屏 compute + mainRender compute 的创建代码已保留，但不再录制。
	// 各 pass 独立命令列表（page 只能提交一次），依赖全部由信号量在 GPU 侧建立。
	// 前置条件：调用方已执行 WaitFrameGPUIdle()，上一帧 GPU 工作已完成。

	// --- 1. 簇选择（GPU 驱动剔除：写 EnableClusterList + IndirectDrawBuffer）---
	// 渲染通道 loadOp=Clear 负责清屏，替代原软光栅的清屏 compute。
	*static_cast<int*>(EnableClusterListBuffer->getBufferData()) = 0;
	// 清空间接绘制缓冲：尾部残留项置 0（vertexCount=0）→ 派发到未选中簇的 draw 为 no-op。
	memset(IndirectDrawBuffer->getBufferData(), 0, (uint64_t)NANITE_MAX_CLUSTERS * 16);
	{
		FISIR::RHIComputeCommandList cmdlist(rhi);
		cmdlist.SetPipelineState(ClusterSelectionPipeline);
		cmdlist.SetResourcePack(ClusterSelectionResourcePack);
		cmdlist.dispatch(2, 1, 1);
		cmdlist.End(nullptr, {}, { SelectDoneSemaphore });
	}

	// --- 2. 传统光栅渲染（渲染到离屏纹理，DrawIndirect 派发每个选中簇）---
	{
		FISIR::RHIRenderCommandList cmdlist(rhi);
		FISIR::ClearValue clearOffscreen{
			.ColorClear = 1,
			.colorinfo = {0.149f, 0.165f, 0.180f, 1.0f},   // 与软光栅 ClearColor 0xFF2E2A26(ABGR) 同色背景
			.DepthStencilClear = 1,
			.depthclearval = 1.0f,
		};
		cmdlist.BeginRenderPass(OffscreenFrameBuffer, 0, clearOffscreen);
		cmdlist.SetPipelineState(NaniteGraphicsPipeline);
		cmdlist.SetResourcePack(NaniteRenderResourcePack);
		cmdlist.SetViewPort(0, 0, NANITE_RT_WIDTH, NANITE_RT_HEIGHT, 1.0f, 0.0f);
		cmdlist.SetScissor(NANITE_RT_WIDTH, NANITE_RT_HEIGHT);
		cmdlist.DrawIndirect(IndirectDrawBuffer, 0, NANITE_MAX_CLUSTERS, 16);
		cmdlist.EndRenderPass();
		cmdlist.End(nullptr, { SelectDoneSemaphore }, { RenderDoneSemaphore });
	}
}

void DestroyClusterResource(FISIR::DynamicRHI* rhi) {
	// 信号量在其上所有提交完成前不可销毁，而此处已退出主循环、最后一帧的呈现提交
	// 未必完成；先等一次帧完成信号量即可保证安全（有未等待的提交时才等）。
	WaitFrameGPUIdle();
	if (FrameDoneSemaphore) {
		rhi->RHIDestroySemaphore(FrameDoneSemaphore);
		FrameDoneSemaphore = nullptr;
	}
	if (RenderDoneSemaphore) {
		rhi->RHIDestroySemaphore(RenderDoneSemaphore);
		RenderDoneSemaphore = nullptr;
	}
	if (SelectDoneSemaphore) {
		rhi->RHIDestroySemaphore(SelectDoneSemaphore);
		SelectDoneSemaphore = nullptr;
	}
	if (ClearDoneSemaphore) {
		rhi->RHIDestroySemaphore(ClearDoneSemaphore);
		ClearDoneSemaphore = nullptr;
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

FISIR::RHITexture* GetOffscreenColorTexture() {
	return OffscreenColorTexture;
}

FISIR::RHISampler* GetOffscreenSampler() {
	return OffscreenSampler;
}
