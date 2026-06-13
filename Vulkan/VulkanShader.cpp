#include "VulkanShader.h"
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
namespace FISIR {
    struct __VkShaderData {
        VkShaderModule mShader;
    };


    VulkanShader::VulkanShader(VulkanDevice* device, const unsigned char* shaderData, size_t size) {
        mData = new __VkShaderData();
        mDevice = device;

		VkShaderModuleCreateInfo shaderModuleCreateInfo = {
		   .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		   .codeSize = size, //TODO: set code size
		   .pCode = (const uint32_t*)shaderData //TODO: set code data
		};


		vkCreateShaderModule(mDevice->getLogicalDevice(), &shaderModuleCreateInfo, nullptr, &mData->mShader);
    }

    VulkanShader::~VulkanShader() {
        vkDestroyShaderModule(mDevice->getLogicalDevice(), mData->mShader, nullptr);
        delete mData;
    }

    void* VulkanShader::getResourceAPIHandle() const {
        return mData->mShader;
    }

}
