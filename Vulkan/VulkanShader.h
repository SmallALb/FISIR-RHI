#pragma once

#include "../RHIShader.h"

	

namespace FISIR {
	class VulkanDevice;
	
	struct __VkShaderData;

	class VulkanShader : public RHIShader {
	public:
		VulkanShader(VulkanDevice* device, const char* EntryPoint, const unsigned char* shaderData, size_t size);

		~VulkanShader();

		virtual void* getResourceAPIHandle() const override;

		virtual const char* getEntryPoint() const override;

		__VkShaderData* mData;

		VulkanDevice* mDevice;

		const char* mEntryPoint = "main";

	};
}

