BunnyPBR 资源目录
=================

本目录内置两级斯坦福兔子网格：

1. bunny_hi.ply —— 高采样原始扫描件（首选）
   - 来源：Stanford 3D Scanning Repository 原始归档
     http://graphics.stanford.edu/pub/3Dscanrep/bunny.tar.gz
     （旧路径 /data/3Dscanrep/bunny.tar.gz 已 404；/pub/ 路径可用）
     归档内文件为 bunny/reconstruction/bun_zipper.ply，此处按样例命名约定
     重命名为 bunny_hi.ply。
   - 35947 顶点 / 69451 三角面，ASCII PLY。归档自带 README 原文：
     "The first file is the high resolution result, while the _res* files are
      decimated versions." —— 即 bun_zipper.ply 是官方最高采样版本。
   - 顶点字段为 x, y, z, confidence, intensity（后两个与几何无关），
     PLY 属性表驱动加载，多余字段按声明步长跳过。

2. bunny.ply —— 降采样版本（回退用）
   - bun_zipper_res2 的镜像副本（8171 顶点 / 16301 三角面，ASCII PLY）。
     来源：https://raw.githubusercontent.com/mikedh/trimesh/main/models/bunny.ply
   - 渲染更快，适合低端 GPU 或只想跑通流程时使用。

加载顺序：Res/bunny_hi.ply → Res/bunny.ply → 程序化球体兜底；
也可用 `BunnyPBR.exe -Model <path.ply>` 显式指定任意 PLY。

PLYLoader 支持 ASCII / binary_little_endian（属性表驱动：顶点多余字段与
n > 3 的面多边形均会自动跳过）；binary_big_endian 会告警后走球体兜底。
