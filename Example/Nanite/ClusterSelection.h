#include "DynamicRHI.h"
#include "RHIShader.h"
#include "RHIBuffer.h"
#include "RHIPipeline.h"
#include "RHICommandList.h"
#include "RHIFence.h"
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
FISIR::RHIBuffer* GetFrameBuffer();

// 渲染参数（VPMatrix 等）的主机可见映射，Main.cpp 每帧写相机矩阵。
RenderParams& getRenderParams();

// 读回 compute 选中的簇列表（主机可见映射）。
// 约定：p[0] = 选中数量 N，p[1 .. N] = clusterID（(pageIndex << 8) | clusterOffset）。
// 调用方需先 ExecuteClusterSelectionPass()（内部 fence->wait()）保证结果可见。
uint32_t* GetEnabledClusterList();

uint32_t GetEnabledClusterCount();


FISIR::RHIResourcePackResult& GetClusterSelectionResourcePack();