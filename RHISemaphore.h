#pragma once


namespace FISIR {

	class RHISemaphore {
	public:
		virtual ~RHISemaphore() {};
	
		virtual void* getSemaphoreHandle() const = 0;

		virtual void wait() = 0;
	};

}