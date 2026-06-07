#include "RHIShader.h"
#include "../Base/DynamicLibLoader.h"
#ifdef _WIN32
	#include <windows.h>
#elif _LINUX
	#include <dlfcn.h>
#endif
#include <dxcapi.h>
#include "../Log/Logger.h"
#include "../Base/DynamicLibLoader.h"
#include "DynamicRHI.h"
#include <mutex>
namespace FISIR {

	static std::atomic_bool dxcLoaded = false, needRelease = false;
	static std::atomic_int32_t ComplierCount = 0;
	static DLibHandle DxilHandle = nullptr;
	static DLibHandle DHandle = nullptr;
	static std::mutex initMutex;
	static DxcCreateInstance2Proc  DxcCreateInstance2 = nullptr;
	//DXINSTANCE_CREATE DxcCreateInstance = nullptr;


	struct ShaderComplierData {
		IDxcLibrary* dxcLibrary = nullptr;
		IDxcCompiler* dxcCompiler = nullptr;
	};

	bool ShaderComplier::InitCompiler() {
		if (!dxcLoaded) {
			std::lock_guard<std::mutex> lock(initMutex);
			if (!dxcLoaded) {
				DxilHandle = DynamicLibLoader::loadDLib("dxil.dll");
				if (!DxilHandle) {
					Error("Failed to load dxil.dll");
					return false;
				}
				DHandle = DynamicLibLoader::loadDLib("dxcompiler.dll");
				if (DHandle) {
					DynamicLibLoader::getFunction(DHandle, "DxcCreateInstance2", &DxcCreateInstance2);
					if (DxcCreateInstance2) {
						dxcLoaded = true;
					}
					else Error("Failed to get DxcCreateInstance2 function from dxcompiler.dll");
				}
				else Error("Failed to load dxcompiler.dll");
			}
		}
		return dxcLoaded;
	}

	void ShaderComplier::DestroyCompiler() {
		needRelease = true;
		if (ComplierCount) return;
		DynamicLibLoader::freeDLib(DHandle);
		DynamicLibLoader::freeDLib(DxilHandle);
	}

	ShaderComplier::ShaderComplier() {
		ComplierCount++;
		Info("Create ShaderComplier: {}", ComplierCount.load());
		
		InitCompiler();
		mData = new ShaderComplierData();
		HRESULT hr = DxcCreateInstance2(nullptr, CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void**)&(mData->dxcLibrary));
		if (FAILED(hr) || !(mData->dxcLibrary)) {
			Error("Failed to create DxcLibrary instance, Error: 0x{:x}", (size_t)hr);
			return;
		}
		else Debug("DxcLibrary instance created successfully");

		hr = DxcCreateInstance2(nullptr, CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void**)&(mData->dxcCompiler));
		if (FAILED(hr) || !(mData->dxcCompiler)) {
			Error("Failed to create DxcCompiler instance, Error: 0x{:x}", (size_t)hr);
			if (mData->dxcLibrary) {
				mData->dxcLibrary->Release();
				mData->dxcLibrary = nullptr;
			}
			return;
		}
		else Debug("DxcCompiler instance created successfully");


	}
	ShaderComplier::~ShaderComplier() {
		ComplierCount--;
		if (mData->dxcLibrary) {
			mData->dxcLibrary->Release();
		}
		if (mData->dxcCompiler) {
			mData->dxcCompiler->Release();
		}
		mData->dxcLibrary = nullptr;
		mData->dxcCompiler = nullptr;
		delete mData;
		if (needRelease) DestroyCompiler();
		clear();
	}

	void ShaderComplier::clear() {
		delete shaderData;
		shaderData = nullptr;
	}


	void ShaderComplier::compileShader(const wchar_t* data, size_t dataSize, const wchar_t* entryPoint, const wchar_t* target) {
		if (shaderData) {
			Error("Shader data already exists, clear before compiling new shader");
			return;
		}
		std::vector<wchar_t> wideData(dataSize / sizeof(wchar_t));

		auto RenderAPI = getCurrentRHIAPI();
		LPCWSTR args[] {
			L"-E", entryPoint,
			L"-T", target, L"-spirv"
		};

		IDxcBlobEncoding* pShaderBlob = nullptr;
		mData->dxcLibrary->CreateBlobWithEncodingOnHeapCopy(
			data, dataSize, DXC_CP_UTF16, &pShaderBlob
		);

		IDxcOperationResult* result = nullptr;

		mData->dxcCompiler->Compile(
			pShaderBlob, nullptr, entryPoint, target,
			args, _countof(args), nullptr, 0, nullptr, &result
		);

		HRESULT hr; result->GetStatus(&hr);
		if (FAILED(hr)) {
			IDxcBlobEncoding* pErrorBlob = nullptr;
			result->GetErrorBuffer(&pErrorBlob);
			std::string errorMsg((const char*)pErrorBlob->GetBufferPointer(), pErrorBlob->GetBufferSize());
			Error("Shader compilation failed: {}", errorMsg.c_str());
			pErrorBlob->Release();
		}
		else {
			IDxcBlob* pCompiledShader = nullptr;
			result->GetResult(&pCompiledShader);
			CodeSize = pCompiledShader->GetBufferSize();
			shaderData = new unsigned char[pCompiledShader->GetBufferSize()];
			memcpy(shaderData, pCompiledShader->GetBufferPointer(), pCompiledShader->GetBufferSize());
			pCompiledShader->Release();
			Debug("Shader compiled successfully");
		}
	}
}
