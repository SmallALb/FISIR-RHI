#pragma once
#include "RHIResource.h"

namespace FISIR {
	
	struct BufferInfo {
		void* data_CPU;
		uint64_t size;
		uint64_t stride;
		BufferLayout bufferlayout;
		MemType memoryType;
	};

	class RHIBuffer : public RHIResource {
	public:

		virtual const char* outPutString() const override {
			return "RHIBuffer";
		}

		virtual void* getBufferData() const = 0;

		virtual void updateBufferData(void* Data, size_t size) = 0;

		virtual Type getResourceType() const override { return Type::Buffer; }



	};
	struct BufferTransitionInfo {
		RHIBuffer* buffer;
		ResourceAccess newAccess;
	};
}