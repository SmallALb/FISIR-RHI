#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "RHITypes.h"
#include "RHICommands.h"
#include "RHISwapChain.h"
#include "RHIDisplay.h"
namespace FISIR {	
	class RHITexture;
	class RHIBuffer;
	class RHIViewport;
	class RHIPipeline;
	class RHIShader;
	class RHICommandListBase;
	class RHIResourcePack;
	class RHIRenderPass;
	class RHIResource;
	class RHIFrameBuffer;
	class RHISemaphore;
	class RHIFence;
	class RHISwapChain;
	class RHISampler;
	struct RHIPipelineState;
	struct RHIRenderPassInfo;
	
	class DynamicRHI {
	public:
		virtual ~DynamicRHI() {}
		virtual bool Init() = 0;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) = 0;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) = 0;

		// 建一个呈现目标。deviceType 决定 deviceHandle 指向哪种句柄布局（见 RHIDisplay.h）：
		//   · Win32Window → Win32DisplayHandle{ hinstance, hwnd }（就是示例里原来的 win32Data 布局）
		//   · Headless    → deviceHandle = nullptr：不建 surface/交换链，RHIGetSwapChain() 返回 nullptr，
		//                   调用方自行渲染到离屏目标（服务器/CI 渲染）
		//   · 其余类型见 RHIDisplay.h；后端未实现的会 Error 并返回一个没有 surface 的视口
		virtual RHIViewport* RHICreateViewport(uint32_t iniWidth, uint32_t initHeight, TextureCOLORType type,
											   DisplayDeviceType deviceType, void* deviceHandle,
											   uint32_t swapChainSlotCount = DEFAULT_SWAPCHAIN_SLOT_COUNT) = 0;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) = 0;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const char* EntryPoint, const unsigned char* Data, size_t size) = 0;

		virtual RHIResourcePackResult RHICreateResourcePack(const std::vector<RHIResource*>& resources) = 0;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) = 0;
	
		virtual RHIFrameBuffer* RHICreateFrameBuffer(uint32_t width, uint32_t height, const std::vector<RHITexture*>& textures, const RHIRenderPassInfo& info) = 0;
		
		virtual RHISemaphore* RHICreateSemaphore(const char* name = "Unnamed Semaphore", FenceType typ = FenceType::Binary) = 0;
		
		virtual void RHIDestroySemaphore(RHISemaphore* semaphore) = 0;

		virtual RHISampler* RHICreateSampler(const SamplerInfo& info) = 0;

		virtual RHIFence* RHICreateFence(bool signaled = false, const char* name = "Unnamed Fence") = 0;

		virtual RHISwapChain* RHIGetSwapChain(RHIViewport* viewport) = 0;
		
		virtual RingCommandPool::Page* RHIGetCommandPoolPage(CmdType cmdtype) = 0;

		virtual void RHIDestroyFence(RHIFence* fence) = 0;

		// 统一资源销毁接口：所有由 RHI 创建的动态资源，都应经由这些
		// 由 RHI 实现的接口销毁，确保 new/delete 在同一个模块（DLL）内
		// 发生，避免跨模块 new/delete 不匹配导致的堆损坏。
		virtual void RHIDestroyTexture(RHITexture* texture) = 0;

		virtual void RHIDestroyBuffer(RHIBuffer* buffer) = 0;

		virtual void RHIDestroySampler(RHISampler* sampler) = 0;

		virtual void RHIDestroyResourcePack(RHIResourcePackResult& pack) = 0;

		virtual void RHIDestroyFrameBuffer(RHIFrameBuffer* frameBuffer) = 0;

		// 最近一帧 GPU 执行耗时（毫秒）。基于 GPU 时间戳查询；后端不支持时返回 0。
		virtual double getLastGPUTimeMs() const { return 0.0; }

	};

}