#pragma once

#include "ExclusiveDepthStencil.h"
#include "RHITypes.h"
#include "RHITexture.h"
#include <vector>
#include "../DataBase/HashCreate.h"

namespace FISIR {
	

	class RHITexture;

	using RenderPass_t = void*;

	struct FResolveRect {
		int X1, Y1, X2, Y2;

		FResolveRect(int x1 = -1, int y1 = -1, int x2 = -1, int y2 = -1) :
			X1(x1), Y1(y1), X2(x2), Y2(y2)
		{
		}

		bool operator==(const FResolveRect& other) const {
			return X1 == other.X1 && Y1 == other.Y1 && X2 == other.X2 && Y2 == other.Y2;
		}

		bool operator!=(const FResolveRect& other) const {
			return !(*this == other);
		}

		bool isvalid() const {
			return X1 >= 0 && Y1 >= 0 && X2 - X1 > 0 && Y2 - Y1 > 0;
		}
	};


	enum class RenderTargetLoadAction : unsigned char {
		None,
		Load,
		Clear,

		Count,
		CountBits = 2,
	};

	enum class RenderTargetStoreAction : unsigned char {
		None,
		Store,
		MultisampleResolve,

		Count,
		CountBits = 2,
	};


	struct ColorEntry {
		RHITexture* RenderTarget;
		RHITexture* ResolveTarget;
		RenderTargetLoadAction loadOp;
		RenderTargetStoreAction storeOp;
		int MipIndex;
		int ArraySlice;
		bool operator == (const ColorEntry& other) const {
			return RenderTarget == other.RenderTarget &&
				ResolveTarget == other.ResolveTarget &&
				loadOp == other.loadOp &&
				storeOp == other.storeOp &&
				MipIndex == other.MipIndex &&
				ArraySlice == other.ArraySlice;
		}

	};

	struct DepthStencilEntry {
		RHITexture* RenderTarget;
		RHITexture* ResolveTarget;
		ExclusiveDepthStencil DepthAction;

		bool operator == (const DepthStencilEntry& other) const {
			return RenderTarget == other.RenderTarget &&
				ResolveTarget == other.ResolveTarget &&
				DepthAction == other.DepthAction;
		}


	};


	struct SubPassInfo {
		uint32_t ColorEntryMask;
		bool UseDepthStencil;
		bool ReadDepthAsInput;
		RHIUsingStage DepthStencilReadStage { NoneStage };

		bool operator == (const SubPassInfo& other) const {
			return ColorEntryMask == other.ColorEntryMask &&
				UseDepthStencil == other.UseDepthStencil &&
				ReadDepthAsInput == other.ReadDepthAsInput &&
				DepthStencilReadStage == other.DepthStencilReadStage;
		}
	};

	struct RHIRenderPassInfo {
		RHIRenderPassInfo() {}

		RHIRenderPassInfo(const std::vector<ColorEntry>& targets, DepthStencilEntry depthTargets, const std::vector<SubPassInfo>& subPasses) : mSubPasses(subPasses), mDepthStencilEntry(depthTargets) {
			for (auto target : targets) {
				mColorEntries[target.RenderTarget->getTextureUseFor()] = target;
				ColorEntriesMask |= target.RenderTarget->getTextureUseFor();
			}
			mSubPasses = subPasses;
			GetHash();
		}

		ColorEntry mColorEntries[TextureUseForAll + 1] {nullptr};
		DepthStencilEntry mDepthStencilEntry;

		std::vector<SubPassInfo> mSubPasses;

		uint32_t ColorEntriesMask{ 0 };
		mutable uint32_t HashVal {0}; 

		bool operator == (const RHIRenderPassInfo& other) const {
			if (!(mDepthStencilEntry == other.mDepthStencilEntry &&
				mSubPasses == other.mSubPasses &&
				HashVal == other.HashVal)) {
				return false;
			}

			for (size_t i = 0; i < TextureUseForAll + 1; ++i) {
				if (mColorEntries[i] != other.mColorEntries[i]) {
					return false;
				}
			}

			return true;
		}

		bool operator != (const RHIRenderPassInfo& other) const {
			return !(*this == other);
		}

		uint32_t GetHash() const {
			if (HashVal == 0) {
				uint32_t mask = ColorEntriesMask;
				for (uint32_t i =0; mask != 0 && i<= TextureUseForAll; i++) if ((mask & (1u << i))) {
					mask &= ~(1u << i);

					HashVal = HashCombine(HashVal, HashPointer(mColorEntries[i].RenderTarget));
					HashVal = HashCombine(HashVal, HashPointer(mColorEntries[i].ResolveTarget));
					HashVal = HashCombine(HashVal, (uint32_t)mColorEntries[i].loadOp);
					HashVal = HashCombine(HashVal, (uint32_t)mColorEntries[i].storeOp);
					HashVal = HashCombine(HashVal, (uint32_t)mColorEntries[i].MipIndex);
					HashVal = HashCombine(HashVal, (uint32_t)mColorEntries[i].ArraySlice);

				}

				HashVal = HashCombine(HashVal, HashPointer(mDepthStencilEntry.RenderTarget));
				HashVal = HashCombine(HashVal, HashPointer(mDepthStencilEntry.ResolveTarget));
				HashVal = HashCombine(HashVal, mDepthStencilEntry.DepthAction.GetHash());

				for (auto& subpass : mSubPasses) {
					HashVal = HashCombine(HashVal, subpass.ColorEntryMask);
					HashVal = HashCombine(HashVal, subpass.UseDepthStencil ? 1u : 0u);
					HashVal = HashCombine(HashVal, subpass.ReadDepthAsInput ? 1u : 0u);
					HashVal = HashCombine(HashVal, (uint32_t)subpass.DepthStencilReadStage);
				}
			}
			return HashVal;
		}

		private:


	};


	class RHIRenderPass {
	public:
		virtual ~RHIRenderPass() {}

		virtual RenderPass_t getRenderPassHandle() = 0;
	
	};

}


namespace std {
	template<>
	struct hash<FISIR::RHIRenderPassInfo> {
		size_t operator()(const FISIR::RHIRenderPassInfo& info) const {
			return info.GetHash();
		}
	};
}