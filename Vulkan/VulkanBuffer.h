#pragma once

#include "../RHIBuffer.h"
#include "VulkanResourceInterface.h"
namespace FISIR {
	class VulkanDevice;


	struct __VKBufferData;

	class VulkanBuffer : public RHIBuffer, VulkanResource {
	public:
		VulkanBuffer(VulkanDevice* device, const BufferInfo& info, uint32_t Usage = 0, const char* DebugName = nullptr);

		~VulkanBuffer();

		virtual size_t getSize() const override;

		virtual void* getBufferData() const { return nullptr; }

		virtual void updateBufferData(void* Data, size_t size) override;

		virtual Type getResourceType() const  override { return Type::Buffer; }

		virtual void* getResourceAPIHandle() const override;

		virtual MemType getResourceMemType() const override;

		uint32_t getMemoryOffset() const;

		uint32_t getMemorySize() const;

		uint32_t getVkDescriptorType() const override;

		uint32_t getDeviceAddress() const;

		void*& getHostVisablePtr() {return Data_GPU;}

		virtual void* changeOtherHandle(const std::type_info& typ) override {
			if (typ == typeid(VulkanResource)) return static_cast<VulkanResource*>(this);
			else return static_cast<RHIResource*>(this);
		}

	private:
		VulkanDevice* mDevice;
		__VKBufferData* mData;
		void* Data_GPU{ nullptr };
	};

}

