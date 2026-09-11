#include "DynamicRHI.h"
#include "RHIShader.h"
#include "RHIBuffer.h"
#include "RHIPipeline.h"
#include "RHICommandList.h"
#include "RHISemaphore.h"
#include <glm/glm.hpp>
struct InputData {
	glm::vec4	Position;   // xyz + pad（std140 下 float3 会被填充到 16 字节，用 vec4 显式对齐）
	glm::vec4	Direction;  // xyz + pad
	float		LodScale;
	float		ZNear;
	uint32_t	CountOfClusters;
	uint32_t 	TotalBVHNodes;
	uint32_t 	TotalSlices;
	uint32_t 	MaxClusters;
};


struct ClusterData {
	uint32_t indexCount;
	uint32_t baseVertex;
	uint32_t baseIndex;
	uint32_t padding;
};

// 离屏渲染目标尺寸（与 Main.cpp 视口一致，供 FrameBuffer / 呈现共用）
constexpr uint32_t NANITE_RT_WIDTH = 512;
constexpr uint32_t NANITE_RT_HEIGHT = 384;

// 场景最大簇数（mitsuba.nanitemesh）。渲染 pass 按此上界派发，
// 使 GPU 写出的「本帧选中数量」无需读回 CPU——那会强制一次额外的 CPU 等待。
constexpr uint32_t NANITE_MAX_CLUSTERS = 932;

// NaniteRender.hlsl 的 RenderParams (b2)，std140 布局：float4x4(64) + float2(8) + 4×float(16) = 96B
struct RenderParams {
	glm::mat4 VPMatrix;    // world -> clip（行向量约定，见 Main.cpp transpose）
	glm::vec2 screenSize;  // {NANITE_RT_WIDTH, NANITE_RT_HEIGHT}
	float ClearDepth;      // 1.0f（最远深度）
	uint32_t ClearColor;      // 0.0f
	float NearPlane;
	float FarPlane;
	float _pad[2];         // 对齐到 96 字节
};

void InitClusterSelection(FISIR::DynamicRHI* rhi);

void ExecuteClusterSelectionPass(FISIR::DynamicRHI* rhi);

void DestroyClusterResource(FISIR::DynamicRHI* rhi);


FISIR::RHIBuffer* GetClusterSelectionBuffer();

FISIR::RHIBuffer* GetClusterDataBuffer();

void SetClusterSelectionBuffer(FISIR::DynamicRHI* rhi);

InputData& getInputData();

// Nanite 渲染结果缓冲（每像素 8 字节：低 32 位深度 | 高 32 位颜色），供呈现管线绑定。
// 硬光栅化后此缓冲不再被写入（软光栅创建代码保留但停用），保留此接口仅为兼容。
FISIR::RHIBuffer* GetFrameBuffer();

// 硬光栅渲染目标（离屏颜色纹理）与采样器，供呈现管线 enableTextureInput 绑定。
FISIR::RHITexture* GetOffscreenColorTexture();

FISIR::RHISampler* GetOffscreenSampler();

// 渲染参数（VPMatrix 等）的主机可见映射，Main.cpp 每帧写相机矩阵。
RenderParams& getRenderParams();

// 读回 compute 选中的簇列表（主机可见映射）。
// 约定：p[0] = 选中数量 N，p[1 .. N] = clusterID（(pageIndex << 8) | clusterOffset）。
// 注意：帧内不再有 CPU 等待，调用方须在 WaitFrameGPUIdle() 之后（即上一帧 GPU 已完成、
// 本帧簇选择尚未重置它之前）读取，才能拿到稳定值——比实际帧滞后一帧。
uint32_t* GetEnabledClusterList();

uint32_t GetEnabledClusterCount();

// ── 帧同步（帧内数据依赖全部由信号量在 GPU 侧建立）────────────────
// 数据链：clear --ClearDone--> render --RenderDone--> present，
//         select --SelectDone--> render，present --FrameDone--> 下帧帧首 CPU 等待。
// 每帧的 CPU 等待点只有两个：帧首 WaitFrameGPUIdle()（等 GPU 完成），以及 present 前的
// finishFence->waitFenceSubmited()（只等 RHI 线程把呈现页提交出去，不等 GPU）。
// 跨页信号量依赖成立的前提是「提交顺序 == 录制顺序」，这由 RHI 按页面录制序号有序提交保证
// （见 VulkanRHI::VulkanRHILoop），因此 clear/select/render/present 必须按此顺序录制。

// 录制本帧最后一个提交（呈现 pass）：等待 acquire 信号量与渲染完成信号量，
// 信号 present 信号量与帧完成信号量。调用方随后须 finishFence->waitFenceSubmited()
// 再 present()。调用后即登记「本帧已提交」。
void EndFramePresentPass(FISIR::RHIRenderCommandList& cmdList, const FISIR::SwapChainGetImageInfo& info);

// 帧首 CPU 等待：等上一帧 GPU 工作全部完成（含呈现 pass 对 FrameBuffer 的读取），
// 从而可以安全地清空 FrameBuffer、重置 EnableClusterList、改写 RenderParams/InputData。
// 首帧（尚无任何提交）直接返回。
void WaitFrameGPUIdle();


FISIR::RHIResourcePackResult& GetClusterSelectionResourcePack();