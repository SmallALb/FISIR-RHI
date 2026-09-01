#pragma once

#include "RHITypes.h"

namespace FISIR {

	class RHISemaphore {
	public:

		virtual ~RHISemaphore() {};

		virtual void setWaitingStage(RHIUsingStageFlags stage) = 0;
	
		virtual RHIUsingStageFlags getWaitingStage() const = 0;

		virtual void* getSemaphoreHandle() const = 0;

		virtual bool wait(uint64_t timeout = UINT64_MAX) = 0;

		virtual FenceType getSemaphoreType() const = 0;
	};

}