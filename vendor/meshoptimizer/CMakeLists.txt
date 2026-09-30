# ══════════════════════════════════════════════════════════════
# meshoptimizer v1.2 (vendored, MIT) — https://github.com/zeux/meshoptimizer
#
# **不建库目标**：第三方源码直接编进用到它的可执行文件（当前只有 Example/Nanite，
# 由 NaniteBuilder.h 离线生成簇与 BVH）。本文件只往外暴露两个变量（PARENT_SCOPE，
# 供其后 add_subdirectory 的示例继承）：
#     MESHOPTIMIZER_SOURCES       —— 全部 .cpp 列表
#     MESHOPTIMIZER_INCLUDE_DIRS  —— meshoptimizer.h / clusterlod.h 所在目录
# 消费方式：
#     add_executable(X ... ${MESHOPTIMIZER_SOURCES})
#     target_include_directories(X PRIVATE ${MESHOPTIMIZER_INCLUDE_DIRS})
#
# 目录名保持 vendor/meshoptimizer 而不是上游的 src/，避免与顶层 include 路径下的
# `src/...` 冲突；其余布局与上游一致，方便整体替换升级。
#   - src/meshoptimizer.h      core library (meshlets, simplification, partitioning)
#   - demo/clusterlod.h        Nanite-style cluster LOD hierarchy builder
#   - demo/clusterlod.cpp      single TU that enables CLUSTERLOD_IMPLEMENTATION
# ══════════════════════════════════════════════════════════════

set(MESHOPTIMIZER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(MESHOPTIMIZER_SOURCES
    ${MESHOPTIMIZER_DIR}/src/allocator.cpp
    ${MESHOPTIMIZER_DIR}/src/clusterizer.cpp
    ${MESHOPTIMIZER_DIR}/src/indexanalyzer.cpp
    ${MESHOPTIMIZER_DIR}/src/indexcodec.cpp
    ${MESHOPTIMIZER_DIR}/src/indexgenerator.cpp
    ${MESHOPTIMIZER_DIR}/src/meshletcodec.cpp
    ${MESHOPTIMIZER_DIR}/src/meshletutils.cpp
    ${MESHOPTIMIZER_DIR}/src/opacitymap.cpp
    ${MESHOPTIMIZER_DIR}/src/overdrawoptimizer.cpp
    ${MESHOPTIMIZER_DIR}/src/partition.cpp
    ${MESHOPTIMIZER_DIR}/src/quantization.cpp
    ${MESHOPTIMIZER_DIR}/src/rasterizer.cpp
    ${MESHOPTIMIZER_DIR}/src/simplifier.cpp
    ${MESHOPTIMIZER_DIR}/src/spatialorder.cpp
    ${MESHOPTIMIZER_DIR}/src/stripifier.cpp
    ${MESHOPTIMIZER_DIR}/src/tangentspace.cpp
    ${MESHOPTIMIZER_DIR}/src/vcacheoptimizer.cpp
    ${MESHOPTIMIZER_DIR}/src/vertexcodec.cpp
    ${MESHOPTIMIZER_DIR}/src/vertexfilter.cpp
    ${MESHOPTIMIZER_DIR}/src/vfetchoptimizer.cpp
    ${MESHOPTIMIZER_DIR}/demo/clusterlod.cpp
    PARENT_SCOPE
)

set(MESHOPTIMIZER_INCLUDE_DIRS
    ${MESHOPTIMIZER_DIR}/src
    ${MESHOPTIMIZER_DIR}/demo
    PARENT_SCOPE
)
