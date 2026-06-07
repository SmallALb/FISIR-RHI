#pragma once

#include "RHIResource.h"
namespace FISIR {
	enum ShaderTYP {
		__VERTEXSHADER__,
		__FRAGMENTSHADER__,
		__TESSSHADER__,
		__GEOMETRY__,
		__COMPUTESHADER__,
		ShaderTYPCOUNT
	};
	

	using shader_t = void*;

	struct ShaderComplierData;


	class ShaderComplier {
	public:
		static bool InitCompiler();

		static void DestroyCompiler();
		ShaderComplier();

		~ShaderComplier();

		void clear();

		void compileShader(const wchar_t* data, size_t dataSize, const wchar_t* entryPoint, const wchar_t* target);

		unsigned char* getShaderData() const { return shaderData; }

		size_t getShaderDataSize() const { return CodeSize; }
	private:
		unsigned char* shaderData{nullptr};
		size_t CodeSize{ 0 };
		ShaderComplierData* mData {nullptr};
	};
	

	class RHIShader : public RHIResource {
	public:
		ShaderTYP getShaderType() const {return typ;}
		
	protected:
		ShaderTYP typ;
	};

	

}