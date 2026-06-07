#pragma once

namespace FISIR{
	class VulkanResource {
	public:
		virtual uint32_t getVkDescriptorType() const = 0;
	};
}