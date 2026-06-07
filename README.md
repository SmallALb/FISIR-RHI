# FISIR渲染接口设计书 FISIR RHI Design

## 概述 Overview
  <details>
    <summary>中文</summary>
    <h3>目标</h3>
    <p>此项目为对现代图形API（Vulkan/dx）的接口抽象，目的是对底层调用进行简化抽象的同时保持高性能，能够进行更好的多线程渲染，以及对资源高效管理调度，同时减少对第三方库的依赖，能够直接部署执行。</p>
    <h3>目前实现和未来计划 （2026.6.7）</h3>
    <p>目前正逐步实现Vulkan RHI, dx12 RHI的实现将会在Vulkan RHI做完后面进行更新，现已经实现大部分基本的命令提交和资源调度功能，但是Vulkan RHI的显存屏障，以及画面输出这一部分还在逐步考虑执行设计中。（ps：因为是学生，所以还在边学习API边思考架构进行参考设计，如有任何设计不好的地方欢迎大佬指出(❁´◡`❁)）</p>
  </details>

  <details>
    <summary>English</summary>
    <h3>Goal</h3>
    <p>This project Abstracts modern Graphic APIs(Vulkan/DirectX 12).It aims to provide a simplified, hight-performance interface for using Graphic API while enabling better multithreading, effcient resource management and scheduling, and reducing thrid-party dependencies. The RHI is designed for direct deployment with minimal runtime overhead.</p>
    <h3>Now and Future （2026.6.7）</h3>
    <p>I'm currently implementing the Vulkan RHI backend. The DirectX 12 backend will follow after Vulkan is compele.Most basic command submission and resource scheduling functions have been compeleted.However,memory barrier and the presentation/output subsystem are still being designed. (Note: I'm a Student learning both graphics APIs and system design.if you see any poor design choices,please point them out. Tku!(❁´◡`❁))</p>
  </details>

## 目录 Contents

- ### [1. RHI抽象API DynamicRHI](./Design/DynamicRHI.md)
  - #### [1.1 VulkanRHI](./Design/DynamicRHI.md#11-VulkanRHI)
  - ####  [1.2 DX12RHI](./Design/DynamicRHI.md#12-DX12RHI)

- ### [2. 指令系统 RHICommand](./Design/RHICommand.md)
  - #### [2.2 RHICommandListBase](./Design/RHICommandListBase.md)
    - ##### [2.2.1 RHIRenderCommandList](./Design/RHIRenderCommandList.md)
    - ##### [2.2.2 RHIComputeCommandList](./Design/RHIComputeCommandList.md)
    - ##### [2.2.3 RHITransferCommandList](./Design/RHITransferCommandList.md)
  - ##### [2.3 CommandListExecutor](./Design/CommandListExecutor.md)
  - ##### [2.4 RHIContext](./Design/RHIContext.md)

- ### [4. 资源 RHIResource](./Design/RHIResource.md)
  - #### [4.1 RHIBuffer](./Design/RHIBuffer.md)
  - #### [4.2 RHITexture](./Design/RHITexture.md)

- ### [5. 渲染通道 RHIRenderPass](./Design/RHIRenderPass.md)

- ### [6. 着色器 RHIShader](./Design/RHIShader.md)

- ### [7. 管线 RHIPipeline](./Design/RHIPipeline.md)

- ### [8. 视口 RHIViewport](./Design/RHIViewport.md)

- ### [9. 窗口 RHIWindow](./Design/RHIWindow.md)

---------------------------------
<details>
  <summary><h2 style="display: inline;">修改 Fix</h2></summary>
  <details>
  <summary>中文</summary>
  <p>2026-6-7 第一次添加设计文档</p>
  </details>

  <details>
  <summary>English</summary>
  <p>2026-6-7 The first time adding a design document</p>
  </details>
</details>

------------------------------------
<details>
<summary><h2 style="display: inline;">任务 Task Table</h2></summary>

<details>
  <summary>中文</summary>

- [] 优化类和函数命名
- [] 消除RHI中所有的.cpp调用，只包含头文件
- [] 修改DynamicLoader为内联头文件
- [] 优化Vulkan Device中VkQueue的创建
- [] 设计RHIViewPort，并实现Vulkan的特化
- [] 修改编译脚本为CMake

</details>

<details>
  <summary>English</summary>

- [] Optimize the name of class and function
- [] Clear all the CPP File IN RHI
- [] Fix DynamicLoader.cpp to inline head file
- [] Optimize the VkQueue Creation
- [] Designing RHIViewPort, and implementing Vulkan version
- [] Modifying to CMake

</details>

</details>