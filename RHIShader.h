#pragma once

#include "RHIResource.h"


#include "RHITypes.h"

namespace FISIR {

	
	using shader_t = void*;

	class RHIShader : public RHIResource {
	public:
		ShaderTYP getShaderType() const {return typ;}
		
	protected:
		ShaderTYP typ;
	};

	

}