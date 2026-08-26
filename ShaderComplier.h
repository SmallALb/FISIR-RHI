#pragma once

#include <mutex>
#include <string>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#elif _LINUX
#include <dlfcn.h>
#endif

#include <dxcapi.h>

// MinGW can't resolve __uuidof for DXC from MSVC-format dxcompiler.lib.
// Define the required IIDs inline.
#ifdef __MINGW32__
inline const GUID _IID_DxcLibrary = { 0xe5204dc7, 0xd18c, 0x4c3c, {0xbd, 0xfb, 0x85, 0x16, 0x73, 0x98, 0x0f, 0xe7} };
inline const GUID _IID_DxcCompiler = { 0x8c210bf3, 0x011f, 0x4422, {0x8d, 0x70, 0x6f, 0x9a, 0xcb, 0x8d, 0xb6, 0x17} };
#define DXC_IID_LIBRARY  _IID_DxcLibrary
#define DXC_IID_COMPILER _IID_DxcCompiler
#else
#define DXC_IID_LIBRARY  __uuidof(IDxcLibrary)
#define DXC_IID_COMPILER __uuidof(IDxcCompiler)
#endif

#include "DynamicLibLoader.h"
#include "Log/Logger.h"
#include "RHICreator.h"

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

    // 将 UTF-8 字节串转换为 UTF-16 宽字符串。
    // 磁盘上的 .hlsl 文件通常以 UTF-8 编码，而 DXC 的编译入口需要 UTF-16 源。
    inline std::wstring UTF8ToWide(const char* data, size_t size) {
#ifdef _WIN32
        if (size == 0) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, data, (int)size, nullptr, 0);
        std::wstring result(len, L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, data, (int)size, result.data(), len);
        return result;
#else
        std::wstring result;
        result.reserve(size);
        for (size_t i = 0; i < size; ++i) result.push_back((wchar_t)(unsigned char)data[i]);
        return result;
#endif
    }

    class ShaderComplier {
    public:
        static inline bool InitCompiler() {
            if (!dxcLoaded) {
                std::lock_guard<std::mutex> lock(initMutex);
                if (!dxcLoaded) {
                    DxilHandle = DynamicLibLoader::loadDLib("dxil.dll");
                    // dxil.dll merged into dxcompiler.dll in newer DXC; optional
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
            HRESULT hr = DxcCreateInstance2(nullptr, CLSID_DxcLibrary, DXC_IID_LIBRARY, (void**)&(mData->dxcLibrary));
            if (FAILED(hr) || !(mData->dxcLibrary)) {
                Error("Failed to create DxcLibrary instance, Error: 0x{:x}", (size_t)hr);
                return;
            }
            else Debug("DxcLibrary instance created successfully");

            hr = DxcCreateInstance2(nullptr, CLSID_DxcCompiler, DXC_IID_COMPILER, (void**)&(mData->dxcCompiler));
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
                L"-Vd"
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

        // 便捷重载：接受 UTF-8 源（例如从 .hlsl 文件读取的内容），内部转换为 UTF-16 后编译。
        void compileShader(const char* data, size_t dataSize, const char* entryPoint, const char* target) {
            const std::wstring wSource = UTF8ToWide(data, dataSize);
            const std::wstring wEntry  = UTF8ToWide(entryPoint, strlen(entryPoint));
            const std::wstring wTarget = UTF8ToWide(target, strlen(target));
            compileShader(wSource.data(), wSource.size() * sizeof(wchar_t), wEntry.c_str(), wTarget.c_str());
        }

        unsigned char* getShaderData() const { return shaderData; }
        size_t getShaderDataSize() const { return CodeSize; }

    private:
        unsigned char* shaderData = nullptr;
        size_t CodeSize = 0;
        ShaderComplierData* mData = nullptr;
    };




}