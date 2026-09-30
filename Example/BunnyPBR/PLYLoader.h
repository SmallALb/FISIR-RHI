#pragma once
// PLYLoader.h —— 斯坦福兔子模型加载（ASCII / binary_little_endian）
//
// 职责：
//   1. 解析 PLY 头部属性表（element / property / property list），据此定位 vertex 的
//      x/y/z 与 face 的顶点索引列表。顶点上的多余字段（confidence / intensity /
//      nx,ny,nz / s,t …）按声明的步长自动跳过——这正是高采样原始扫描件
//      bun_zipper.ply（35947 顶点，带 confidence+intensity）能被正确读入的前提：
//      旧实现对二进制顶点写死「3 个 float」，遇到多余字段会整体错位。
//   2. 生成平滑法线（面积加权平均，因为斯坦福兔子只有 x,y,z 无法线）
//   3. 中心化 + 均匀缩放（缩放到目标尺寸，法线保持单位长度）
//   4. 计算 OBB（当前用 AABB 近似，轴对齐；PCA 精确 OBB 留作后续）
//   5. 兜底：找不到文件 / 解析失败时生成程序化球体网格，保证样例开箱即跑
//
// 顶点布局固定为 { float3 position; float3 normal; }，与 RHI 的
// RHIVertexInputInfo{ _Fvec3, _Fvec3 } 一一对应。
//
// 二进制分支按 x86 小端直接读取（PLY 的 binary_little_endian）；binary_big_endian
// 未支持，会明确告警后走兜底球体，而不是静默解析出错误几何。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// ═══════════════════════════ PLY 头部属性表 ═══════════════════════════
// 只保留解析所必需的信息：元素名/数量、每个属性的类型与在二进制记录内的偏移。
enum class PlyType { None, I8, U8, I16, U16, I32, U32, F32, F64 };

static int PlyTypeSize(PlyType t) {
    switch (t) {
    case PlyType::I8:  case PlyType::U8:  return 1;
    case PlyType::I16: case PlyType::U16: return 2;
    case PlyType::I32: case PlyType::U32: case PlyType::F32: return 4;
    case PlyType::F64: return 8;
    default: return 0;
    }
}

struct PlyProperty {
    char    name[32] = { 0 };
    PlyType type = PlyType::None;       // 标量类型；isList 时为列表元素类型
    bool    isList = false;
    PlyType countType = PlyType::None;  // isList 时：列表长度字段类型
    int     offset = 0;                 // 二进制记录内偏移（仅标量属性有效）
};

struct PlyElement {
    char    name[32] = { 0 };
    size_t  count = 0;
    std::vector<PlyProperty> props;
    int     stride = 0;                 // 二进制记录步长
};

// 取第 n 个空白分隔 token（0 基）；越界返回 nullptr。返回的是行内指针，不做拷贝。
static const char* PlyNthToken(const char* line, int n) {
    const char* p = line;
    for (int i = 0; i <= n; ++i) {
        p += strspn(p, " \t\r\n");
        if (*p == '\0') return nullptr;
        if (i == n) return p;
        p += strcspn(p, " \t\r\n");
    }
    return nullptr;
}

// token 是否等于 word（token 以空白或 '\0' 结束；容忍前导空白）
static bool PlyTokEq(const char* tok, const char* word) {
    if (!tok) return false;
    tok += strspn(tok, " \t\r\n");
    size_t n = strlen(word);
    if (strncmp(tok, word, n) != 0) return false;
    char c = tok[n];
    return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static PlyType PlyParseType(const char* tok) {
    if (PlyTokEq(tok, "char")   || PlyTokEq(tok, "int8"))    return PlyType::I8;
    if (PlyTokEq(tok, "uchar")  || PlyTokEq(tok, "uint8"))   return PlyType::U8;
    if (PlyTokEq(tok, "short")  || PlyTokEq(tok, "int16"))   return PlyType::I16;
    if (PlyTokEq(tok, "ushort") || PlyTokEq(tok, "uint16"))  return PlyType::U16;
    if (PlyTokEq(tok, "int")    || PlyTokEq(tok, "int32"))   return PlyType::I32;
    if (PlyTokEq(tok, "uint")   || PlyTokEq(tok, "uint32"))  return PlyType::U32;
    if (PlyTokEq(tok, "float")  || PlyTokEq(tok, "float32")) return PlyType::F32;
    if (PlyTokEq(tok, "double") || PlyTokEq(tok, "float64")) return PlyType::F64;
    return PlyType::None;
}

static void PlyCopyToken(char* dst, size_t cap, const char* tok) {
    if (cap == 0) return;
    size_t i = 0;
    if (tok) {
        for (; i + 1 < cap && tok[i] && tok[i] != ' ' && tok[i] != '\t' && tok[i] != '\r' && tok[i] != '\n'; ++i) {
            dst[i] = tok[i];
        }
    }
    dst[i] = '\0';
}

// 按小端读出标量（x86 主机；PLY binary_little_endian）
static double PlyReadNumeric(const unsigned char* p, PlyType t) {
    switch (t) {
    case PlyType::I8:  { int8_t   v; memcpy(&v, p, 1); return (double)v; }
    case PlyType::U8:  { uint8_t  v; memcpy(&v, p, 1); return (double)v; }
    case PlyType::I16: { int16_t  v; memcpy(&v, p, 2); return (double)v; }
    case PlyType::U16: { uint16_t v; memcpy(&v, p, 2); return (double)v; }
    case PlyType::I32: { int32_t  v; memcpy(&v, p, 4); return (double)v; }
    case PlyType::U32: { uint32_t v; memcpy(&v, p, 4); return (double)v; }
    case PlyType::F32: { float    v; memcpy(&v, p, 4); return (double)v; }
    case PlyType::F64: { double   v; memcpy(&v, p, 8); return v; }
    default: return 0.0;
    }
}

// 行未读完时丢弃剩余部分（超长行保护，保证记录边界不串行）
static void PlySkipRestOfLine(FILE* f) {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {}
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
        char line[512];
        bool ascii = false, bigEndian = false;
        bool headerOk = false;
        std::vector<PlyElement> elems;

        if (fgets(line, sizeof(line), f) && strncmp(line, "ply", 3) == 0) {
            PlyElement* cur = nullptr;
            while (fgets(line, sizeof(line), f)) {
                if (!strchr(line, '\n')) PlySkipRestOfLine(f);   // 头部行都很短，防御性处理
                if (PlyTokEq(line, "format")) {
                    ascii = (strstr(line, "ascii") != nullptr);
                    bigEndian = (strstr(line, "big_endian") != nullptr);
                } else if (PlyTokEq(line, "element")) {
                    const char* name = PlyNthToken(line, 1);
                    const char* cnt = PlyNthToken(line, 2);
                    if (!name || !cnt) { headerOk = false; break; }
                    elems.emplace_back();
                    cur = &elems.back();
                    PlyCopyToken(cur->name, sizeof(cur->name), name);
                    cur->count = (size_t)strtoull(cnt, nullptr, 10);
                } else if (PlyTokEq(line, "property")) {
                    if (!cur) { headerOk = false; break; }
                    const char* t1 = PlyNthToken(line, 1);
                    PlyProperty prop;
                    if (PlyTokEq(t1, "list")) {
                        const char* ct = PlyNthToken(line, 2);
                        const char* it = PlyNthToken(line, 3);
                        const char* nm = PlyNthToken(line, 4);
                        if (!ct || !it || !nm) { headerOk = false; break; }
                        prop.isList = true;
                        prop.countType = PlyParseType(ct);
                        prop.type = PlyParseType(it);
                        PlyCopyToken(prop.name, sizeof(prop.name), nm);
                    } else {
                        const char* nm = PlyNthToken(line, 2);
                        if (!t1 || !nm) { headerOk = false; break; }
                        prop.type = PlyParseType(t1);
                        PlyCopyToken(prop.name, sizeof(prop.name), nm);
                    }
                    if (prop.type == PlyType::None || (prop.isList && prop.countType == PlyType::None)) {
                        Warn("PLY '{}': unknown property type, giving up", path);
                        headerOk = false;
                        break;
                    }
                    cur->props.push_back(prop);
                } else if (PlyTokEq(line, "end_header")) {
                    headerOk = true;
                    break;
                }
                // comment / obj_info 等一律忽略
            }
        }

        // 计算二进制步长（list 属性按约定位于元素末尾，不参与后续标量偏移）
        for (auto& e : elems) {
            int off = 0;
            for (auto& p : e.props) {
                p.offset = off;
                if (!p.isList) off += PlyTypeSize(p.type);
            }
            e.stride = off;
        }

        const PlyElement* vertElem = nullptr;
        const PlyElement* faceElem = nullptr;
        for (auto& e : elems) {
            if (!vertElem && strcmp(e.name, "vertex") == 0) vertElem = &e;
            if (!faceElem && strcmp(e.name, "face") == 0) faceElem = &e;
        }

        int px = -1, py = -1, pz = -1;
        if (vertElem) {
            for (size_t i = 0; i < vertElem->props.size(); ++i) {
                const char* n = vertElem->props[i].name;
                if (px < 0 && strcmp(n, "x") == 0) px = (int)i;
                else if (py < 0 && strcmp(n, "y") == 0) py = (int)i;
                else if (pz < 0 && strcmp(n, "z") == 0) pz = (int)i;
            }
        }
        int faceListIdx = -1;
        if (faceElem) {
            for (size_t i = 0; i < faceElem->props.size(); ++i) {
                if (!faceElem->props[i].isList) continue;
                if (faceListIdx < 0) faceListIdx = (int)i;
                if (strcmp(faceElem->props[i].name, "vertex_indices") == 0 ||
                    strcmp(faceElem->props[i].name, "vertex_index") == 0) { faceListIdx = (int)i; break; }
            }
        }

        const bool formatOk = headerOk && !bigEndian &&
                              vertElem && faceElem && vertElem->count > 0 && faceElem->count > 0 &&
                              px >= 0 && py >= 0 && pz >= 0 && faceListIdx >= 0;
        if (!formatOk) {
            if (headerOk && bigEndian) Warn("PLY '{}': binary_big_endian is not supported", path);
        }

        if (formatOk) {
            const PlyElement& V = *vertElem;
            const PlyElement& F = *faceElem;
            const PlyProperty& faceList = F.props[faceListIdx];
            positions.reserve(V.count * 3);
            faces.reserve(F.count * 3);

            if (ascii) {
                // 顶点：整行取第 px/py/pz 个 token（属性顺序任意，多余字段天然跳过）
                for (size_t i = 0; i < V.count; ++i) {
                    if (!fgets(line, sizeof(line), f)) break;
                    const char* tx = PlyNthToken(line, px);
                    const char* ty = PlyNthToken(line, py);
                    const char* tz = PlyNthToken(line, pz);
                    if (tx && ty && tz) {
                        positions.push_back(strtof(tx, nullptr));
                        positions.push_back(strtof(ty, nullptr));
                        positions.push_back(strtof(tz, nullptr));
                    }
                    if (!strchr(line, '\n')) PlySkipRestOfLine(f);
                }
                // 面：第一个 token 是顶点数，随后 3 个是三角形索引（n > 3 的多边形取前 3）
                for (size_t i = 0; i < F.count; ++i) {
                    if (!fgets(line, sizeof(line), f)) break;
                    const char* t0 = PlyNthToken(line, 0);
                    if (t0) {
                        long n = strtol(t0, nullptr, 10);
                        if (n >= 3) {
                            const char* ta = PlyNthToken(line, 1);
                            const char* tb = PlyNthToken(line, 2);
                            const char* tc = PlyNthToken(line, 3);
                            if (ta && tb && tc) {
                                faces.push_back((uint32_t)strtoul(ta, nullptr, 10));
                                faces.push_back((uint32_t)strtoul(tb, nullptr, 10));
                                faces.push_back((uint32_t)strtoul(tc, nullptr, 10));
                            }
                        }
                    }
                    if (!strchr(line, '\n')) PlySkipRestOfLine(f);
                }
            } else {
                // 顶点：整条记录读入，按属性偏移取 x/y/z，其余字段随步长跳过
                std::vector<unsigned char> rec((size_t)(V.stride > 0 ? V.stride : 1));
                const int off[3] = { V.props[px].offset, V.props[py].offset, V.props[pz].offset };
                const PlyType typ[3] = { V.props[px].type, V.props[py].type, V.props[pz].type };
                for (size_t i = 0; i < V.count; ++i) {
                    if (fread(rec.data(), 1, (size_t)V.stride, f) != (size_t)V.stride) break;
                    for (int k = 0; k < 3; ++k) positions.push_back((float)PlyReadNumeric(rec.data() + off[k], typ[k]));
                }
                // 面：<countType> n 后接 n 个 <indexType> 索引
                // buf 按最长标量（8 字节）预留：count 1 个 + 索引 3 个
                const size_t cntSize = (size_t)PlyTypeSize(faceList.countType);
                const size_t idxSize = (size_t)PlyTypeSize(faceList.type);
                unsigned char cntBuf[8];
                unsigned char idxBuf[3 * 8];
                for (size_t i = 0; i < F.count; ++i) {
                    if (fread(cntBuf, 1, cntSize, f) != cntSize) break;
                    long long n = (long long)PlyReadNumeric(cntBuf, faceList.countType);
                    if (n < 0) break;
                    if (n >= 3) {
                        if (fread(idxBuf, 1, idxSize * 3, f) != idxSize * 3) break;
                        for (int k = 0; k < 3; ++k)
                            faces.push_back((uint32_t)PlyReadNumeric(idxBuf + idxSize * (size_t)k, faceList.type));
                    }
                    if (n > 3) fseek(f, (long)((n - 3) * (long long)idxSize), SEEK_CUR);
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

            // 顶点/面都在，却一个三角形都没凑出来（索引越界等）也算失败
            if (!verts.empty() && !faces.empty()) {
                out.vertices = std::move(verts);
                out.indices  = std::move(faces);
                out.loaded   = true;
            }
        }
        fclose(f);
    }

    // ── 兜底：加载失败或文件不存在时用球体 ──
    if (!out.loaded) {
        Warn("PLY '{}' not found or failed to parse; using the procedural sphere fallback", path);
        GenerateFallbackSphere(out.vertices, out.indices);
        NormalizeMesh(out.vertices, targetSize);
        out.obbHalfExtents[0] = out.obbHalfExtents[1] = out.obbHalfExtents[2] = targetSize * 0.5f;
        out.boundingRadius = targetSize * 0.5f;
    }

    return out;
}

} // namespace FISIR
