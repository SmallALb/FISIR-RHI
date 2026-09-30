#pragma once
// OBJLoader.h —— 解析 Wavefront OBJ（ASCII），仅提取位置(v)与面(f)。
//
// 职责：
//   1. 解析 `v x y z` 顶点位置；
//   2. 解析 `f ...` 面（支持任意多边形，按扇形三角化；支持 v、v/vt、v//vn、v/vt/vn；
//      支持负索引，-1 表示最近一个顶点）；
//   3. 返回平铺的 positions（float3）与三角形索引（uint32，3/面，0 基）。
//
// Nanite 只用位置，故忽略 vt/vn；但解析时会正确跳过这些字段。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Log/Logger.h"

namespace FISIR {

struct ObjMesh {
    std::vector<float>    positions;   // xyz 平铺
    std::vector<uint32_t> indices;    // 三角形索引（3/面），0 基
    bool loaded = false;
};

// 中心化 + 均匀缩放到目标尺寸（与 BunnyPBR/PLYLoader 的 NormalizeMesh 一致），
// 使不同来源的 OBJ 归一化到相同量级，相机参数无需随模型尺寸调整。
static void NormalizePositions(std::vector<float>& positions, float targetSize) {
    if (positions.empty()) return;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < positions.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            float v = positions[i + k];
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    }
    float center[3], maxDim = 0.0f;
    for (int k = 0; k < 3; ++k) {
        center[k] = (mn[k] + mx[k]) * 0.5f;
        maxDim = (mx[k] - mn[k]) > maxDim ? (mx[k] - mn[k]) : maxDim;
    }
    if (maxDim < 1e-6f) return;
    float scale = targetSize / maxDim;
    for (size_t i = 0; i < positions.size(); i += 3) {
        for (int k = 0; k < 3; ++k) positions[i + k] = (positions[i + k] - center[k]) * scale;
    }
}

static ObjMesh LoadObj(const char* path) {
    ObjMesh out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Warn("OBJ '{}' failed to open (working dir?)", path);
        return out;
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        // ⚠ 超长行保护：fgets 在缓冲装满时**截断**，余下部分会在下一次循环里被当成**新的一行**。
        // 那样一行面会被拆成两行解析 → 产生一堆越界索引的垃圾面，表现为「模型缺面、且面数
        // 对不上」。这里把截断后的残段整段丢掉，宁可少一个超长面，也不让后续行错位。
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        }

        // 跳过空行与注释
        char c = line[0];
        if (c == '#' || c == '\n' || c == '\r' || c == '\0') continue;

        if (c == 'v' && (line[1] == ' ' || line[1] == '\t')) {
            float x = 0, y = 0, z = 0;
            if (sscanf_s(line + 2, "%f %f %f", &x, &y, &z) == 3) {
                out.positions.push_back(x);
                out.positions.push_back(y);
                out.positions.push_back(z);
            }
        } else if (c == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            // 解析面顶点（取每项的顶点索引字段）
            int idx[256];
            int n = 0;
            char* tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 256) {
                idx[n++] = atoi(tok);
                tok = strtok(nullptr, " \t\r\n");
            }
            if (n >= 3) {
                int vcount = (int)(out.positions.size() / 3);
                auto resolve = [&](int i) -> int { return i > 0 ? (i - 1) : (vcount + i); };
                // 扇形三角化：0,1,2 / 0,2,3 / 0,3,4 ...
                for (int k = 1; k < n - 1; ++k) {
                    out.indices.push_back((uint32_t)resolve(idx[0]));
                    out.indices.push_back((uint32_t)resolve(idx[k]));
                    out.indices.push_back((uint32_t)resolve(idx[k + 1]));
                }
            }
        }
    }
    fclose(f);

    out.loaded = !out.positions.empty() && !out.indices.empty();
    if (!out.loaded) {
        Warn("OBJ '{}' is empty (no vertices or faces)", path);
    } else {
        // 归一化到目标尺寸 2.0（中心化 + 均匀缩放），使相机参数与模型量级解耦
        NormalizePositions(out.positions, 2.0f);
        Info("OBJ '{}' loaded successfully: {} vertices / {} triangles (normalized to 2.0)", path,
            out.positions.size() / 3, out.indices.size() / 3);
    }
    return out;
}

} // namespace FISIR
