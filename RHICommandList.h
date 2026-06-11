#pragma once

#include "RHIContext.h"          
#include "RHIPipeline.h"         
#include "RHIResourcePack.h"     
#include "RHIRenderPass.h"       
#include "RHITexture.h"          
#include "RHIBuffer.h"           
#include "DynamicRHI.h"          
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
		RHICommandListBase() { CommandLink = &Root; }

		void* AllocaCommand(int AllocaSize, int Alignemnt) {
			void* ptr = malloc(AllocaSize);
			RHICommand* res = (RHICommand*)ptr;
			*CommandLink = res;
			CommandLink = &res->nxt;
			return res;
		}

		template <typename FUNC>
		void PushFunc(FUNC&& func) {
			Info("Command Buffer Size is {}", sizeof(RHIFunctionCommand<RHICommandListBase, FUNC>));
			auto cmd = new 
				(AllocaCommand(sizeof(RHIFunctionCommand<RHICommandListBase, FUNC>), alignof(RHIFunctionCommand<RHICommandListBase, FUNC>))) 
			RHIFunctionCommand<RHICommandListBase, FUNC>(std::forward<FUNC>(func));
		}

		RHIContext* getContext() {return Context; }

		virtual CmdType getCommandListType() const = 0;

	virtual void executeSubCommands() {
		PushFunc([this](RHICommandListBase& cmdList) {
			cmdList.getContext()->RHIExecuteSubCommand();
		});
	}

		RHICommand* Root{nullptr};
		RHICommand** CommandLink;
		RHIContext* Context;
	};

	class RHIComputeCommandList : public RHICommandListBase {
	public:
		RHIComputeCommandList(DynamicRHI* rhi) { Context = rhi->RHIGetContext(CmdType::Compute); }
		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ);
		void setPipelineState(RHIPipeline* pipeline);
		CmdType getCommandListType() const { return CmdType::Compute; }
	};

	class RHITransferCommandList : public RHICommandListBase {
	public:
		RHITransferCommandList(DynamicRHI* rhi) { Context = rhi->RHIGetContext(CmdType::Transfer); }

		void TransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions) {
			auto ctx = static_cast<RHITransferContext*>(getContext());
			PushFunc([ctx, bufferTransitions](RHICommandListBase&) {
				ctx->RHITransitionBuffers(bufferTransitions);
			});
		}

		void TransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) {
			auto ctx = static_cast<RHITransferContext*>(getContext());
			PushFunc([ctx, textureTransitions](RHICommandListBase&) {
				ctx->RHITransitionTextures(textureTransitions);
			});
		}

		void CopyBuffer(RHIBuffer* dst, RHIBuffer* src, uint64_t dstOffset, uint64_t srcOffset, uint64_t size) {
		
		}

		void CopyTexture(RHITexture* dst, RHITexture* src) {

		}

		CmdType getCommandListType() const { return CmdType::Transfer; }
	};

	class RHIRenderCommandList : public RHICommandListBase {
	public:
		RHIRenderCommandList(DynamicRHI* rhi) { Context = rhi->RHIGetContext(CmdType::Render); }

		CmdType getCommandListType() const {return CmdType::Render;}

		void BeginRenderPass(RHIRenderPass* pass, const char* name) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, &pass, name](RHICommandListBase&) {
				ctx->RHIBeginRenderPass(pass);
			});
		}

		void EndRenderPass() {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx](RHICommandListBase&) {
				ctx->RHIEndRenderPass();
			});
		}

		void DrawPrimitive(uint32_t BaseVertexIndex, uint32_t NumsPrimitives, uint32_t NumInstances) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, BaseVertexIndex, NumsPrimitives, NumInstances](RHICommandListBase&) {
				ctx->RHIDrawPrimitive(BaseVertexIndex, NumsPrimitives, NumInstances);
			});
		}

		void setPipelineState(RHIPipeline* pipeline) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, pipeline, this](RHICommandListBase&) {
				ctx->RHISetGraphicsPipelineState(pipeline);
			});
		}

		void BindResourcePack(RHIResourcePack* pack) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, pack](RHICommandListBase&) {
				ctx->RHIBindResourcePack(pack);
			});
		}

		void TransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, bufferTransitions, waitForStageDone, beginStageWhenDone](RHICommandListBase&) {
				ctx->RHITransitionBuffers(bufferTransitions, waitForStageDone, beginStageWhenDone);
			});
		}

		void TransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, textureTransitions, waitForStageDone, beginStageWhenDone](RHICommandListBase&) {
				ctx->RHITransitionTextures(textureTransitions, waitForStageDone, beginStageWhenDone);
			});
		}

		void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, dst, src, dstOffset, srcOffset, size](RHICommandListBase&) {
				ctx->RHICopyBuffer(dst, src, size, srcOffset, dstOffset);
			});
		}

		void CopyTexture(RHIBuffer* src, RHITexture* dst, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcoffset, TextureSize dstOffset, TextureSize size) {
			auto ctx = static_cast<RHIRenderContext*>(getContext());
			PushFunc([ctx, dst, src, dstOffset, srcoffset, size, miplevel, arrayindex, arraycount](RHICommandListBase&) {
				ctx->RHICopyTexture(src, dst, size, miplevel, arrayindex, arraycount, srcoffset, dstOffset);
			});
		}
	};

	class RHICommandListImmediate : public RHICommandListBase {
	public:

	};
	
	//CommandListExecutor  执行位置所在应该是RHI线程还是？
	//
	class CommandListExecutor {
	public:
		CommandListExecutor() {}

		void ExecuteList(RHICommandListBase& CmdList) {
			auto ctx = CmdList.getContext();
			RHICommand* cmd = CmdList.Root;
			ctx->RHIBegin();
			while (cmd) {
				cmd->Execute(CmdList);
				RHICommand* next = cmd->nxt;
				cmd->~RHICommand();
				free(cmd);
				cmd = next;
			}
			ctx->RHIEnd();
			CmdList.CommandLink = &CmdList.Root;
			CmdList.Root = nullptr;
		}

		void ExecuteList(RHICommandListImmediate& CmdList) {

		}
	};
}