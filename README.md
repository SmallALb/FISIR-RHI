# FISIR渲染接口设计书 FISIR RHI Design

## 概述 Overview
  <details>
    <summary>中文</summary>
     
 **FISIR-RHI** 为对现代图形API（Vulkan/DirectX 12）的轻量抽象库，目的是对底层调用进行简化抽象的同时保持高性能，能够进行更好的多线程渲染，以及对资源高效管理调度，同时减少对第三方库的依赖，能够直接部署执行。

### 核心设计目标

- ✅ **多线程友好**：命令缓冲独立录制，支持并行提交
- ✅ **资源生命周期可控**：显式内存管理，避免 GC 抖动
- ✅ **最小依赖**：仅依赖 Vulkan SDKSDK，可直接部署
    
  </details>

  <details>
    <summary>English</summary>
    <h3>Goal</h3>
    <p>This project Abstracts modern Graphic APIs(Vulkan/DirectX 12).It aims to provide a simplified, hight-performance interface for using Graphic API while enabling better multithreading, effcient resource management and scheduling, and reducing thrid-party dependencies. The RHI is designed for direct deployment with minimal runtime overhead.</p>
    <h3>Now and Future （2026.6.7）</h3>
    <p>I'm currently implementing the Vulkan RHI backend. The DirectX 12 backend will follow after Vulkan is compele.Most basic command submission and resource scheduling functions have been compeleted. (Note: I'm a Student learning both graphics APIs and system design.if you see any poor design choices,please point them out. Tku!(❁´◡`❁))</p>
  </details>


## Status & Roadmap

| Backend | Command Recording | Resource Management | Swapchain | Compute |
| :--- | :---: | :---: | :---: | :---: |
| **Vulkan** | ✅ | ✅ | ✅ | 🚧 |
| **DX12** | ⏳ | ⏳ | ⏳ | ⏳ |
