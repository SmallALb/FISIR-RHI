#include "RHICommandList.h"
#include "RHIRenderPass.h"
#include "RHIContext.h"
#include "RHIPipeline.h"
#include "DynamicRHI.h"
#include <cstdint>
#include <cstdlib>

namespace FISIR {
	RHICommandListBase::RHICommandListBase() {
		CommandLink = &Root;
	}

	void* RHICommandListBase::AllocaCommand(int AllocaSize, int Alignemnt) {
		void* ptr = malloc(AllocaSize);	
		RHICommand* res = (RHICommand*) ptr;
		*CommandLink = res;
		CommandLink = &res->nxt;
		return res;
	}


	void RHICommandListBase::executeSubCommands() {
		PushFunc([this](RHICommandListBase& cmdList) {
			cmdList.getContext()->RHIExecuteSubCommand();
		});
	}

	RHIRenderCommandList::RHIRenderCommandList() {
		Context = RHIGet()->RHIGetContext(CmdType::Render);
	}

	void RHIRenderCommandList::BeginRenderPass(RHIRenderPass* pass, const char* name) {
		auto ctx = static_cast<RHIRenderContext*>(getContext());
		PushFunc([ctx, &pass, name](RHICommandListBase&) {
			ctx->RHIBeginRenderPass(pass);
		});
	}

	void RHIRenderCommandList::EndRenderPass() {
		auto ctx = static_cast<RHIRenderContext*>(getContext());
		PushFunc([ctx](RHICommandListBase&) {
			ctx->RHIEndRenderPass();
		});
	}

	void RHIRenderCommandList::DrawPrimitive(uint32_t BaseVertexIndex, uint32_t NumsPrimitives, uint32_t NumInstances) {
		auto ctx = static_cast<RHIRenderContext*>(getContext());
		PushFunc([ctx, BaseVertexIndex, NumsPrimitives, NumInstances](RHICommandListBase&) {
			ctx->RHIDrawPrimitive(BaseVertexIndex, NumsPrimitives, NumInstances);
		});
	}

	void RHIRenderCommandList::setPipelineState(RHIPipeline* pipeline) {
		auto ctx = static_cast<RHIRenderContext*>(getContext());
		PushFunc([ctx, pipeline, this](RHICommandListBase&) {
			ctx->RHISetGraphicsPipelineState(pipeline);
		});
	}

	void RHIRenderCommandList::BindResourcePack(RHIResourcePack* pack) {
		auto ctx = static_cast<RHIRenderContext*>(getContext());
		PushFunc([ctx, pack](RHICommandListBase&) {
			ctx->RHIBindResourcePack(pack);
		});
	}

	RHIComputeCommandList::RHIComputeCommandList() {
		Context = RHIGet()->RHIGetContext(CmdType::Compute);
	}

	void RHIComputeCommandList::dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ) {


	}

	void RHIComputeCommandList::setPipelineState(RHIPipeline* pipeline) {

	}


	RHITransferCommandList::RHITransferCommandList() {
		Context = RHIGet()->RHIGetContext(CmdType::Transfer);
	}

	void RHITransferCommandList::TransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions) {
		auto ctx = static_cast<RHITransferContext*>(getContext());
		PushFunc([ctx, bufferTransitions](RHICommandListBase&) {
			ctx->RHITransitionBuffers(bufferTransitions);
		});
	}

	void RHITransferCommandList::TransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) {
		auto ctx = static_cast<RHITransferContext*>(getContext());
		PushFunc([ctx, textureTransitions](RHICommandListBase&) {
			ctx->RHITransitionTextures(textureTransitions);
		});
	
	}


	void RHITransferCommandList::CopyTexture(RHITexture* dst, RHITexture* src) {

	}


	CommandListExecutor::CommandListExecutor() {

	}

	void CommandListExecutor::ExecuteList(RHICommandListBase& CmdList) {
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

	void FISIR::CommandListExecutor::ExecuteList(RHICommandListImmediate& CmdList) {
		
	}
}