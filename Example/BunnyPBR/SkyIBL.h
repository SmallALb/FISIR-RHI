// SkyIBL.h —— BunnyPBR 的程序化天空 + 图像光照（IBL）CPU 烘焙（header-only）
//
// 目标：给样例加上「天空盒背景」与「基于图像的环境光照」，并解锁金属度。
//
// ═══ 设计要点（为何这样做）═══════════════════════════════════════════
//
// 1) 天空是**解析的**，且只在 C++ 里定义一次（SkyRadiance）。
//    烘焙出来的环境立方体贴图就是唯一的「环境真值」：天空盒直接采样它的第 0 级，
//    IBL 从它推导（SH 投影 / GGX 预滤波）。GPU 侧从不重算天空公式，
//    于是不会出现「天空盒与 IBL 各写一份、结果对不上」这种经典裂缝。
//
// 2) 烘焙全部在 CPU 上完成（启动时一次性，实测 ~0.2 s），不走 compute：
//    · RHI 的 VulkanTexture::getVkDescriptorType() 由纹理的 useFor 位决定 ——
//      同一张纹理若同时带 TextureUseForStorage 与 TextureUseForShaderReadOnly，
//      只能报出其中一个描述符类型，也就是「GPU 写入 + 采样」的双身份在本后端
//      表达不了（要么再建一个共享 VkImage、用途不同的纹理对象，要么放开 RHI）。
//      CPU 烘焙绕开这一层：纹理只需要 TransferDst | ShaderReadOnly 两种用途。
//    · 环境数据是静态的（天空不随时间变化），没有任何理由每帧上 GPU 重算。
//
// 3) 像素格式用 RGBA_16（VK_FORMAT_R16G16B16A16_UNORM，8 字节/像素）：
//    UNORM 格式的线性过滤是 Vulkan **规范强制**支持的；而 RGBA_32
//    （VK_FORMAT_R32G32B32A32_SFLOAT）的线性过滤属于可选特性
//    （VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT），部分集成/移动 GPU
//    并不支持 —— 那样天空盒会直接采样失败。
//    但 UNORM 的上限是 1.0，而 HDR 天空的高饱和蓝（>1）与太阳圆盘（~6）都超过它，
//    直接编码会把颜色压成灰白。因此烘焙时先按「整个天空的最大分量」归一化到 [0,1]
//    （SkyPeakScale），把峰值倍数随 IBLParams 一起传回着色器相乘还原 ——
//    线性缩放不改变颜色比例，16 位精度也远够（归一化后最暗处的相对精度仍 ~1e-4）。
//
// 4) 漫反射 IBL 用 SH9（Ramamoorthi & Hanrahan 2001）：
//    9 个 float3 跟随 FrameUB 进 cbuffer，省掉一张辐照度立方体贴图，也比
//    「低分辨率立方体 + 三线性插值」更平滑（余弦卷积后环境只剩 l ≤ 2 的频段，
//    SH 是这个频段上的精确表示）。
//
// 5) 镜面 IBL 用 split-sum 近似（Karis 2013）：
//    预滤波环境立方体（GGX 重要性采样，mip ↔ 粗糙度）+ **解析** env-BRDF 近似，
//    省掉一张 BRDF LUT 纹理和那一趟烘焙，精度对本样例完全够用。
//
// ═══ 立方体贴图约定 ═══════════════════════════════════════════════════
// 面顺序 = Vulkan/D3D 数组层顺序：0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z。
// 面内 (s,t) → 方向 的映射与 Vulkan 规范的「Cube Map Face Selection」表互逆：
//   +X: ( 1, -t, -s)   -X: (-1, -t,  s)   +Y: ( s,  1,  t)
//   -Y: ( s, -1, -t)   +Z: ( s, -t,  1)   -Z: (-s, -t, -1)
// （s,t ∈ [-1,1]，都含该面的「主半轴」分量 1，不必再除 |ma|。）

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "glm/glm.hpp"

namespace FISIR {
namespace SkyIBL {

	// ── 天空参数：线性辐射亮度，**允许 > 1**（HDR）。烘焙时统一按峰值归一化。─────
	struct SkyParams {
		glm::vec3 sunDir{ -0.4319f, 0.8638f, -0.2592f };  // 单位向量，**指向太阳**（= -LightDir）
		glm::vec3 sunColor{ 1.00f, 0.92f, 0.76f };
		glm::vec3 zenith{ 0.180f, 0.420f, 1.300f };       // 天顶（高饱和蓝，>1）
		glm::vec3 horizon{ 0.620f, 0.700f, 1.000f };      // 地平线（亮、偏暖）
		glm::vec3 ground{ 0.160f, 0.140f, 0.125f };       // 地平线以下：地面反照（半球环境光的一份）
		float     sunAngularRadiusDeg{ 2.2f };            // 太阳圆盘角半径（度）
		float     sunDiscGain{ 6.0f };                    // 圆盘的附加亮度（HDR 峰值来源）
		float     sunGlowGain{ 0.9f };                    // 圆盘外围辉光
		float     horizonSoftness{ 0.02f };               // 地平线上下过渡的半宽（避免立方体上出现硬边）
		float     gradientExponent{ 0.35f };              // 天顶→地平线梯度的 pow 指数（<1 让亮带集中在地平线）
	};

	// 解析天空：方向 → 线性辐射亮度（未归一化，可能 > 1）
	inline glm::vec3 SkyRadiance(const SkyParams& p, const glm::vec3& dir) {
		const glm::vec3 d = glm::normalize(dir);
		const float t = glm::clamp(d.y, -1.0f, 1.0f);

		// 天空梯度：地平线 → 天顶
		const float k = std::pow(glm::clamp(t, 0.0f, 1.0f), p.gradientExponent);
		glm::vec3 col = glm::mix(p.horizon, p.zenith, k);

		// 太阳：硬圆盘 + 大范围辉光。smoothstep 的边取 cos 值，
		// 注意 GLM 要求 edge0 < edge1，故写成 (cosOuter, cosInner) —— 圆盘内为 1。
		const float cosA = glm::dot(d, p.sunDir);
		const float rad  = glm::radians(p.sunAngularRadiusDeg);
		const float cosInner = std::cos(rad);
		const float cosOuter = std::cos(rad * 1.35f);
		const float disc = glm::smoothstep(cosOuter, cosInner, cosA);
		const float glow = std::pow(glm::clamp(cosA, 0.0f, 1.0f), 220.0f);
		col += p.sunColor * (disc * p.sunDiscGain + glow * p.sunGlowGain);

		// 地平线以下（含软过渡）→ 地面色
		const float skyMask = glm::smoothstep(-p.horizonSoftness, p.horizonSoftness, t);
		col = glm::mix(p.ground, col, skyMask);

		return glm::max(col, glm::vec3(0.0f));
	}

	// 归一化系数：1 / 天空辐射亮度的全局最大分量。
	// 峰值在太阳圆盘中心（disc·sunDiscGain 远大于梯度项），但参数改动后不该依赖这个假设，
	// 所以再沿地平线带扫一圈取保险 —— 只需几百次求值，代价可忽略。
	inline float SkyPeakScale(const SkyParams& p) {
		float peak = 0.0f;
		auto consider = [&](const glm::vec3& d) {
			const glm::vec3 c = SkyRadiance(p, d);
			peak = std::max(peak, std::max(c.r, std::max(c.g, c.b)));
		};
		consider(p.sunDir);
		const uint32_t N = 512;
		for (uint32_t i = 0; i < N; ++i) {
			const float a = 2.0f * 3.14159265358979f * (float)i / (float)N;
			consider(glm::vec3(std::cos(a), 0.02f, std::sin(a)));   // 地平线带
			consider(glm::vec3(std::cos(a), 0.55f, std::sin(a)));   // 中天带
		}
		return 1.0f / std::max(peak, 1e-6f);
	}

	// ── 立方体面内坐标 → 方向 ─────────────────────────────────────────────
	inline glm::vec3 CubeFaceDirection(uint32_t face, float s, float t) {
		switch (face) {
		case 0:  return glm::vec3(1.0f, -t, -s);      // +X
		case 1:  return glm::vec3(-1.0f, -t, s);      // -X
		case 2:  return glm::vec3(s, 1.0f, t);        // +Y
		case 3:  return glm::vec3(s, -1.0f, -t);      // -Y
		case 4:  return glm::vec3(s, -t, 1.0f);       // +Z
		default: return glm::vec3(-s, -t, -1.0f);     // -Z
		}
	}

	// ── RGBA16 立方体贴图（CPU 侧）────────────────────────────────────────
	// texels 布局：[mip][face][y][x][rgba] —— 每个 mip 是一段「6 面连续」的块，
	// 正好就是一次 CopyToTexture(..., arrayindex=0, arraycount=6) 需要的源布局。
	struct CubeImage {
		uint32_t size = 0;                 // 第 0 级每面边长
		uint32_t mips = 1;
		std::vector<uint16_t>  texels;     // RGBA16，4 分量交错
		std::vector<size_t>    mipOffset;  // 每个 mip 的起始 uint16 下标

		uint32_t mipSize(uint32_t mip) const { return std::max(1u, size >> mip); }

		size_t mipTexelCount(uint32_t mip) const {
			const size_t s = mipSize(mip);
			return (size_t)6 * s * s;
		}

		uint16_t* mipData(uint32_t mip) { return texels.data() + mipOffset[mip]; }
		const uint16_t* mipData(uint32_t mip) const { return texels.data() + mipOffset[mip]; }

		glm::vec3 texel(uint32_t mip, uint32_t face, uint32_t x, uint32_t y) const {
			const uint32_t s = mipSize(mip);
			const size_t i = mipOffset[mip] + (((size_t)face * s + y) * s + x) * 4;
			const float inv = 1.0f / 65535.0f;
			return glm::vec3(texels[i], texels[i + 1], texels[i + 2]) * inv;
		}
	};

	inline uint16_t EncodeUNORM16(float v) {
		return (uint16_t)std::lround(glm::clamp(v, 0.0f, 1.0f) * 65535.0f);
	}

	inline CubeImage AllocateCube(uint32_t size, uint32_t mips) {
		CubeImage c;
		c.size = size;
		c.mips = mips;
		c.mipOffset.resize(mips);
		size_t totalTexels = 0;
		for (uint32_t m = 0; m < mips; ++m) {
			c.mipOffset[m] = totalTexels;
			totalTexels += c.mipTexelCount(m);
		}
		c.texels.assign(totalTexels * 4, 0);
		return c;
	}

	// ── 天空盒源：逐面直接求值天空（镜面级，1 个 mip）────────────────────
	// scale = SkyPeakScale(p)，把 HDR 辐射亮度压进 UNORM16 的 [0,1]
	inline void BakeEnvFace(CubeImage& img, const SkyParams& p, float scale, uint32_t face) {
		const uint32_t s = img.size;
		uint16_t* base = img.mipData(0) + (size_t)face * s * s * 4;
		for (uint32_t y = 0; y < s; ++y) {
			for (uint32_t x = 0; x < s; ++x) {
				const float sc = 2.0f * ((float)x + 0.5f) / (float)s - 1.0f;
				const float tc = 2.0f * ((float)y + 0.5f) / (float)s - 1.0f;
				const glm::vec3 c = SkyRadiance(p, CubeFaceDirection(face, sc, tc)) * scale;
				uint16_t* d = base + ((size_t)y * s + x) * 4;
				d[0] = EncodeUNORM16(c.r);
				d[1] = EncodeUNORM16(c.g);
				d[2] = EncodeUNORM16(c.b);
				d[3] = 65535;
			}
		}
	}

	// van der Corput（基 2），Hammersley 序列的第二维
	inline float RadicalInverseVdC(uint32_t bits) {
		bits = (bits << 16u) | (bits >> 16u);
		bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
		bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
		bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
		bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
		return (float)bits * 2.3283064365386963e-10f;
	}

	// GGX 半程向量重要性采样：α = roughness²，与 BunnyPBR.hlsl 的 D_GGX
	//（a = roughness², d = NoH²(a²-1)+1）保持同一套 α 定义，否则预滤波的波瓣
	// 会比 BRDF 实际使用的波瓣宽/窄，粗糙度看起来「对不上」。
	inline glm::vec3 ImportanceSampleGGX(float u1, float u2, const glm::vec3& n, float alpha) {
		const float phi = 2.0f * 3.14159265358979f * u1;
		const float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
		const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
		const glm::vec3 hLocal(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);

		// 以 n 为 z 轴构造切线基
		const glm::vec3 up = (std::fabs(n.z) < 0.999f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		const glm::vec3 tx = glm::normalize(glm::cross(up, n));
		const glm::vec3 ty = glm::cross(n, tx);
		return glm::normalize(tx * hLocal.x + ty * hLocal.y + n * hLocal.z);
	}

	inline uint32_t PrefilterSampleCount(uint32_t mip) {
		switch (mip) {
		case 0:  return 1;      // 镜面：直接取该方向
		case 1:  return 128;
		case 2:  return 96;
		case 3:  return 64;
		case 4:  return 48;
		default: return 32;
		}
	}

	// 逐面逐 mip 做 GGX 预滤波（mip m ↔ 粗糙度 m/(mips-1)）。
	// scale 与天空盒用同一个值 —— 两者必须处在同一套归一化单位下，
	// 着色器才能用同一个 IBLParams.x 把它们一起还原成辐射亮度。
	inline void BakePrefilterFaceMip(CubeImage& img, const SkyParams& p, float scale,
	                                 uint32_t face, uint32_t mip, uint32_t samples) {
		const uint32_t s = std::max(1u, img.size >> mip);
		const float roughness = (img.mips > 1) ? (float)mip / (float)(img.mips - 1) : 0.0f;
		const float alpha = std::max(roughness * roughness, 1e-4f);
		const bool mirror = (mip == 0);

		uint16_t* base = img.mipData(mip) + (size_t)face * s * s * 4;

		for (uint32_t y = 0; y < s; ++y) {
			for (uint32_t x = 0; x < s; ++x) {
				const float sc = 2.0f * ((float)x + 0.5f) / (float)s - 1.0f;
				const float tc = 2.0f * ((float)y + 0.5f) / (float)s - 1.0f;
				const glm::vec3 N = glm::normalize(CubeFaceDirection(face, sc, tc));

				glm::vec3 sum(0.0f);
				if (mirror) {
					sum = SkyRadiance(p, N) * scale;
				} else {
					// Cranley–Patterson 旋转：同一个 Hammersley 点集在所有 texel 上复用会
					// 留下结构性的走样花纹，用 texel 坐标做一个确定性的相位偏移即可打散
					//（确定性 = 每次运行结果一致，便于回归比对）。
					const uint32_t h = (face * 73856093u) ^ (x * 19349663u) ^ (y * 83492791u);
					const float jitter = (float)(h & 0xFFFFu) / 65536.0f;

					float weight = 0.0f;
					for (uint32_t i = 0; i < samples; ++i) {
						float u1 = ((float)i + 0.5f) / (float)samples + jitter;
						float u2 = RadicalInverseVdC(i) + jitter * 0.6180339887f;
						u1 -= std::floor(u1);
						u2 -= std::floor(u2);

						// split-sum 的经典假设：V = N = R
						const glm::vec3 H = ImportanceSampleGGX(u1, u2, N, alpha);
						const glm::vec3 L = 2.0f * glm::dot(N, H) * H - N;
						const float NoL = glm::dot(N, L);
						if (NoL > 0.0f) {
							sum += SkyRadiance(p, L) * (NoL * scale);
							weight += NoL;
						}
					}
					sum = (weight > 0.0f) ? sum / weight : SkyRadiance(p, N) * scale;
				}

				uint16_t* d = base + ((size_t)y * s + x) * 4;
				d[0] = EncodeUNORM16(sum.r);
				d[1] = EncodeUNORM16(sum.g);
				d[2] = EncodeUNORM16(sum.b);
				d[3] = 65535;
			}
		}
	}

	// ── SH9 基函数（标准 D3D 顺序：L00, L1-1, L10, L11, L2-2, L2-1, L20, L21, L22）
	//    这里的 (x,y,z) 就是世界坐标分量：基函数在球面上正交，用哪个轴当「极轴」
	//    只是基的一组旋转，投影与求值用同一组即可自洽。
	inline void SHBasis(const glm::vec3& d, float* y) {
		y[0] = 0.282095f;
		y[1] = 0.488603f * d.y;
		y[2] = 0.488603f * d.z;
		y[3] = 0.488603f * d.x;
		y[4] = 1.092548f * d.x * d.y;
		y[5] = 1.092548f * d.y * d.z;
		y[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
		y[7] = 1.092548f * d.x * d.z;
		y[8] = 0.546274f * (d.x * d.x - d.y * d.y);
	}

	// 立方体贴图 texel 的立体角（CubeMapGen 的 areaElement 差分）
	inline float AreaElement(float x, float y) {
		return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0f));
	}

	inline float TexelSolidAngle(float u, float v, float du) {
		return AreaElement(u, v) - AreaElement(u - du, v) - AreaElement(u, v - du) + AreaElement(u - du, v - du);
	}

	// 把第 0 级环境贴图投影到 SH9 → 原始辐射亮度系数（L_lm = ∫ L(d)·Y_lm(d) dω）。
	// 用「积分已烘焙的立方体」而不是再解析积分一次天空：全程只有一处天空求值，
	// 且天然带上了 texel 的立体角权重（Σw = 4π）。
	inline void ProjectIrradianceSH(const CubeImage& env, glm::vec3 sh[9]) {
		for (int i = 0; i < 9; ++i) sh[i] = glm::vec3(0.0f);

		const uint32_t s = env.size;
		const float du = 2.0f / (float)s;
		for (uint32_t face = 0; face < 6; ++face) {
			for (uint32_t y = 0; y < s; ++y) {
				for (uint32_t x = 0; x < s; ++x) {
					const float sc = 2.0f * ((float)x + 0.5f) / (float)s - 1.0f;
					const float tc = 2.0f * ((float)y + 0.5f) / (float)s - 1.0f;
					const glm::vec3 d = glm::normalize(CubeFaceDirection(face, sc, tc));
					const float w = TexelSolidAngle(sc, tc, du);
					const glm::vec3 c = env.texel(0, face, x, y);

					float yb[9];
					SHBasis(d, yb);
					for (int i = 0; i < 9; ++i) sh[i] += c * (w * yb[i]);
				}
			}
		}
	}

	// ── 烘焙配置 ──────────────────────────────────────────────────────────
	struct BakeConfig {
		uint32_t envSize{ 256 };         // 天空盒源：256²/面 ≈ 0.35°/texel，太阳圆盘（≈4.4°）跨 ~12 texel
		uint32_t prefilterSize{ 128 };   // 镜面 IBL 基准面
		uint32_t prefilterMips{ 6 };     // 128,64,32,16,8,4 → mip m ↔ 粗糙度 m/(mips-1)
	};

	struct EnvironmentBake {
		CubeImage  env;                  // 天空盒源（1 个 mip）
		CubeImage  prefiltered;          // 预滤波环境（mips 级，mip ↔ 粗糙度）
		glm::vec3  irradianceSH[9];      // 环境辐射亮度的 SH9（与上面两张图同一套归一化单位）
		float      radianceScale{ 1.0f }; // 归一化时除掉的峰值倍数：着色器乘回它即得真实辐射亮度
	};

	inline EnvironmentBake BakeEnvironment(const SkyParams& p, const BakeConfig& cfg = {}) {
		EnvironmentBake out;
		out.env         = AllocateCube(cfg.envSize, 1);
		out.prefiltered = AllocateCube(cfg.prefilterSize, cfg.prefilterMips);

		// 先定归一化系数：两张立方体贴图与 SH 必须用同一套单位
		const float scale = SkyPeakScale(p);
		out.radianceScale = 1.0f / scale;

		// 6 个面彼此独立、写的是各自独立的内存块 → 一面一个线程。
		// 预滤波是这里唯一有分量的一步（~4M 次天空求值），单线程要 ~150 ms，
		// 6 线程后压到 ~25 ms，值得。
		auto parallelFaces = [](auto&& fn) {
			std::vector<std::thread> workers;
			workers.reserve(6);
			for (uint32_t f = 0; f < 6; ++f) workers.emplace_back([&fn, f] { fn(f); });
			for (auto& w : workers) w.join();
		};

		parallelFaces([&](uint32_t f) { BakeEnvFace(out.env, p, scale, f); });
		parallelFaces([&](uint32_t f) {
			for (uint32_t m = 0; m < out.prefiltered.mips; ++m)
				BakePrefilterFaceMip(out.prefiltered, p, scale, f, m, PrefilterSampleCount(m));
		});

		// SH 投影读的是 env 的全部 texel，必须在所有写线程 join 之后
		ProjectIrradianceSH(out.env, out.irradianceSH);
		return out;
	}

}  // namespace SkyIBL
}  // namespace FISIR
