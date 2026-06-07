#pragma once
#include "RHIResource.h"

namespace FISIR {

	class RHIBuffer : public RHIResource {
	public:

		virtual const char* outPutString() const override {
			return "RHIBuffer";
		}

		virtual void* getBufferData() const = 0;

		virtual void updateBufferData(void* Data, size_t size) = 0;

		virtual Type getResourceType() const override { return Type::Buffer; }



	};

}