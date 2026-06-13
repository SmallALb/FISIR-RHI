#pragma once

#include "RHITexture.h"

namespace FISIR {
	class RHIRenderPass;

	class RHIFrameBuffer : public RHIResource {
	public:
		virtual ~RHIFrameBuffer() {}

		virtual RHIRenderPass* getFrameRenderPass() const = 0;

		virtual uint32_t getFrameWidth() const = 0;

		virtual uint32_t getFrameHeight() const = 0;

		virtual Type getResourceType() const override {return Type::FrmeBuffer;}
	};

}