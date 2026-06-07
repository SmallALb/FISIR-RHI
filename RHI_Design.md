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
  
- ### RHI抽象API DynamicRHI

  - #### VulkanRHI
 
  - #### DX12RHI

- ### 指令系统 RHICommand

  - #### RHICommandListBase

    - ##### RHIRenderCommandList

    - ##### RHIComputeCommandList

    - #### RHITransferCommandList

  - #### CommandListExecutor

  - #### RHIContext

- ### 资源 RHIResource

  - #### RHIBuffer

  - #### RHITexture

- ### 渲染通道 RHIRenderPass

- ### 着色器 RHIShader

- ### 管线 RHIPipeline

- ### 视口 RHIViewport

- ### 窗口 RHIWindow