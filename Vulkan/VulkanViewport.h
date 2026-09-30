#pragma once

#include "../RHIViewport.h"
#include "../RHISwapChain.h"   // DEFAULT_SWAPCHAIN_SLOT_COUNT
#include <atomic>

struct VkSurfaceKHR_T;
struct VkFence_T;
namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;
	class VulkanFrameBuffer;
	class VulkanRenderPass;
	class VulkanPipeline;
	class VulkanImageView;
	class VulkanQueue;
	class RHIShader;
	class DynamicRHI;
	class VulkanSemaphore;
	class VulkanFence;

	struct __VKViewportData;

	class VulkanViewport : public RHIViewport {
	public:

		VulkanViewport(DynamicRHI* rhi, TextureCOLORType colorType, uint32_t iniWidth, uint32_t initHeight,
					   DisplayDeviceType displayType, void* deviceHandle,
					   uint32_t swapChainSlotCount = DEFAULT_SWAPCHAIN_SLOT_COUNT);

		~VulkanViewport();

		virtual void* getNativeWindow(void** handle) const override;

		virtual DisplayDeviceType getDisplayDeviceType() const override { return DisplayType; }

		// 有没有真正的 surface。Headless（以及本后端未实现的设备类型）没有。
		bool hasSurface() const;

		virtual uint32_t getViewportWidth() const override;

		virtual uint32_t getViewportHeight() const override;

		virtual void setViewportResize(uint32_t height, uint32_t width) override;

		// 创建 viewport 时用户指定的槽位数（帧在飞数）。设备初始化时据此创建交换链；
		// 交换链会把它夹取到「≤ 交换链图像数」，实际生效值用 RHISwapChain::getSlotCount() 取。
		uint32_t getSwapChainSlotCount() const { return SwapChainSlotCount; }


		VkSurfaceKHR_T* getVkSurface() const;

		uint32_t getVulkanColorFormat() const;

		TextureCOLORType ImageColorType;
		__VKViewportData* mData{ nullptr };

	private:
		// 只为 Win32Window 建 surface；句柄布局 = Win32DisplayHandle{ hinstance, hwnd }
		void CreateWin32Surface();

		uint32_t SwapChainSlotCount{ DEFAULT_SWAPCHAIN_SLOT_COUNT };
		DisplayDeviceType DisplayType{ DisplayDeviceType::Win32Window };
		// 建 surface 用到的句柄副本（按 DisplayType 解释；Headless/未实现类型不填）
		struct Win32Data { void* hinstance; void* hwnd; } WindowHandle{ nullptr, nullptr };
	};

}