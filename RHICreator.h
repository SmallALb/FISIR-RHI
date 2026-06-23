#pragma once

#include "DynamicLibLoader.h"
#include "DynamicRHI.h"
#include "../Log/Logger.h"


namespace FISIR{
	enum class RHIAPI {
		Dx12,
		Vulkan
	};

	class RHICreator {
		using FISIR_RHICREATE = DynamicRHI * (*)();
		using FISIR_RHIDESTROY = void (*)(DynamicRHI*);


		inline static std::string choicePath(RHIAPI api) {
			std::string res = "../../RHI/";
			switch (api) {
			case RHIAPI::Vulkan:
			#if defined(_DEBUG) && defined(_WIN32)
				return res + "Vulkan/x64/Debug/RHIVK.dll";
			#elif defined(_WIN32)
				return res + "Vulkan/x64/Release/RHIVK.dll";
			#endif
			default:
				return res;
			}
		}

	public:
		inline static bool setRenderInterfaceApi(RHIAPI api, const char* inPath = nullptr) {
			currentApi = api;
			auto path = inPath == nullptr ? choicePath(currentApi) : inPath;
			hDll = DynamicLibLoader::loadDLib(path.c_str());
			if (!hDll) {
				Error("Error RHI API include {}!\n", path.c_str());
				return DllLoaded = false;
			}
			DynamicLibLoader::getFunction(hDll, "RHICreate", &RHICreate_PTR);
			DynamicLibLoader::getFunction(hDll, "RHIDestroy", &RHIDestroy_PTR);
			if (!RHICreate_PTR) {
				Error("RHICREATE FUNC NOT FOUND!\n");
				return DllLoaded = false;
			}
			if (!RHIDestroy_PTR) {
				Error("RHIDESTROY FUNC NOT FOUND!\n");
				return DllLoaded = false;
			}	
			return DllLoaded = true;
		}

		inline static DynamicRHI* getCurrentRenderInterface() {
			if (Rhi) return Rhi;
			if (!DllLoaded) {
				Error("Failed To Load Current API");
				return nullptr;
			}
			Rhi = RHICreate_PTR();
			return Rhi;
		}

		inline static void destroyRenderInterface() {
			if (Rhi) RHIDestroy_PTR(Rhi);
			Rhi = nullptr;
		}

		inline static void freeCurrentRenderInterfaceApi() {
			if (Rhi) destroyRenderInterface();
			DynamicLibLoader::freeDLib(hDll);

		}

		inline static RHIAPI getCrrentRenderInterfaceApi() {
			return currentApi;
		}

	private:
		inline static DLibHandle hDll;
		inline static RHIAPI currentApi;
		inline static DynamicRHI* Rhi = nullptr;
		inline static FISIR_RHICREATE RHICreate_PTR = nullptr;
		inline static FISIR_RHIDESTROY RHIDestroy_PTR = nullptr;
		inline static bool DllLoaded{0};
	};

}
