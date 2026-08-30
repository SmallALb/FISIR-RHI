<div align="center">

# FISIR RHI

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) ![API](https://img.shields.io/badge/API-Vulkan%20%7C%20D3D12-orange.svg) ![C++](https://img.shields.io/badge/C++-20-00599C?logo=cplusplus&logoColor=white) ![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows&logoColor=white)
这是一个轻量级、跨API的渲染硬件接口，在尽量减少第三方依赖的同时，保证高效率渲染，高性能计算
</div>

-----------------------------

#### 可以干什么

- 图形学学习和实验
- 作为引擎的渲染底层
- 高性能计算


#### 特性

- 高度并行，支持多线程录制命令，多线程提交，完全为多核CPU而设计的架构
- 彻底采用现代图形API **（Vulkan | D3D12）** 和现代化的接口设计
- 无锁环形缓冲命令池 + 三线程执行管线，将命令录制、翻译、提交与资源回收解耦

#### 系统要求

- Windows 10/11
- 支持 Vulkan 1.x 的显卡与驱动
- MSVC（C++20）、CMake 3.21+
- [Vulkan SDK](https://vulkan.lunarg.com/)（含 DXC，`dxcompiler.dll` 需在 `PATH` 或与可执行文件同目录）

#### 构建与运行

```bash
# 在仓库根目录配置
cmake -B out -S .
# 构建（Debug 或 Release）
cmake --build out --config Release
# 运行示例（工作目录需为输出目录）
cd out/bin/Release && ./TextureCube.exe
```

- 示例程序从 `./RHI/Vulkan/` 相对路径加载 `RHIVK.dll`
- 可用 `-DBUILD_VULKAN_RHI=OFF` / `-DBUILD_EXAMPLES=OFF` 关闭对应目标

#### 性能基准

> **测试环境**：Intel Core i5-13500HX（14C/20T）· 32 GB DDR5 · NVIDIA GeForce RTX 4060 Laptop（8 GB）· Windows 11

> **构建**：MSVC（cl 14.51）+ Ninja，Debug 与 Release 各测一次

<div align="center">


##### TextureCube —— 绘制调用 / 实例数基准
 **负载**：离屏渲染 1024×1024，每帧 2 个渲染通道（离屏 + 呈现），旋转立方体
</div>


###### 使用方式

```bash
# 测试 10 / 100 / 1000 次 draw call，每组 2000 帧，各剔除 60 帧预热
TextureCube.exe -Test -DC 10 -DC 100 -DC 1000 -Frames 2000 -Warmup 60
```

| 参数 | 含义 | 默认 |
| --- | --- | --- |
| `-Test` | 启用性能测试，跑完自动导出报告并退出 | — |
| `-DC N` | 每帧绘制调用数，可多次指定（每个 `-DC` 生成一组测试） | 1 |
| `-Frames N` | 每组测试帧数 | 5000 |
| `-Warmup N` | 剔除的预热帧数（避开首帧编译/缓存未命中） | 60 |

每组导出 `PerfReport_DC<N>.md` 与 `PerfFrameTimes_DC<N>.csv`。

###### 结果（每组 1940 有效帧）

| 每帧 draw call | Debug 帧时间 | Debug FPS | Release 帧时间 | Release FPS |
| --- | --- | --- | --- | --- |
| 10 | 0.480 ms | 2083 | 0.283 ms | 3532 |
| 100 | 0.611 ms | 1636 | 0.438 ms | 2284 |
| 1000 | 2.255 ms | 443 | 2.430 ms | 412 |

###### 资源占用

- Release 模式下 TextureCube 示例进程内存占用约 **244.7 MB**（含纹理、交换链、描述符池、命令池等，随交换链帧数/纹理尺寸/池大小变化）。

###### 性能特征

- **边际 draw call 成本约 2 µs**：按 `(avg[1000] − avg[10]) / 990` 估算，Debug ≈1.8 µs、Release ≈2.2 µs，两者同量级——说明这 2 µs 是 Vulkan 驱动录制 + 命令写入环形缓冲的开销，而非应用侧 CPU 计算（否则 Release 应显著更快）。
- **固定每帧开销约 0.26 ms（Release）/ 0.46 ms（Debug）**：acquire fence 等待 + 提交确认 + 双通道等与 draw call 数量无关的固定成本。
- **高 draw call 时 Debug/Release 趋同**：1000 draw call 时两种构建帧时间几乎一致（2.26 vs 2.43 ms），瓶颈从应用 CPU 转移到驱动录制与提交同步。
- **尾延迟由同步点主导**：DC10 时 Release 平均 0.283 ms，但 P99 高达 1.08 ms，与 draw call 数量和优化等级均无关。

<div align="center">


##### BunnyPBR —— 多实例 PBR + 碰撞仿真基准
**负载**：斯坦福兔子 `bunny.ply` 实例化渲染 ×N（GGX dielectric PBR + 方向光 + 点光），每帧一次 compute 刚体碰撞仿真（积分 + OBB-OBB SAT 冲量 + 撞墙 + 自旋）。离屏 1280×720，每帧 2 渲染通道（离屏 PBR + 呈现）+ 1 compute dispatch。
</div>

###### 使用方式

```bash
# 测试 10 / 100 / 500 / 1000 / 2000 只兔子，每组 5000 帧，各剔除 100 帧预热
BunnyPBR.exe -Test -C 10 -C 100 -C 500 -C 1000 -C 2000 -Frames 5000 -Warmup 100
```

| 参数 | 含义 | 默认 |
| --- | --- | --- |
| `-Test` | 启用性能测试，跑完自动导出报告并退出 | — |
| `-C X` | 兔子数量（实例数），可多次指定（每个 `-C` 生成一组测试） | 10 |
| `-Frames N` | 每组测试帧数 | 5000 |
| `-Warmup N` | 剔除的预热帧数 | 60 |

每组导出 `PerfReport_C<X>.md` 与 `PerfFrameTimes_C<X>.csv`。

###### 结果（每组 4900 有效帧，每兔 16301 三角面）

| 兔子数量 | 总三角面 | Debug 帧时间 | Debug FPS | Release 帧时间 | Release FPS |
| --- | --- | --- | --- | --- | --- |
| 10 | 163,010 | 0.744 ms | 1344 | 0.472 ms | 2117 |
| 100 | 1,630,100 | 1.050 ms | 952 | 0.846 ms | 1182 |
| 500 | 8,150,500 | 2.726 ms | 367 | 2.822 ms | 354 |
| 1000 | 16,301,000 | 5.028 ms | 199 | 5.114 ms | 195 |
| 2000 | 32,602,000 | 9.884 ms | 101 | 9.786 ms | 102 |

###### 模型数据

- `Res/bunny.ply`：Stanford 兔子的降采样版本（8171 顶点 / 16301 三角面，原始 bun_zipper 为 35947 顶点），ASCII PLY；加载时中心化 + 均匀缩放 + 面积加权法线生成。
- 缺失 `.ply` 时自动退化为程序化球体网格，保证开箱即跑。

###### 性能特征

- **帧时间随兔子数量近似线性增长**：Release 下 10 → 2000 只，0.472 → 9.786 ms；2000 只（3260 万三角面）仍约 100 FPS，瓶颈为几何/光栅化吞吐。
- **边际每兔成本约 4.7 µs、固定开销约 0.42 ms**：按 `(avg[2000] − avg[10]) / 1990` 估算 Release 约 4.7 µs/兔，外推每帧固定开销约 0.42 ms（acquire 等待 + 提交确认 + 双通道 + compute 串行化）。
- **低实例数 Release 显著快于 Debug，高实例数趋同**：10 只 0.472 vs 0.744 ms（约 1.6×）；2000 只 9.786 vs 9.884 ms——与 TextureCube 一致，瓶颈从 CPU 侧录制/提交转移到 GPU 计算与光栅化。
- **每帧含一次 compute 仿真**：由 `computeFence->wait()` 串行化，帧时间含 CPU 等待 compute 完成的耗时。
- **尾延迟由同步点主导**：C1000 平均 5.114 ms 而 P99 达 9.418 ms，主要来自 acquire 等待与提交确认。

> **测量口径**：仅 CPU 侧帧时间（含 acquire 等待与提交确认），当前 RHI 尚未暴露 GPU 时间戳查询，故不含纯 GPU 耗时；Mailbox 无垂直同步，FPS 为原始吞吐而非刷新率锁定值。以上数据为固定测试环境的相对值，用于横向对比构建/调用量，不代表绝对吞吐。


<div align="center">


![bunny](Design/buddy.gif)
##### 此为BunnyPBR的渲染效果
</div>

#### 目前状态和工作

 - 现已完成 Vulkan 后端的大部分功能（含计算管线，compute dispatch 已用于 GPU 剔除），D3D12 后端还未开始设计编写
 - PSO缓存管理，以及一些资源的管理方式任然需要进一步的优化和简化
 - 对于着色器目前只支持使用DXC编译hlsl为SPIR-V，后续将会将其修改为slang着色器语言
 - 此渲染接口目前只能在windows平台中编译运行，更多平台的兼容和测试仍在开发中
 - 目前正在使用该RHI实现Nanite的样例，因此大部分的这些问题都会在边实现边完善或实现后进行完善（个人精力有限）

