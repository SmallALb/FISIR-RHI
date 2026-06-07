#include "DynamicRHI.h"
#include <cstdio>
#include <string>
#include "../Log/Logger.h"
#include "../Base/DynamicLibLoader.h"

namespace FISIR {
	using FISIR_RHICREATE = DynamicRHI * (*)();
	using FISIR_RHIDESTROY = void (*)(DynamicRHI*);
  
	DLibHandle hDll = nullptr;

	FISIR_RHICREATE RHICreate_PTR = nullptr;
	FISIR_RHIDESTROY RHIDestroy_PTR = nullptr;
	static RHIAPI currentApi;

	DynamicRHI* rhi = nullptr;


	static std::string choicePath(RHIAPI api) {
		std::string res = "../../RHI/";
		switch (api) {
			case RHIAPI::Vulkan: return 
			#if defined(_DEBUG) && defined(_WIN32)
				res += "Vulkan/x64/Debug/RHIVK.dll";
			#elif _WIN32 
				res += "Vulkan/x64/Release/RHIVK.dll";
			#elif defined(_DEBUG)
			#endif // DEBUG
		}
		return res;
	}

	void setRHIAPI(RHIAPI api) {
		currentApi = api;
		auto path = choicePath(currentApi);
		hDll = DynamicLibLoader::loadDLib(path.c_str());
		if (hDll == nullptr)  {
			Error("Error RHI API include {}!\n", path.c_str());
			return;
		}
		DynamicLibLoader::getFunction(hDll, "RHICreate", &RHICreate_PTR);
		DynamicLibLoader::getFunction(hDll, "RHIDestroy", &RHIDestroy_PTR);
		if (RHICreate_PTR == NULL) Error("RHICREATE FUNC NOT FOUND!\n");
		if (RHIDestroy_PTR == NULL) Error("RHIDESTROY FUNC NOT FOUND!\n");
	}

	RHIAPI getCurrentRHIAPI() {
		return currentApi;
	}

	void closeRHIAPI() {
		if (hDll) DynamicLibLoader::freeDLib(hDll);
	}

	DynamicRHI* RHIGet() {
		if (!rhi) rhi = RHICreate_PTR();
		return rhi;
	}

	void RHIDestroy() {
		RHIDestroy_PTR(rhi);
	}

}