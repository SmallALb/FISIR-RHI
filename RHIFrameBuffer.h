#pragma once

#include "RHITexture.h"
#include <vector>

namespace FISIR {
	class RHIRenderPass;

	class RHIFrameBuffer : public RHIResource {
	public:
		virtual ~RHIFrameBuffer() {}

		virtual RHIRenderPass* getFrameRenderPass() const = 0;

		virtual uint32_t getFrameWidth() const = 0;

		virtual uint32_t getFrameHeight() const = 0;

		virtual std::vector<RHITexture*>& getFrameTextures() = 0;

		virtual Type getResourceType() const override {return Type::FrameBuffer;}
	};

}