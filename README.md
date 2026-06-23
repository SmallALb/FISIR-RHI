# FISIR渲染接口设计书 FISIR RHI Design

## 概述 Overview
  <details>
    <summary>中文</summary>
    <h3>目标</h3>
    <p>此项目为对现代图形API（Vulkan/dx）的接口抽象，目的是对底层调用进行简化抽象的同时保持高性能，能够进行更好的多线程渲染，以及对资源高效管理调度，同时减少对第三方库的依赖，能够直接部署执行。</p>
    <h3>目前实现和未来计划 （2026.6.7）</h3>
    <p>目前正逐步实现Vulkan RHI, dx12 RHI的实现将会在Vulkan RHI做完后面进行更新，现已经实现大部分基本的命令提交和资源调度功能。（ps：因为是学生，所以还在边学习API边思考架构进行参考设计，如有任何设计不好的地方欢迎大佬指出(❁´◡`❁)）</p>
  </details>

  <details>
    <summary>English</summary>
    <h3>Goal</h3>
    <p>This project Abstracts modern Graphic APIs(Vulkan/DirectX 12).It aims to provide a simplified, hight-performance interface for using Graphic API while enabling better multithreading, effcient resource management and scheduling, and reducing thrid-party dependencies. The RHI is designed for direct deployment with minimal runtime overhead.</p>
    <h3>Now and Future （2026.6.7）</h3>
    <p>I'm currently implementing the Vulkan RHI backend. The DirectX 12 backend will follow after Vulkan is compele.Most basic command submission and resource scheduling functions have been compeleted. (Note: I'm a Student learning both graphics APIs and system design.if you see any poor design choices,please point them out. Tku!(❁´◡`❁))</p>
  </details>

## 目录 Contents

- ### [1. RHI抽象API DynamicRHI](./Design/DynamicRHI.md)
  - #### [1.1 VulkanRHI](./Design/DynamicRHI.md#11-VulkanRHI)
  - ####  [1.2 DX12RHI](./Design/DynamicRHI.md#12-DX12RHI)

- ### [2. 指令系统 RHICommand](./Design/RHICommand.md)
  - #### [2.2 RHICommandListBase](./Design/RHICommand.md)
    - ##### [2.2.1 RHIRenderCommandList](./Design/RHICommand.md)
    - ##### [2.2.2 RHIComputeCommandList](./Design/RHICommand.md)
    - ##### [2.2.3 RHITransferCommandList](./Design/RHICommand.md)
  - ##### [2.3 CommandListExecutor](./Design/RHICommand.md)
  - ##### [2.4 RHIContext](./Design/RHICommand.md)

- ### [4. 资源 RHIResource](./Design/RHIResource.md)
  - #### [4.1 RHIBuffer](./Design/RHIResource.md#41-RHIBuffer)
  - #### [4.2 RHITexture](./Design/RHIResource.md#42-RHITexture)
  - #### [4.3 RHIResourcePack](/Design/RHIResource.md#43-RHIResourcePack)
- ### [5. 渲染通道 RHIRenderPass](./Design/RHIRenderPass.md)

- ### [6. 着色器 RHIShader](./Design/RHIShader.md)

- ### [7. 管线 RHIPipeline](./Design/RHIPipeline.md)

- ### [8. 帧缓冲 RHIFrameBuffer](./Design/RHIFrameBuffer.md)

- ### [9. 视口 RHIViewport](./Design/RHIViewport.md)

- ### [10. 窗口 RHIWindow](./Design/RHIWindow.md)

- ### [11.VulkanRHI Details](./Design/VulkanRHIDetails.md)

  - ### [11.1 VulkanRHI Threads](./Design/VulkanRHIDetails.md##Vulkan-RHI-Threads)

  - #### [11.2 Texture Layout and Barrier](./Design/VulkanRHIDetails.md##Texture-layout-and-barrier)

  - ### [11.3 Present and SwapChain](./Design/VulkanRHIDetails.md##Present-and-SwapChain)

---------------------------------
<details>
  <summary><h2 style="display: inline;">修改 Fix</h2></summary>
  <details>
  <summary>中文</summary>

  - 第一次添加设计文档 (2026-6-7)
  - 消除RHI中所有的.cpp调用，只包含头文件 (2026-6-8)

  </details>

  <details>
  <summary>English</summary>
  
  - The first time adding a design document (2026-6-7)
  - Clear all the .Cpp file in RHI (2026-6-8)
  
  </details>
</details>

------------------------------------
<details open>
<summary><h2 style="display: inline;">任务 Task Table</h2></summary>

<details open>
  <summary>中文</summary>

- [] 优化类和函数命名
- [x] 消除RHI中所有的.cpp调用，只包含头文件 (2026-6-8)
- [x] 修改DynamicLoader为内联头文件 (2026-6-8)
- [x] 优化Vulkan Device中VkQueue的创建 (2026-6-8)
- [x] 设计Layout转换和Barrier (2026-6-11)
- [x] 设计RHIViewPort，RHIFrameBuffer，并实现Vulkan的特化
- [] 修改编译脚本为CMake
<a id="CN_Task_FIX-RHIDLL_PATH_FOUND_MEOTH"></a>
- [] 修改RHIDLL自动路径匹配的方式[[DynamicRHI.md](./Design/DynamicRHI.md#CN-RHIDLL_PATH_FOUND_MEOTH_PROBLEM)][[RHICreator.h](RHICreator.h#19)]

</details>

<details open>
  <summary>English</summary>

- [] Optimize the name of class and function
- [x] Clear all the CPP File IN RHI (2026-6-8)
- [x] Fix DynamicLoader.cpp to inline head file (2026-6-8)
- [x] Optimize the VkQueue Creation (2026-6-8)
- [x] Design Layout Transition and Memory Barrier (2026-6-11)
- [x] Designing RHIViewPort, and implementing Vulkan version
- [] Modifying to CMake

</details>

</details>

----------------------