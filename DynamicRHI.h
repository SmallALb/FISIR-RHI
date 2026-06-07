#pragma once
#include "RHITypes.h"
#include <vector>
#include <cstdint>
#include "../Log/Logger.h"
#include "../Base/DynamicLibLoader.h"

enum class RHIAPI {
	OpenGL,
	Vulkan
};

namespace FISIR {	
	class RHITexture;
	class RHIBuffer;
	class RHIViewport;
	class RHIPipeline;
	class RHIContext;
	class RHIShader;
	class RHICommandListBase;
	class RHIResourcePack;
	class RHIRenderPass;
	class RHIResource;

	struct RHIPipelineState;
	struct RHIRenderPassInfo;

	inline DLibHandle hDll = nullptr;
	inline RHIAPI currentApi;

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

	class DynamicRHI {
	public:
		virtual ~DynamicRHI() {}
		virtual bool Init() = 0;

		virtual RHITexture* RHICreateTexture(const TextureInfo& textureInfo) = 0;

		virtual RHIBuffer* RHICreateBuffer(const BufferInfo& bufferInfo) = 0;

		virtual RHIViewport* RHICreateViewport() = 0;

		virtual RHIPipeline* RHICreatePipeline(const RHIPipelineState& PipelineState) = 0;

		virtual RHIContext* RHIGetContext(CmdType type) = 0;

		virtual RHIShader* RHICreateShader(ShaderTYP typ, const unsigned char* Data, size_t size) = 0;

		virtual void RHISubmitCommandList(RHICommandListBase* cmdList) = 0;

		virtual RHIResourcePack* RHICreateResourcePack(Type restyp, std::initializer_list<RHIResource*> resources) = 0;

		virtual RHIResourcePack* RHICreateResourcePack(Type restyp, const std::vector<RHIResource*>& resources) = 0;

		virtual RHIRenderPass* RHICreateRenderPass(const RHIRenderPassInfo& info) = 0;
	};

	inline DynamicRHI* rhi = nullptr;
	using FISIR_RHICREATE = DynamicRHI * (*)();
	using FISIR_RHIDESTROY = void (*)(DynamicRHI*);
	inline FISIR_RHICREATE RHICreate_PTR = nullptr;
	inline FISIR_RHIDESTROY RHIDestroy_PTR = nullptr;


	inline void setRHIAPI(RHIAPI api) {
		currentApi = api;
		auto path = choicePath(currentApi);
		hDll = DynamicLibLoader::loadDLib(path.c_str());
		if (!hDll) {
			Error("Error RHI API include {}!\n", path.c_str());
			return;
		}
		DynamicLibLoader::getFunction(hDll, "RHICreate", &RHICreate_PTR);
		DynamicLibLoader::getFunction(hDll, "RHIDestroy", &RHIDestroy_PTR);
		if (!RHICreate_PTR) Error("RHICREATE FUNC NOT FOUND!\n");
		if (!RHIDestroy_PTR) Error("RHIDESTROY FUNC NOT FOUND!\n");
	}

	inline RHIAPI getCurrentRHIAPI() { return currentApi; }

	inline void closeRHIAPI() {
		if (hDll) DynamicLibLoader::freeDLib(hDll);
	}

	inline DynamicRHI* RHIGet() {
		if (!rhi) rhi = RHICreate_PTR();
		return rhi;
	}

	inline void RHIDestroy() {
		RHIDestroy_PTR(rhi);
	}
}