# Vulkan RHI Details

- ##  Texture Layout and Barrier

  <details>
  <summary>中文</summary>
    
  通过CmdList调用 **TransitionTextures** 
  Q1: 多次转换命令编译时，一个转换一旦提交，那么之前的转换命令全部编译无效
   （手动控制？）
  Q2: 编译时需要添加专门的优化命令，特别是跨队列使用的屏障命令
   （因此，Executor应该修改为可抽象的实现）
  Q3: 如何处理队列之间的信号量？
  以上的问题暂时留着了，因为不可能一会就完成，先修改渲染上下文可使用转换和资源复制：
  1 在渲染上下文中添加对应资源操作指令

  - ### 在渲染指令集中执行资源操作
  当资源数量较小的时候推荐使用渲染上线文提供的Transfer和Copy指令，可以减小跨队列的麻烦，效率更高
  执行资源布局转换命令时，会记录资源，然后等待此次命令执行完毕后，在资源管理线程中立即修改在 VulkanTexture 的 layou显示

  - ### 在资源指令集中执行纹理操作

  </details>

  <details>
    <summary>English</summary>
  </details>


------------------------------------

  [Back To README](../README.md)