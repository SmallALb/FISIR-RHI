#pragma once

#include  "RHIContext.h"
#include "RHIResource.h"
#include "RHITexture.h"
#include "RHIBuffer.h"
#include "../Log/Logger.h"

namespace FISIR {
	class RHICommandListBase;



	struct RHICommand {
		virtual ~RHICommand() {}
		RHICommand* nxt{nullptr};
		virtual void Execute(RHICommandListBase& cmdList) = 0;
	};

	template<typename RHICmdListT, typename FUNC>
	struct RHIFunctionCommand final: public RHICommand {
		
		RHIFunctionCommand(FUNC&& func) : mfunction(std::forward <FUNC>(func)) {}

		virtual void Execute(RHICommandListBase& cmdList) override {
			mfunction(*static_cast<RHICommandListBase*>(&cmdList));
		}
		                                                              
		FUNC mfunction;
	};

	class RHICommandListBase {
	public:
		RHICommandListBase();

		void* AllocaCommand(int AllocaSize, int Alignemnt);

		template <typename FUNC>
		void PushFunc(FUNC&& func) {
			Info("Command Buffer Size is {}", sizeof(RHIFunctionCommand<RHICommandListBase, FUNC>));
			auto cmd = new 
				(AllocaCommand(sizeof(RHIFunctionCommand<RHICommandListBase, FUNC>), alignof(RHIFunctionCommand<RHICommandListBase, FUNC>))) 
			RHIFunctionCommand<RHICommandListBase, FUNC>(std::forward<FUNC>(func));
		}

		RHIContext* getContext() {return Context; }

		virtual CmdType getCommandListType() const = 0;

		virtual void executeSubCommands();

		RHICommand* Root{nullptr};
		RHICommand** CommandLink;
		RHIContext* Context;
	};

	class RHIComputeCommandList : public RHICommandListBase {
	public:
		RHIComputeCommandList();
		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ);
		void setPipelineState(RHIPipeline* pipeline);
		CmdType getCommandListType() const { return CmdType::Compute; }
	};

	class RHITransferCommandList : public RHICommandListBase {
	public:
		RHITransferCommandList();
		void TransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions);

		void TransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions);
		
		void CopyBuffer(RHIBuffer* dst, RHIBuffer* src, uint64_t dstOffset, uint64_t srcOffset, uint64_t size);
		
		void CopyTexture(RHITexture* dst, RHITexture* src);


		CmdType getCommandListType() const { return CmdType::Transfer; }
	};

	class RHIRenderCommandList : public RHICommandListBase {
	public:
		RHIRenderCommandList();
		void BeginRenderPass(RHIRenderPass* pass, const char* name);
		
		void EndRenderPass();
		
		void DrawPrimitive(uint32_t BaseVertexIndex, uint32_t NumsPrimitives, uint32_t NumInstances);
		
		void setPipelineState(RHIPipeline* pipeline);

		void BindResourcePack(RHIResourcePack* pack);

		CmdType getCommandListType() const {return CmdType::Render;}
	};

	class RHICommandListImmediate : public RHICommandListBase {
	public:

	};
	
	//CommandListExecutor  执行位置所在应该是RHI线程还是？
	//
	class CommandListExecutor {
	public:
		CommandListExecutor();

		void ExecuteList(RHICommandListBase& CmdList);
		void ExecuteList(RHICommandListImmediate& CmdList);
	};
}