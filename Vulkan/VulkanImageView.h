#pragma once


struct VkImageView_T;

namespace FISIR {
	class VulkanDevice;
	class VulkanTexture;

	struct __VkImageViewData;

	class VulkanImageView {
	public:
		VulkanImageView(VulkanDevice* device, VulkanTexture* texture = nullptr);

		~VulkanImageView();
		
		void build(VulkanTexture* texture);

		void destroyCurrent();

		VkImageView_T* getImageViewHandle() const;
	private:
		__VkImageViewData* mData;
		VulkanDevice* mDevice;
	};

	struct __VkImageViewManagerData;


	class VulkanImageViewManager {
	public:
		VulkanImageViewManager(VulkanDevice* device);

		~VulkanImageViewManager();

		VulkanImageView* getViewToTexture(VulkanTexture* texture);

		void freeViewToTexture(VulkanTexture* texture);

	private:

		VulkanDevice* mDevice;
		std::vector<VulkanImageView*> ImageViews;
		__VkImageViewManagerData* mData;
	};


}


