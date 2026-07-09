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

		virtual void reset() = 0;
	};

}