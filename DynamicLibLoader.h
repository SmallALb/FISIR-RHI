#pragma once

#include <cstring>      // for memcpy
#include "../Log/Logger.h"  // 如果 Debug/Error 宏依赖它

#ifdef _WIN32
#include <windows.h>
#elif __linux__
#include <dlfcn.h>
#endif

namespace FISIR {

    using DLibHandle = void*;
    using DLibFuncPtr = void*;

    class DynamicLibLoader {
    public:
        static inline DLibHandle loadDLib(const char* libPath) {
#ifdef _WIN32
            char dllPath[MAX_PATH];
            HMODULE hModule = GetModuleHandleA(libPath);
            if (hModule) {
                GetModuleFileNameA(hModule, dllPath, MAX_PATH);
                Debug("Current executable path: {}", dllPath);
            }
            return LoadLibraryA(libPath);
#elif __linux__
            return dlopen(libPath, RTLD_LAZY);
#else
            return nullptr;
#endif
        }

        static inline void freeDLib(DLibHandle handle) {
#ifdef _WIN32
            FreeLibrary((HMODULE)handle);
#elif __linux__
            dlclose(handle);
#endif
        }

        template<typename T>
        static inline void getFunction(DLibHandle handle, const char* funcName, T* Func) {
            void* rawFunc = getFunction_(handle, funcName);
            memcpy(Func, &rawFunc, sizeof(rawFunc));
        }

    private:
        static inline DLibFuncPtr getFunction_(DLibHandle handle, const char* funcName) {
#ifdef _WIN32
            return GetProcAddress((HMODULE)handle, funcName);
#elif __linux__
            return dlsym(handle, funcName);
#else
            return nullptr;
#endif
        }
    };

} // namespace FISIR