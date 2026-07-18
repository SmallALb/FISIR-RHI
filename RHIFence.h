#pragma once 

#include <cstdint>

namespace FISIR {

	class RHIFence {
	public:
		enum class Statue {
			Pendding,
			Signaled,
			UnSignaled,
		};

		virtual ~RHIFence() {};

		virtual void* getFenceHandle() const = 0;

		virtual Statue getFenceStage() const = 0;

		virtual void wait() = 0;

		virtual bool waitFor(uint64_t timeout = UINT64_MAX) = 0;

		virtual void reset() = 0;
	};

}