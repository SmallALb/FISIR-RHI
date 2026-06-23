#pragma once

#include "../Log/Logger.h"

#ifdef _WIN32
#include <windows.h>
#elif _LINUX
#include <dlfcn.h>
#endif
#include <dxcapi.h>

#include "RHICreator.h"

#include <mutex>
#include "DynamicLibLoader.h"

namespace FISIR {


    inline std::atomic_bool dxcLoaded = false, needRelease = false;
    inline std::atomic_int32_t ComplierCount = 0;
    inline DLibHandle DxilHandle = nullptr;
    inline DLibHandle DHandle = nullptr;
    inline std::mutex initMutex;
    inline DxcCreateInstance2Proc DxcCreateInstance2 = nullptr;


    struct ShaderComplierData {
        IDxcLibrary* dxcLibrary = nullptr;
        IDxcCompiler* dxcCompiler = nullptr;
    };

    class ShaderComplier {
    public:
        static inline bool InitCompiler() {
            if (!dxcLoaded) {
                std::lock_guard<std::mutex> lock(initMutex);
                if (!dxcLoaded) {
                    DxilHandle = DynamicLibLoader::loadDLib("dxil.dll");
                    if (!DxilHandle) { Error("Failed to load dxil.dll"); return false; }
                    DHandle = DynamicLibLoader::loadDLib("dxcompiler.dll");
                    if (DHandle) {
                        DynamicLibLoader::getFunction(DHandle, "DxcCreateInstance2", &DxcCreateInstance2);
                        if (DxcCreateInstance2) dxcLoaded = true;
                        else Error("Failed to get DxcCreateInstance2");
                    }
                    else Error("Failed to load dxcompiler.dll");
                }
            }
            return dxcLoaded;
        }

        static inline void DestroyCompiler() {
            needRelease = true;
            if (ComplierCount) return;
            DynamicLibLoader::freeDLib(DHandle);
            DynamicLibLoader::freeDLib(DxilHandle);
        }

        ShaderComplier() {
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

        ~ShaderComplier() {
            ComplierCount--;
            if (mData->dxcLibrary) mData->dxcLibrary->Release();
            if (mData->dxcCompiler) mData->dxcCompiler->Release();
            delete mData;
            if (needRelease) DestroyCompiler();
            clear();
        }

        void clear() { delete[] shaderData; shaderData = nullptr; CodeSize = 0; }

        void compileShader(const wchar_t* data, size_t dataSize, const wchar_t* entryPoint, const wchar_t* target) {
            if (shaderData) {
                Error("Shader data already exists, clear before compiling new shader");
                return;
            }

            auto RenderAPI = RHICreator::getCrrentRenderInterfaceApi();
            std::vector<LPCWSTR> args{
                L"-E", entryPoint,
                L"-T", target, L"-spirv",
            };
            if (RenderAPI == FISIR::RHIAPI::Vulkan) args.push_back(L"-fspv-debug=vulkan-with-source");


            IDxcBlobEncoding* pShaderBlob = nullptr;
            mData->dxcLibrary->CreateBlobWithEncodingOnHeapCopy(
                data, dataSize, DXC_CP_UTF16, &pShaderBlob
            );

            IDxcOperationResult* result = nullptr;

            mData->dxcCompiler->Compile(
                pShaderBlob, nullptr, entryPoint, target,
                args.data(), uint32_t(args.size()), nullptr, 0, nullptr, &result
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

        unsigned char* getShaderData() const { return shaderData; }
        size_t getShaderDataSize() const { return CodeSize; }

    private:
        unsigned char* shaderData = nullptr;
        size_t CodeSize = 0;
        ShaderComplierData* mData = nullptr;
    };




}