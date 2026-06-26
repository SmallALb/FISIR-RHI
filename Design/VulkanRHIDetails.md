# Vulkan RHI Details

- ## Vulkan RHI Threads
  
  <details>
    <summary>中文</summary>

    在Vulkan的设计中有两个线程：

    - #### RHI Thread

      主要目的就是向VulkanQueue提交一系列的命令，并执行命令

    - #### Resource Thread

      主要目的是查看命令的执行情况（主要是查看Fence的状态），然后将CommandBuffer回收回CommandBufferPool池中，以及处理一些同步的问题


  </details>

  <details>
    <summary>English</summary>
  </details>

- ##  Texture Layout and Barrier

  <details>
  <summary>中文</summary>
    

  - ### 在渲染指令集中执行资源操作
  当资源数量较小的时候推荐使用渲染上线文提供的Transfer和Copy指令，可以减小跨队列的麻烦，效率更高。
  执行资源布局转换命令时，会记录资源将要变成的布局和操作，然后等待此次命令执行完毕后，在资源管理线程中修改在 VulkanTexture 的 布局显示。
  </details>

  <details>
    <summary>English</summary>
  </details>

- ## Present and SwapChain

  <details>
    <summary>中文</summary>
  对于Vk的画面呈现需要从Surface 中创建一个呈现队列或者直接使用图形队列作为呈现队列，那么就一定要创建surface 和 swapchain

  就是需要给整个屏幕一个铺满整个画面的长方型顶点，然后将渲染好的帧图像渲染到这个交换链图像上就好了，这一块的指令就在RHIThread中执行就最好
  
  目前要做的就是呈现的这一块，屏幕窗口由外部进行传入。然后根据当前的系统平台选择合适的surface 创建合适的交换链和presentQue
  </details>
  

  <details>
    <summary>English</summary>
  </details>

------------------------------------

  [Back To README](../README.md)
