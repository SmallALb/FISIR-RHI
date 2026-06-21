#pragma once 

#include "RHIResource.h"
#include <initializer_list>
namespace FISIR {

	class RHIResourcePack {
	
	public:

		virtual ~RHIResourcePack() {}

		virtual Type getResourceType() const = 0;
	
	};

}