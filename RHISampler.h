#pragma once 

#include "RHIResource.h"

namespace FISIR {

	class RHISampler : public RHIResource {
	public:
		virtual Type getResourceType() const { return Type::Sampler; }

		virtual SamplerInfo getSamplerInfo() const = 0;
	};

}