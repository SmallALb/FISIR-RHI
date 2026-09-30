#pragma once 

#include "RHIResource.h"
#include "RHIDisplay.h"

namespace FISIR {
	class RHIRenderCommandList;
	class DynamicRHI;

	class RHIViewport {
	public:
		virtual ~RHIViewport() {}

		virtual void* getNativeWindow(void** handle) const {return nullptr;}

		// 本视口是照哪一类呈现设备建的。Headless = 没有 surface、没有交换链，只做离屏渲染。
		virtual DisplayDeviceType getDisplayDeviceType() const { return DisplayDeviceType::Win32Window; }

		virtual uint32_t getViewportWidth() const {return 0;}

		virtual uint32_t getViewportHeight() const {return 0;}
		
		virtual void setViewportResize(uint32_t height, uint32_t width) = 0;

	
	};

}