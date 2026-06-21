#pragma once 

#include "RHIResource.h"

namespace FISIR {
	class RHIRenderCommandList;
	class DynamicRHI;

	class RHIViewport {
	public:
		virtual ~RHIViewport() {}

		virtual void* getNativeWindow(void** handle) const {return nullptr;}

		virtual uint32_t getViewportWidth() const {return 0;}

		virtual uint32_t getViewportHeight() const {return 0;}
		
		virtual void setViewportResize(uint32_t height, uint32_t width) = 0;

	
	};

}