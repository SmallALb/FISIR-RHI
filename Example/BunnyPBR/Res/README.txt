BunnyPBR 资源目录
=================

本目录已内置 bunny.ply（斯坦福兔子 bun_zipper 网格，ASCII PLY）。

来源：Stanford 3D Scanning Repository 的原始下载链接
（http://graphics.stanford.edu/data/3Dscanrep/bunny.tar.gz）已失效（404），
故改用 GitHub 镜像（trimesh 库随附的测试模型）：
  https://raw.githubusercontent.com/mikedh/trimesh/main/models/bunny.ply

说明：
  - 该镜像为 bun_zipper 的降采样版本：8171 顶点 / 16301 三角面（原始为 35947 顶点）。
    对 PBR + 碰撞演示而言足够平滑，且渲染更快。
  - PLYLoader 支持 ASCII / binary_little_endian；顶点多余字段（confidence/intensity）
    与面多边形（n > 3 时）会被自动跳过。

若未提供 bunny.ply，样例会自动退化为程序化球体网格，保证开箱即跑。
