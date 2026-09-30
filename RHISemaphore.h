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

		// 标记该信号量由驱动/外部（如交换链 acquire）而非命令缓冲 signal。
		// 提交循环据此把「等待它」视为已满足，不构成跨 CB 依赖。
		virtual void setExternalSignal(bool ext) =0;
		virtual bool isExternalSignal() const =0;

	};

}