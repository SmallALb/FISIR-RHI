#pragma once
// PLYLoader.h —— 斯坦福兔子模型加载
//
// 职责：
//   1. 解析 ASCII / binary_little_endian 两种 PLY（斯坦福兔子的标准分发格式）
//   2. 生成平滑法线（面积加权平均，因为 bunny.ply 只有 x,y,z 无法线）
//   3. 中心化 + 均匀缩放（缩放到目标尺寸，法线保持单位长度）
//   4. 计算 OBB（当前用 AABB 近似，轴对齐；PCA 精确 OBB 留作后续）
//   5. 兜底：找不到 .ply 时生成程序化球体网格，保证样例开箱即跑
//
// 顶点布局固定为 { float3 position; float3 normal; }，与 RHI 的
// RHIVertexInputInfo{ _Fvec3, _Fvec3 } 一一对应。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace FISIR {

struct MeshVertex {
    float pos[3];
    float normal[3];
};

struct MeshData {
    std::vector<MeshVertex> vertices;   // 顶点（已含法线，已中心化 + 缩放）
    std::vector<uint32_t>  indices;     // uint32 索引（后端写死 VK_INDEX_TYPE_UINT32）
    float obbHalfExtents[3];            // OBB 半边长（局部空间，轴对齐）
    float boundingRadius;               // 包围球半径（参考用）
    bool  loaded = false;               // 是否真正加载了 PLY（false = 兜底球体）
};

// ── 工具：均匀缩放到目标尺寸，并中心化到原点 ──────────────────
static void NormalizeMesh(std::vector<MeshVertex>& verts, float targetSize) {
    if (verts.empty()) return;

    float minv[3] = { 1e30f, 1e30f, 1e30f };
    float maxv[3] = { -1e30f, -1e30f, -1e30f };
    for (auto& v : verts) {
        for (int k = 0; k < 3; ++k) {
            if (v.pos[k] < minv[k]) minv[k] = v.pos[k];
            if (v.pos[k] > maxv[k]) maxv[k] = v.pos[k];
        }
    }
    float center[3];
    float maxDim = 0.0f;
    for (int k = 0; k < 3; ++k) {
        center[k] = (minv[k] + maxv[k]) * 0.5f;
        maxDim = (maxv[k] - minv[k]) > maxDim ? (maxv[k] - minv[k]) : maxDim;
    }
    if (maxDim < 1e-6f) return;

    float scale = targetSize / maxDim;   // 均匀缩放 → 法线保持单位长
    for (auto& v : verts) {
        for (int k = 0; k < 3; ++k) v.pos[k] = (v.pos[k] - center[k]) * scale;
    }
}

// ── 兜底：程序化 UV 球（找不到 bunny.ply 时用）────────────────
static void GenerateFallbackSphere(std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices, int segU = 48, int segV = 32, float radius = 0.4f) {
    verts.clear();
    indices.clear();
    // 顶点环：V 从 0..segV，U 从 0..segU（首尾重复以接缝）
    for (int v = 0; v <= segV; ++v) {
        float phi = 3.14159265359f * (float)v / (float)segV;          // 0..pi（北极到南极）
        float sinPhi = sinf(phi), cosPhi = cosf(phi);
        for (int u = 0; u <= segU; ++u) {
            float theta = 2.0f * 3.14159265359f * (float)u / (float)segU;
            float nx = sinPhi * cosf(theta);
            float ny = cosPhi;
            float nz = sinPhi * sinf(theta);
            MeshVertex mv{};
            mv.pos[0] = nx * radius; mv.pos[1] = ny * radius; mv.pos[2] = nz * radius;
            mv.normal[0] = nx; mv.normal[1] = ny; mv.normal[2] = nz;
            verts.push_back(mv);
        }
    }
    // 索引：每个四边形拆两个三角形，跳过极点的退化三角形
    int cols = segU + 1;
    for (int v = 0; v < segV; ++v) {
        for (int u = 0; u < segU; ++u) {
            uint32_t a = (uint32_t)(v * cols + u);
            uint32_t b = (uint32_t)(v * cols + u + 1);
            uint32_t c = (uint32_t)((v + 1) * cols + u);
            uint32_t d = (uint32_t)((v + 1) * cols + u + 1);
            if (v != 0)      { indices.push_back(a); indices.push_back(c); indices.push_back(b); }
            if (v != segV-1){ indices.push_back(b); indices.push_back(c); indices.push_back(d); }
        }
    }
}

// ── PLY 解析入口：成功返回 true，否则填充兜底球体并返回 false ──
// 返回的 MeshData 已中心化 + 缩放到 targetSize，法线已生成。
static MeshData LoadMesh(const char* path, float targetSize = 0.8f) {
    MeshData out{};

    std::vector<float> positions;     // 原始 xyz 平铺
    std::vector<uint32_t> faces;      // 原始三角形索引平铺（每 3 个一组）

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        f = nullptr;
    }

    if (f) {
        // ── 头部解析 ──
        char line[256];
        bool ascii = false;
        int vertCount = 0, faceCount = 0;
        if (!fgets(line, sizeof(line), f) || strncmp(line, "ply", 3) != 0) {
            fclose(f); f = nullptr;
        } else {
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "format", 6) == 0) {
                    ascii = (strstr(line, "ascii") != nullptr);
                } else if (strncmp(line, "element vertex", 14) == 0) {
                    vertCount = atoi(line + 15);
                } else if (strncmp(line, "element face", 12) == 0) {
                    faceCount = atoi(line + 13);
                } else if (strncmp(line, "end_header", 10) == 0) {
                    break;
                }
            }
        }

        if (f && vertCount > 0 && faceCount > 0) {
            positions.reserve((size_t)vertCount * 3);
            faces.reserve((size_t)faceCount * 3);

            if (ascii) {
                for (int i = 0; i < vertCount; ++i) {
                    float x, y, z;
                    if (fscanf_s(f, "%f %f %f", &x, &y, &z) != 3) break;
                    // 跳过本行剩余字段（有的 PLY 带法线/纹理）
                    if (!fgets(line, sizeof(line), f)) break;
                    positions.push_back(x); positions.push_back(y); positions.push_back(z);
                }
                for (int i = 0; i < faceCount; ++i) {
                    int n;
                    if (fscanf_s(f, "%d", &n) != 1) break;
                    if (n >= 3) {
                        int a, b, c;
                        fscanf_s(f, "%d %d %d", &a, &b, &c);
                        faces.push_back((uint32_t)a);
                        faces.push_back((uint32_t)b);
                        faces.push_back((uint32_t)c);
                        for (int k = 3; k < n; ++k) { int dummy; fscanf_s(f, "%d", &dummy); }
                    }
                    if (!fgets(line, sizeof(line), f)) break;
                }
            } else {
                // binary_little_endian：顶点 = 3 个 float，面 = uchar n + n 个 int
                for (int i = 0; i < vertCount; ++i) {
                    float xyz[3];
                    if (fread(xyz, sizeof(float), 3, f) != 3) break;
                    positions.push_back(xyz[0]); positions.push_back(xyz[1]); positions.push_back(xyz[2]);
                }
                for (int i = 0; i < faceCount; ++i) {
                    unsigned char n = 0;
                    if (fread(&n, 1, 1, f) != 1) break;
                    int idx[3] = { 0, 0, 0 };
                    if (n >= 3) {
                        fread(idx, sizeof(int), 3, f);
                        faces.push_back((uint32_t)idx[0]);
                        faces.push_back((uint32_t)idx[1]);
                        faces.push_back((uint32_t)idx[2]);
                        if (n > 3) { int skip[16]; fread(skip, sizeof(int), n - 3, f); }
                    }
                }
            }

            // ── 生成法线（面积加权平均）──
            size_t vc = positions.size() / 3;
            std::vector<MeshVertex> verts(vc);
            for (size_t i = 0; i < vc; ++i) {
                verts[i].pos[0] = positions[i * 3 + 0];
                verts[i].pos[1] = positions[i * 3 + 1];
                verts[i].pos[2] = positions[i * 3 + 2];
                verts[i].normal[0] = verts[i].normal[1] = verts[i].normal[2] = 0.0f;
            }
            size_t triCount = faces.size() / 3;
            for (size_t t = 0; t < triCount; ++t) {
                uint32_t ia = faces[t * 3 + 0], ib = faces[t * 3 + 1], ic = faces[t * 3 + 2];
                if (ia >= vc || ib >= vc || ic >= vc) continue;
                float ax = verts[ib].pos[0] - verts[ia].pos[0];
                float ay = verts[ib].pos[1] - verts[ia].pos[1];
                float az = verts[ib].pos[2] - verts[ia].pos[2];
                float bx = verts[ic].pos[0] - verts[ia].pos[0];
                float by = verts[ic].pos[1] - verts[ia].pos[1];
                float bz = verts[ic].pos[2] - verts[ia].pos[2];
                // 叉积未归一化 → 模长正比于三角形面积，天然实现面积加权
                float nx = ay * bz - az * by;
                float ny = az * bx - ax * bz;
                float nz = ax * by - ay * bx;
                for (uint32_t vidx : { ia, ib, ic }) {
                    verts[vidx].normal[0] += nx;
                    verts[vidx].normal[1] += ny;
                    verts[vidx].normal[2] += nz;
                }
            }
            for (auto& v : verts) {
                float len = sqrtf(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
                if (len > 1e-6f) {
                    v.normal[0] /= len; v.normal[1] /= len; v.normal[2] /= len;
                } else {
                    v.normal[1] = 1.0f;   // 退化顶点兜底
                }
            }

            // ── 中心化 + 均匀缩放 ──
            NormalizeMesh(verts, targetSize);

            // ── AABB（作为 OBB 近似）──
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            for (auto& v : verts) {
                for (int k = 0; k < 3; ++k) {
                    if (v.pos[k] < mn[k]) mn[k] = v.pos[k];
                    if (v.pos[k] > mx[k]) mx[k] = v.pos[k];
                }
            }
            float maxHalf = 0.0f;
            for (int k = 0; k < 3; ++k) {
                out.obbHalfExtents[k] = (mx[k] - mn[k]) * 0.5f;
                if (out.obbHalfExtents[k] > maxHalf) maxHalf = out.obbHalfExtents[k];
            }
            out.boundingRadius = maxHalf;

            out.vertices = std::move(verts);
            out.indices  = std::move(faces);
            out.loaded   = true;
        }
        fclose(f);
    }

    // ── 兜底：加载失败或文件不存在时用球体 ──
    if (!out.loaded) {
        Warn("PLY '{}' 未找到或解析失败，改用程序化球体兜底（请放入 bunny.ply 覆盖）", path);
        GenerateFallbackSphere(out.vertices, out.indices);
        NormalizeMesh(out.vertices, targetSize);
        out.obbHalfExtents[0] = out.obbHalfExtents[1] = out.obbHalfExtents[2] = targetSize * 0.5f;
        out.boundingRadius = targetSize * 0.5f;
    }

    return out;
}

} // namespace FISIR
