#pragma once

#include "../RHIViewport.h"
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

		VulkanViewport(DynamicRHI* rhi, TextureCOLORType colorType, uint32_t iniWidth, uint32_t initHeight, void* WindowHandle);

		~VulkanViewport();

		virtual void* getNativeWindow(void** handle) const override;

		virtual uint32_t getViewportWidth() const override;

		virtual uint32_t getViewportHeight() const override;

		virtual void setViewportResize(uint32_t height, uint32_t width) override;


		VkSurfaceKHR_T* getVkSurface() const;

		uint32_t getVulkanColorFormat() const;

		TextureCOLORType ImageColorType;
		__VKViewportData* mData{ nullptr };
	};

}