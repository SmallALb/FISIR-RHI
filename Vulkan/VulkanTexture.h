#pragma once

#include "../RHITexture.h"
#include "VulkanResourceInterface.h"

struct VkImage_T;

namespace FISIR {
	class VulkanDevice;
	
	struct __VkTextureData;
	
	

	class VulkanTexture : public RHITexture, VulkanResource {
	public:
		VulkanTexture(VulkanDevice* inDevice, const TextureInfo& info, uint32_t usage = 0, const char* name = nullptr);

		VulkanTexture(VulkanDevice* inDevice, VkImage_T* imagehandle, size_t format,
			const TextureSize& size, const char* name = nullptr);

		virtual ~VulkanTexture();

		virtual void* getResourceAPIHandle() const override;

		virtual TextureSize getTextureSize() const override;

		virtual TextureLayout getCurrentLayout() const override;


		virtual TextureCOLORType getColorType() const override;

		virtual Type getResourceType() const override { return Type::Texture; }

		virtual uint32_t getVkDescriptorType() const override;

		uint32_t getVkColorType() const;

		uint32_t getVkTextureLayout() const;

		virtual uint32_t getSampleCount() const override;

		virtual TextureUseForFlags getTextureUseFor() const override;

		virtual TextureType getTextureType() const override;


		virtual void* changeOtherHandle(const std::type_info& typ) override {
			if (typ == typeid(VulkanResource)) return static_cast<VulkanResource*>(this);
			else return static_cast<RHIResource*>(this);
		}
		

		virtual uint16_t getLayerCount() const override;

		virtual uint16_t getMipLevelCount() const override;

		void transitionLayout(TextureLayout newLayout);

		__VkTextureData* mData;
	private:
		VulkanDevice* mDevice;
	};
}

