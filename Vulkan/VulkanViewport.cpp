#include "VulkanViewport.h"
#include "VulkanDevice.h"
#include "../../Log/Logger.h"
#ifdef  _WIN32
	#define VK_USE_PLATFORM_WIN32_KHR
#endif //  _WIN32

#include <vulkan/vulkan.h>

#include "VulkanTexture.h"

#include "VulkanRenderPass.h"




#include "VulkanFrameBuffer.h"


#include "VulkanRHI.h"

namespace FISIR {
	
	extern VkInstance GetGlobalInstance();

	struct __VKViewportData {
		VkSurfaceKHR Surface;
		VkViewport Viewport;
		VkSwapchainKHR SwapChain;
		void * WindowHandle;
		VulkanTexture* OutPutBuffer;
		std::vector<VkImage> SwapChainImageHandles;
		std::vector<VkImageView> SwapChainImageViewHandles;

	};
#ifdef _WIN32
	static PFN_vkCreateWin32SurfaceKHR vkWind32SurfaceCreate = nullptr;
#endif //  _WIN32

	
	VullkanViewport::VullkanViewport(VulkanDevice* device, uint32_t iniWidth, uint32_t initHeight, void* WindowHandle) {
		mData = new __VKViewportData;
		mDevice = device;
		mData->Viewport.height = initHeight;
		mData->Viewport.width = iniWidth;
		mData->WindowHandle = WindowHandle;
	//Create Win32 WindowSurface
	#ifdef _WIN32
		struct WindowHandles {
			HINSTANCE hInstance;
			HWND hwnd;
		};
		auto winInstance = static_cast<WindowHandles*>(mData->WindowHandle);
		vkWind32SurfaceCreate = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(GetGlobalInstance(), "vkCreateWin32SurfaceKHR"));
		if (!vkWind32SurfaceCreate) {
			Error("Vulkan Win32 Create Function create failed");
			return;
		}
		VkWin32SurfaceCreateInfoKHR win32SurfaceCreateInfo {
			.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			.hinstance = winInstance->hInstance,
			.hwnd = winInstance->hwnd
		};

		if (vkWind32SurfaceCreate(GetGlobalInstance(), &win32SurfaceCreateInfo, nullptr, &mData->Surface) != VK_SUCCESS) {
			Error("Could Not Create Win32 Suraface");
			return;
		}
	#endif // _WIN32
	//Create SwapChain
		//-SwapChain
		uint32_t formatCount = 0;
		std::vector<VkSurfaceFormatKHR> vkSurfaceFormats;
		vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice->getPhysicalDevice(), mData->Surface, &formatCount, nullptr);
		vkSurfaceFormats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice->getPhysicalDevice(), mData->Surface, &formatCount, vkSurfaceFormats.data());

		VkSurfaceCapabilitiesKHR vkSurfaceCapabilitiesKHR;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice->getPhysicalDevice(), mData->Surface, &vkSurfaceCapabilitiesKHR);

		VkSurfaceFormatKHR choiceFormat;
		for (auto& F : vkSurfaceFormats) if (F.format == VK_FORMAT_R8G8B8A8_UNORM && F.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
			choiceFormat = F;
		}

		mDevice->initPresentQue(mData->Surface);
		

		uint32_t QuefamilyIndex[] = {mDevice->getGraphicQueue()->getFamilyIndex(), mDevice->getPresentQueue()->getFamilyIndex()};

		VkSwapchainCreateInfoKHR swapChainInfo {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = mData->Surface,
			.minImageCount = vkSurfaceCapabilitiesKHR.minImageCount,
			.imageFormat = choiceFormat.format,
			.imageColorSpace = choiceFormat.colorSpace,
			.imageExtent = {iniWidth, initHeight},
			.imageArrayLayers = vkSurfaceCapabilitiesKHR.maxImageArrayLayers,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 2,
			.pQueueFamilyIndices = QuefamilyIndex,
			.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = VK_PRESENT_MODE_MAILBOX_KHR
		};

		if (vkCreateSwapchainKHR(mDevice->getLogicalDevice(), &swapChainInfo, nullptr, &mData->SwapChain) != VK_SUCCESS) {
			Error("SwapChain Creat Failed");
			return;
		}
	//RenderTarget and RenderPass;
		uint32_t colorBufferCount = 0;
		vkGetSwapchainImagesKHR(mDevice->getLogicalDevice(), mData->SwapChain, &colorBufferCount, nullptr);
		mData->SwapChainImageHandles.resize(colorBufferCount);
		vkGetSwapchainImagesKHR(mDevice->getLogicalDevice(), mData->SwapChain, &colorBufferCount, mData->SwapChainImageHandles.data());
		
		VulkanSwapChainTextures.resize(colorBufferCount);
		mData->SwapChainImageViewHandles.resize(colorBufferCount);
		
		VulkanSwapChainFrameBuffers.resize(colorBufferCount);

		for (uint32_t i=0; i<colorBufferCount; i++) {
			mData->SwapChainImageViewHandles;
			VulkanSwapChainTextures[i] = new VulkanTexture(
				mDevice, mData->SwapChainImageHandles[i], (uint32_t)VK_FORMAT_R8G8B8A8_UNORM, {initHeight, iniWidth, 1});
				

			VkImageViewCreateInfo viewinfo {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = mData->SwapChainImageHandles[i],
				.format = VK_FORMAT_B8G8R8A8_UNORM,
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			};

			if (vkCreateImageView(mDevice->getLogicalDevice(), &viewinfo, nullptr, &mData->SwapChainImageViewHandles[i]) != VK_SUCCESS) {
				Error("Create Image View {} Failed", i);
				return;
			}
		}

		ColorEntry entry {
			.EntryPros = {
				.loadOp = RenderTargetLoadAction::Clear,
				.storeOp = RenderTargetStoreAction::Store,
				.initLayout = TextureLayout::Undefined,
				.dstLayout = TextureLayout::Present,
				.colorType = TextureCOLORType::RGBA_8,
			}
		};

		SubPassInfo subpass {
			.ColorEntryMask = 1<<0,
			.UseDepthStencil = false,
			.ReadDepthAsInput = false,
		};

		RHIRenderPassInfo renderpassinfo({{0, entry}}, {.exeit = 0}, {subpass});
		VulkanSwapChainRednerPass = new VulkanRenderPass(mDevice, renderpassinfo);
		
		//Create Frame;
		for (size_t i = 0; i < VulkanSwapChainFrameBuffers.size(); i++) {
			VulkanSwapChainFrameBuffers[i] = new VulkanFrameBuffer(mDevice, { VulkanSwapChainTextures[i] }, iniWidth, initHeight, VulkanSwapChainRednerPass);
		}
	}

	VullkanViewport::~VullkanViewport() {
		delete mData;
	}

	void* VullkanViewport::getNativeSwapChain() const {
		return mData->SwapChain;
	}

	void* VullkanViewport::getNativeBackBufferTexture() const {
		return nullptr;
	}

	void* VullkanViewport::getNativeWindow(void** handle) const {
		return mData->WindowHandle;
	}

	uint32_t VullkanViewport::getViewportWidth() const {
		return mData->Viewport.width;
	}

	uint32_t VullkanViewport::getViewportHeight() const {
		return mData->Viewport.height;
	}

	void VullkanViewport::tick(float deltatime) {

	}

	void VullkanViewport::waitForFrameEventCompletion() {

	}

	void VullkanViewport::IssueFrameEvent() {
	}

	VkSurfaceKHR_T* VullkanViewport::getVkSurface() const {
		return mData->Surface;
	}


}