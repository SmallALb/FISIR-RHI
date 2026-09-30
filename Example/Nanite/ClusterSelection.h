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
	// 软/硬光栅分配阈值：见 ClusterSelection.slang 的 ShouldUseSoftwareRaster。
	// 0 = 全给硬光栅；很大 = 全给软光栅。
	float		SwRasterThreshold;
	// 整个层次里最细一层的 LOD 误差（由 SetClusterSelectionBuffer 扫 BVH 算出）。
	// 选择 shader 用它给误差预算 T 兜底：相机贴近/进入包围球时 projScale 会塌到 0，
	// 没有这个下界的话近处几何会被整片丢掉。恰好落在原结构体的尾部填充里，不改变大小。
	float		MinLODError;
};



struct ClusterData {
	uint32_t indexCount;
	uint32_t baseVertex;
	uint32_t baseIndex;
	uint32_t padding;
};

// 离屏渲染目标尺寸（与 Main.cpp 视口一致，供 FrameBuffer / 呈现共用）
constexpr uint32_t NANITE_RT_WIDTH = 1920;
constexpr uint32_t NANITE_RT_HEIGHT = 1080;

// 场景最大簇数（mitsuba.nanitemesh）。渲染 pass 按此上界派发，
// 使 GPU 写出的「本帧选中数量」无需读回 CPU——那会强制一次额外的 CPU 等待。
constexpr uint32_t NANITE_MAX_CLUSTERS = 932;

// 单个簇的三角形数上限，决定软光栅「三角形工作列表」的容量与派发规模：
//   · NaniteBuilder 的 maxClusterTriangles = 128（mitsuba 数据集实测 126）；
//   · 工作列表条目把「簇内三角形序号」打包在低 8 位，所以上限必须 < 256。
// 列表容量 = g_ClusterCount × 本值（bunny 268×128 = 34304 项 ≈ 137KB）。
constexpr uint32_t NANITE_MAX_CLUSTER_TRIANGLES = 128;

// HZB（深度金字塔）最高层级：L0 = 全分辨率（上一帧深度），L1..L7 逐级 2×2 取 min。
// 层级尺寸按**向上取整**逐级减半；以下三处必须完全一致：
//   ClusterSelection.h（缓冲尺寸 / 派发量）、HZBBuild.slang、ClusterSelection.slang 的 HzbDim。
constexpr uint32_t NANITE_HZB_MAX_LEVEL = 7;

// HZB 金字塔的 texel 总数 = L0..maxLevel 各层 ceil(w/2^L)×ceil(h/2^L) 之和
inline uint64_t GetHzbTexelCount(uint32_t width, uint32_t height, uint32_t maxLevel = NANITE_HZB_MAX_LEVEL) {
    uint64_t total = 0;
    for (uint32_t level = 0; level <= maxLevel; ++level) {
        const uint64_t w = (width + (1u << level) - 1u) >> level;
        const uint64_t h = (height + (1u << level) - 1u) >> level;
        total += w * h;
    }
    return total;
}


// 数据来源：留空 = 用内置的 Res/mitsuba.bvh + Res/mitsuba.nanitemesh（默认，渲染已验证）。
// 填路径则尝试导入该 OBJ，用 NaniteBuilder.h（meshoptimizer + clusterlod）现场生成簇与 BVH；
// 路径为空、文件打不开、或构建失败，都自动回退 mitsuba。**换模型只改这一行。**

// 导入模型的缩放（仅 NANITE_OBJ_PATH 非空时生效）。LoadObj 会把模型归一化到**最大边长 2.0**，
// 所以世界尺寸 = 2.0 × 本值；实测 bunny_hi 归一化后包围球半径 ≈ 1.346，即 1.346 × 本值。
// 参考量级：mitsuba 场景 AABB 中心约 (0,-107,3)、包围球半径约 248；相机在 (0,-60,400)、FOV 60°，
// 原点平面可见高度约 462。故 bunny 取 ~184 与 mitsuba 视觉等大，160 稍小、画面更稳。
// 注：projScale 与 LODError 同为长度量，缩放不改变选中的 LOD 集合，只改视觉大小。

// NaniteRender.slang / FrameBufferWrite.slang / ClearScreen.slang 的 RenderParams，
// std140 布局：float4x4(64) + float2(8) + 4×float(16) + 2×float(8) = 96B
struct RenderParams {
	glm::mat4 VPMatrix;    // world -> clip（行向量约定，见 Main.cpp transpose）
	glm::vec2 screenSize;  // {NANITE_RT_WIDTH, NANITE_RT_HEIGHT}
	float ClearDepth;      // 1.0f（最远深度）
	uint32_t ClearColor;      // 0.0f
	// 着色模式（FrameBufferWrite.slang 的解析管线用）：0 = Lambert 面着色；1 = 色块
	// （每簇一个固定颜色，用来肉眼查重叠/漏选）。运行时开关，面板上有勾选框，不再用宏编译。
	// 这个位置原本是 NearPlane —— 三个着色器都只声明、从未读取，正好拿来用。**不要挪到结构体
	// 尾部（88 字节处）**：尾部那 8 字节落在描述符声明的范围之外，着色器读到的恒为 0（实测踩过）。
	uint32_t ColorBlock;
	float FarPlane;
	float _pad[2];         // 对齐到 96 字节
};

void SetModelPath(const char* path, float scale);

void InitClusterSelection(FISIR::DynamicRHI* rhi);

void ExecuteClusterSelectionPass(FISIR::DynamicRHI* rhi);

void DestroyClusterResource(FISIR::DynamicRHI* rhi);


FISIR::RHIBuffer* GetClusterSelectionBuffer();

FISIR::RHIBuffer* GetClusterDataBuffer();

void SetClusterSelectionBuffer(FISIR::DynamicRHI* rhi);

InputData& getInputData();

// 扫 BVH 求出「最细一层的 LOD 误差」，写进 InputData.MinLODError。
// 由 SetClusterSelectionBuffer 在数据上传后调用（此时 BVH 已经在缓冲里）。
void UpdateMinLODError();

// Nanite 渲染结果缓冲（每像素 8 字节：低 32 位深度 | 高 32 位颜色），供呈现管线绑定。
// 硬光栅化后此缓冲不再被写入（软光栅创建代码保留但停用），保留此接口仅为兼容。
FISIR::RHIBuffer* GetFrameBuffer();

// 硬光栅渲染目标（离屏颜色纹理）与采样器，供呈现管线 enableTextureInput 绑定。
FISIR::RHITexture* GetOffscreenColorTexture();

FISIR::RHISampler* GetOffscreenSampler();

// 渲染参数（VPMatrix 等）的主机可见映射，Main.cpp 每帧写相机矩阵。
RenderParams& getRenderParams();

// 读回 compute 选中的列表（主机可见映射）。
// 软光栅列表布局：[0] = 软光栅簇数，[1] = 软光栅三角形数（也是三角形工作列表的分配计数器），
// [2 .. 2+N) = clusterID；硬光栅列表：[0] = 簇数，[1 ..] = clusterID。
// 注意：帧内不再有 CPU 等待，调用方须在 WaitFrameGPUIdle() 之后（即上一帧 GPU 已完成、
// 本帧簇选择尚未重置它之前）读取，才能拿到稳定值——比实际帧滞后一帧。
uint32_t* GetEnabledClusterList();

// 读回本帧两条列表的数量（主机可见映射；须在 WaitFrameGPUIdle() 之后读，比实际帧滞后一帧）。
void GetSelectedClusterCounts(uint32_t& outSoftwareClusters, uint32_t& outHardwareClusters,
                              uint32_t& outSoftwareTriangles);

// ── 帧同步（帧内数据依赖全部由信号量在 GPU 侧建立）────────────────
// 数据链：clear --ClearDone--> render --RenderDone--> present，
//         select --SelectDone--> render，present --FrameDone--> 下帧帧首 CPU 等待。
// 每帧的 CPU 等待点只有两个：帧首 WaitFrameGPUIdle()（等 GPU 完成），以及提交后的
// finishFence->waitFenceSubmited()（只作节流，不等 GPU —— 呈现已指令化，顺序由 RHI
// 线程保证）。跨页信号量依赖成立的前提是「提交顺序 == 录制顺序」，这由 RHI 按页面
// 录制序号有序提交保证（见 VulkanRHI::VulkanRHILoop），因此 clear/select/render/present
// 必须按此顺序录制。

// 录制本帧最后一个提交（呈现 pass）：等待 acquire 信号量与渲染完成信号量，
// 信号 present 信号量与帧完成信号量。调用方须先把呈现录成指令 ——
// cmdList.Present(swapchain, frameID) **写在本次 End 之前**（见 Main.cpp）；
// 取图（acquire）与呈现同在 RHI 线程，VkSwapchainKHR 不会被跨线程触碰。
void EndFramePresentPass(FISIR::RHIRenderCommandList& cmdList, const FISIR::SwapChainGetImageInfo& info);

// 帧首 CPU 等待：等上一帧 GPU 工作全部完成（含呈现 pass 对 FrameBuffer 的读取），
// 从而可以安全地清空 FrameBuffer、重置 EnableClusterList、改写 RenderParams/InputData。
// 首帧（尚无任何提交）直接返回。
void WaitFrameGPUIdle();


FISIR::RHIResourcePackResult& GetClusterSelectionResourcePack();