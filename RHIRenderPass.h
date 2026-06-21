#pragma once
#include "ExclusiveDepthStencil.h"
#include "RHITypes.h"
#include "RHITexture.h"
#include <vector>
#include "../DataBase/HashCreate.h"
#include "../Log/Logger.h"
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





	struct ColorEntry {
		struct {
			RenderTargetLoadAction	loadOp : 2 {RenderTargetLoadAction::None};
			RenderTargetStoreAction storeOp : 2  {RenderTargetStoreAction::None};
			TextureLayout			initLayout : 4 {TextureLayout::Undefined};
			TextureLayout			dstLayout : 4 {TextureLayout::Undefined};
			TextureCOLORType		colorType : 4 { TextureCOLORType::RGB_8 };
			uint32_t				sampleCount : 2 { 1 };
			bool					hasResolveTarget : 1 { false };
			TextureCOLORType		resolveColorType : 3{ TextureUseForNone };
			uint32_t				resolveSampleCount : 2{ 1 };
			bool					exeit: 1 {false};
			uint32_t                _padding : 7;
		} EntryPros;
		uint32_t value;

		bool operator == (const ColorEntry& other) const {
			return value == other.value;
		}
	};

	struct ColorEntryInputInfo {
		uint8_t InputPosition {0};
		ColorEntry Entry;
	};

	struct DepthStencilEntry {

		TextureCOLORType						colorType{ TextureCOLORType::Depth24_Stencil8 };
		uint32_t								sampleCount { 1 };
		ExclusiveDepthStencil					depthAction {};
		bool									hasResolveTarget{ false };
		TextureCOLORType						resolveColorType{ TextureCOLORType::Depth24_Stencil8 };
		uint32_t								resolveSampleCount { 1 };
		TextureLayout							initLayout { TextureLayout::Undefined };
		TextureLayout							dstLayout { TextureLayout::Undefined };
		bool									exeit{0};

		bool operator == (const DepthStencilEntry& other) const {
			return colorType == other.colorType &&
				sampleCount == other.sampleCount &&
				depthAction == other.depthAction &&
				hasResolveTarget == other.hasResolveTarget &&
				resolveColorType == other.resolveColorType &&
				resolveSampleCount == other.resolveSampleCount &&
				initLayout == other.initLayout &&
				dstLayout == other.dstLayout;
		}
	};


	struct SubPassInfo {
		uint64_t ColorEntryMask;
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

		RHIRenderPassInfo(const std::vector<ColorEntryInputInfo>& targets, DepthStencilEntry depthTargets, const std::vector<SubPassInfo>& subPasses) {
			for (auto [pos, entry] : targets) if (!ColorEntries[pos].EntryPros.exeit) {
				ColorEntries[pos] = entry;
				ColorEntries[pos].EntryPros.exeit = true;
			}
			else {
				Warn("This Position: {} Had Been Inputed", pos);
			}
			DepthStencilEntry = depthTargets;
			SubPasses = subPasses;
			GetHash();
		}

		ColorEntry ColorEntries[64];
		DepthStencilEntry DepthStencilEntry;

		std::vector<SubPassInfo> SubPasses;
		mutable uint32_t HashVal {0}; 

		bool operator == (const RHIRenderPassInfo& other) const {
			if (ColorEntries != other.ColorEntries) return false;
			
			if (!(DepthStencilEntry == other.DepthStencilEntry &&
				SubPasses == other.SubPasses)) {
				return false;
			}
			return true;
		}

		bool operator != (const RHIRenderPassInfo& other) const {
			return !(*this == other);
		}

		uint32_t GetHash() const {
			if (HashVal == 0) {
				for (auto &target : ColorEntries) {
					HashVal = HashCombine(HashVal, target.value);
				}

				HashVal = HashCombine(HashVal, (int)DepthStencilEntry.colorType);
				HashVal = HashCombine(HashVal, DepthStencilEntry.sampleCount);
				HashVal = HashCombine(HashVal, DepthStencilEntry.depthAction.GetHash());
				HashVal = HashCombine(HashVal, DepthStencilEntry.hasResolveTarget);
				HashVal = HashCombine(HashVal, (int)DepthStencilEntry.resolveColorType);
				HashVal = HashCombine(HashVal, DepthStencilEntry.resolveSampleCount);
				HashVal = HashCombine(HashVal, (uint32_t)DepthStencilEntry.initLayout);
				HashVal = HashCombine(HashVal, (uint32_t)DepthStencilEntry.dstLayout);


				for (auto& subpass : SubPasses) {
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

		virtual TextureLayout getAttachmentFinalLayout(uint32_t index) const = 0;
	
		virtual uint32_t getAttachmentCount() const = 0;
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