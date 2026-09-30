<!-->
  <details>
    <summary>中文</summary>
  </details>

  <details>
    <summary>English</summary>
  </details>
<-->

# 1. 渲染接口 DynamicRHI
  
  <details>
  <summary>中文</summary>
    
  - ### 接口介绍和使用方法

  这是整个渲染接口的关键入口，所有的资源创建，指令上下文获取都得从这里开始，第一步就是创建DynamicRHI接口, 导入 include [RHICreator.h](../RHICreator.h)

  ```cpp
    enum class RHIAPI {
      Dx12,
      Vulkan
    };

    inline static bool setRenderInterfaceApi(RHIAPI api, const char* inPath = nullptr);

    //Main.cpp
    bool res = FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);

  ```
  调用此行后程序会在程序运行时链接动态库，如果没有传入需要的路径，**那么就会默认在RHI文件夹的前两级文件夹**<a id="CN-RHIDLL_PATH_FOUND_MEOTH_PROBLEM"></a>[❌](../README.md#CN_Task_FIX-RHIDLL_PATH_FOUND_MEOTH)，这么设计是为了程序能够在运行时进行API切换，在使用API一些针对性的功能的时候，无需关闭程序，更为方便快捷。

  在执行读取成功后，就可以创建和销毁RHI了

  ```cpp

    inline static DynamicRHI* getCurrentRenderInterface();
    
    inline static void destroyRenderInterface();

    //Main.cpp
      //创建RHI
    FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();

      //不再使用RHI则进行销毁
    FISIR::RHICreator::destroyRenderInterface();

      //如果不再使用API 可以直接释放动态库
    FISIR::RHICreator::freeCurrentRenderInterfaceApi();

  ```
  所有的API实现都预留导出RHI的创建和销毁接口

  ```cpp

  extern "C" {
    EXPORTDLL FISIR::DynamicRHI* RHICreate();

    EXPORTDLL void RHIDestroy(FISIR::DynamicRHI* rhi);
  }

  ```
  然后由动态库读取器读出函数填入函数指针，Creator调用函数指针来进行创建，当然你可以直接读取库后调用函数指针来创建多个API 但是不建议这么做

  创建完rhi就可以调用接口函数了[[Dynamic.h](../DynamicRHI.h)]

  接口列表如下:

  ```cpp
    //初始化
		virtual bool Init() = 0;

    //创建Gpu纹理
		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) = 0;

    //创建Gpu缓存
		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) = 0;

    //创建呈现目标（RHIViewPort）：deviceType 决定 deviceHandle 指向哪种句柄布局，见 RHIDisplay.h
    //  Win32Window → Win32DisplayHandle{ hinstance, hwnd }；Headless → nullptr（无 surface/交换链）
		virtual RHIViewport* RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type,
		                                       DisplayDeviceType deviceType, void* deviceHandle,
		                                       uint32_t swapChainSlotCount = DEFAULT_SWAPCHAIN_SLOT_COUNT) = 0;

    //创建管线
		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) = 0;

    //创建着色器
		virtual RHIShader* RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) = 0;

    //提交渲染指令
		virtual void RHISubmitCommandList(RHICommandListBase* cmdList, RHIFence* fence, const std::vector<RHISemaphore*>& waitSemaphore, const std::vector<RHISemaphore*>& singalSemaphore, std::atomic_bool* tag) = 0;

    //创建静态资源包
		virtual RHIResourcePackResult RHICreateResourcePack(const std::vector<RHIResource*>& resources) = 0;

    //创建渲染通道
		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) = 0;

    //创建帧缓冲
		virtual RHIFrameBuffer* RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) = 0;

    //创建信号量
		virtual RHISemaphore* RHICreateSemaphore(const char* name = "Unnamed Semaphore") = 0;
		
    //销毁信号量
		virtual void RHIDestroySemaphore(RHISemaphore* semaphore) = 0;

    //创建纹理采样器
		virtual RHISampler* RHICreateSampler(const SamplerInfo& info) = 0;

    //创建命令链表的上下文
		virtual void RHICreateContext(RHICommandListBase* cmdlist) = 0;

    //创建命令同步信号
		virtual RHIFence* RHICreateFence(bool signaled = false, const char* name = "Unnamed Fence") = 0;

    //创建交换链
		virtual RHISwapChain* RHIGetSwapChain(RHIViewport* viewport) = 0;

    //销毁命令同步信号
		virtual void RHIDestroyFence(RHIFence* fence) = 0;
  ```
  成功创建rhi后调用Init()

  ```cpp
   rhi->Init();
  ```
  之后所有的资源创建销毁都将通过rhi->RHIxxx来进行

  如何创建资源，进行渲染这些将会在后面的章节解释，以及为何这样设计
  </details>

  <details>
    <summary>English</summary>
  </details>

## 1.1 VulkanRHI

  <details>
    <summary>中文</summary>
  </details>

  <details>
    <summary>English</summary>
  </details>

## 1.2 DX12RHI

  <details>
    <summary>中文</summary>
    无
  </details>

  <details>
    <summary>English</summary>
    None
  </details>

------------------------------------

  [Back To README](../README.md)