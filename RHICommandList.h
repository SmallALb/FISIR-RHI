#pragma once

#include "RHIContext.h"          
#include "RHIPipeline.h"         
#include "RHIResourcePack.h"     
#include "RHIRenderPass.h"       
#include "RHITexture.h"        
#include "RHISwapChain.h"
#include "RHIViewport.h"
#include "RHIBuffer.h"           
#include "DynamicRHI.h"          
#include "../Log/Logger.h"
#include <iostream>

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

	protected:
		virtual void Execute(RHICommandListBase& cmdList) override {
			mfunction(*static_cast<RHICommandListBase*>(&cmdList));
		}
		                                                              
		FUNC mfunction;
	};

	class RHICommandListBase {
	public:
		RHICommandListBase() { 
			Pool = (uint8_t*)malloc(POOL_SIZE);
			Pool_Ptr = Pool;
			Root = nullptr;
		}

		~RHICommandListBase() {
			RHICommand* cmd = Root;
			//Debug("Destructor this=0x{:x}, Pool=0x{:x}", (size_t)this, (size_t)Pool);
			free(Pool);
		}

		void* AllocaCommand(int AllocaSize, int Alignemnt) {
			size_t used = Pool_Ptr - Pool;
			uintptr_t alignedOffset = (used + Alignemnt - 1) & ~(Alignemnt - 1);
			if (alignedOffset + AllocaSize > POOL_SIZE) {
				Error("Cmd List Pool overflow");
				return nullptr;
			}
			void* ptr = (void*)(Pool + alignedOffset);
			Pool_Ptr = (Pool + alignedOffset) + AllocaSize;

			RHICommand* res = (RHICommand*)ptr;
			res->nxt = nullptr;
			
			(Tail ? Tail->nxt : Root) = res;

			Tail = res;
			return res;
			return res;
		}

		template <typename FUNC>
		void PushFunc(FUNC&& func) {
			//Info("Command Buffer Size is {}", sizeof(RHIFunctionCommand<RHICommandListBase, FUNC>));
			auto cmd = new 
				(AllocaCommand(sizeof(RHIFunctionCommand<RHICommandListBase, FUNC>), alignof(RHIFunctionCommand<RHICommandListBase, FUNC>))) 
			RHIFunctionCommand<RHICommandListBase, FUNC>(std::forward<FUNC>(func));
		}

		void ExectueList() {
			//Debug("ExecuteList START : this = 0x{:X}, Pool = 0x{:x}, Root = 0x{:x}", (size_t)this, (size_t)Pool, (size_t)Root);
			auto ctx = getContext();
			if (!ctx || DontExecuteAndSubmit || Executed) {
				//Error("Ctx is Null or List had Been executed");
				return;
			}
			RHICommand* cmd = Root;
			ctx->RHIBegin();
			while (cmd) {
				cmd->Execute(*this);
				auto nxt = cmd->nxt;
				cmd = nxt;
			}
			ctx->RHIEnd();
			//Debug("ExecuteList End : this = 0x{:X}, Pool = 0x{:x}", (size_t)this, (size_t)Pool);

			Pool_Ptr = Pool;
			Root = nullptr;
			Tail = nullptr;
			Executed = true;
		}

		RHIContext* getContext() {return Context; }

		virtual CmdType getCommandListType() const = 0;

		virtual void executeSubCommands() {
			PushFunc([this](RHICommandListBase& cmdList) {
			cmdList.getContext()->RHIExecuteSubCommand();
			});
		}

		void setContext(RHIContext* context) { Context  = context;}

		RHICommand* Root{nullptr};
		RHICommand* Tail{ nullptr };
		RHIContext* Context {nullptr};
		bool DontExecuteAndSubmit{0};
		bool Executed{0};
	private:
		static constexpr size_t POOL_SIZE = 1024 * 1024;
		uint8_t* Pool;
		uint8_t* Pool_Ptr;
	};

	class RHIComputeCommandList : public RHICommandListBase {
	public:
		RHIComputeCommandList() {}
		void dispatch(uint32_t GroupCountX, uint32_t GroupCountY, uint32_t GroupCountZ);
		void setPipelineState(RHIPipeline* pipeline);
		CmdType getCommandListType() const { return CmdType::Compute; }
	};

	class RHITransferCommandList : public RHICommandListBase {
	public:
		RHITransferCommandList() {}

		void TransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions) {
			PushFunc([this, bufferTransitions](RHICommandListBase&) {
				auto ctx = static_cast<RHITransferContext*>(getContext());
				ctx->RHITransitionBuffers(bufferTransitions);
			});
		}

		void TransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions) {
			PushFunc([this, textureTransitions](RHICommandListBase&) {
				auto ctx = static_cast<RHITransferContext*>(getContext());
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
		RHIRenderCommandList() {}

		CmdType getCommandListType() const {return CmdType::Render;}

		void BeginRenderPass(RHIFrameBuffer* frame, const ClearValue& value) {
			PushFunc([this, frame, &value](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHIBeginRenderPass(frame, value);
			});
		}

		void EndRenderPass() {
			PushFunc([this](RHICommandListBase&) {
				//Warn("RenderPass End Begin");
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHIEndRenderPass();
				//Warn("RenderPass End Finish");

			});
		}

		void SetViewPort(float x, float y, float width, float height, float maxDepth, float minDepth) {
			PushFunc([this, x, y, width, height, maxDepth, minDepth](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHISetViewport(x, y, width, height, maxDepth, minDepth);
			});
		}

		void SetScissor(uint32_t width, uint32_t height) {
			PushFunc([this, width, height](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHISetScissor(width, height);
			});	
		}

		void DrawPrimitive(uint32_t BaseVertexIndex, uint32_t NumsPrimitives, uint32_t NumInstances) {
			PushFunc([this, BaseVertexIndex, NumsPrimitives, NumInstances](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHIDrawPrimitive(BaseVertexIndex, NumsPrimitives, NumInstances);
			});
		}

		void setPipelineState(RHIPipeline* pipeline) {
			PushFunc([this, pipeline](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHISetGraphicsPipelineState(pipeline);
			});
		}

		void BindResourcePack(RHIResourcePack* Resourcepack, RHIResourcePack* Samplerpack) {
			PushFunc([this, Resourcepack, Samplerpack](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHIBindResourcePack(Resourcepack, Samplerpack);
			});
		}

		void BindVertexBuffer(RHIBuffer* buffer, uint32_t binding, uint64_t offset) {
			PushFunc([this, buffer, binding, offset](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHIBindVertexBuffer(buffer, binding, offset);
				});
		}

		void TransitionBuffers(std::initializer_list<BufferTransitionInfo> bufferTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
			PushFunc([this, bufferTransitions, waitForStageDone, beginStageWhenDone](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHITransitionBuffers(bufferTransitions, waitForStageDone, beginStageWhenDone);
			});
		}

		void TransitionTextures(std::initializer_list<TextureTransitionInfo> textureTransitions, RHIUsingStage waitForStageDone, RHIUsingStage beginStageWhenDone) {
			PushFunc([this, textureTransitions, waitForStageDone, beginStageWhenDone](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHITransitionTextures(textureTransitions, waitForStageDone, beginStageWhenDone);
			});
		}

		void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
			PushFunc([this, dst, src, dstOffset, srcOffset, size](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHICopyBuffer(dst, src, size, srcOffset, dstOffset);
			});
		}

		void CopyTexture(RHIBuffer* src, RHITexture* dst, uint32_t miplevel, uint32_t arrayindex, uint32_t arraycount, uint64_t srcoffset, TextureSize dstOffset, TextureSize size) {
			PushFunc([this, dst, src, dstOffset, srcoffset, size, miplevel, arrayindex, arraycount](RHICommandListBase&) {
				auto ctx = static_cast<RHIRenderContext*>(getContext());
				ctx->RHICopyTexture(src, dst, size, miplevel, arrayindex, arraycount, srcoffset, dstOffset);
			});
		}


	};

	class RHICommandListImmediate : public RHICommandListBase {
	public:

	};
	
}