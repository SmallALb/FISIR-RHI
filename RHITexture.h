#pragma once

#include "RHIResource.h"

namespace FISIR {
	using Texture_t = void*;

	enum class TextureType {
		TEXTURE1D,
		TEXTURE2D,
		TEXTURE3D,
		TEXTUREARRAY = TEXTURE3D,
	};
	
	enum class TextureCOLORType {
		RGB_8,
		RGB_16,
		RGB_32,

		RGBA_8,
		RGBA_16,
		RGBA_32,

		R_8,

		Depth24_Stencil8,
	};

	enum TextureUseFor {
		TextureUseForColorAttachment = 1,
		TextureUseForDepthStencilAttachment = 1<<1,
		TextureUseForShaderReadOnly = 1<<2,
		TextureUseForTransferSrc = 1<<3,
		TextureUseForTransferDst = 1<<4,
		TextureUseForStorage = 1<<5,
		TextureUseForDefault = TextureUseForColorAttachment | TextureUseForDepthStencilAttachment | TextureUseForShaderReadOnly,
		TextureUseForAll = TextureUseForColorAttachment | TextureUseForDepthStencilAttachment | TextureUseForShaderReadOnly | TextureUseForTransferSrc | TextureUseForTransferDst | TextureUseForStorage,
	};

	using TextureUseForFlags = uint32_t;


	inline size_t getTextureColorTypeSize(TextureCOLORType type) {
		switch (type) {
		case TextureCOLORType::RGB_8:
			return 3;
		case TextureCOLORType::RGB_16:
			return 6;
		case TextureCOLORType::RGB_32:
			return 12;
		case TextureCOLORType::RGBA_8:
			return 4;
		case TextureCOLORType::RGBA_16:
			return 8;
		case TextureCOLORType::RGBA_32:
			return 16;
		case TextureCOLORType::R_8:
			return 1;
		case TextureCOLORType::Depth24_Stencil8:
			return 4;
		default:
			return 0;
		}
	}

	struct ImageData {
		int x;
		int y;
		int width;
		int height;
		unsigned char* data;
		TextureCOLORType colorType;
	};

	struct TextureInfo {
		uint32_t			height;
		uint32_t			width;
		TextureCOLORType	colorType;
		TextureType			type;
		TextureUseForFlags  useFor{ TextureUseForStorage | TextureUseForShaderReadOnly };
		uint16_t			mipLevels{ 1 };
		uint16_t 			arrayLayers{ 1 };
		uint32_t			sampleCount{ 1 };
	};

	class RHITexture : public RHIResource {
	public:
		virtual ~RHITexture() {}

		virtual uint32_t getWidth() const = 0;

		virtual uint32_t getHeight() const = 0;

		virtual TextureLayout getCurrentLayout() const = 0;

		virtual void transitionLayout(TextureLayout newLayout) = 0;

		virtual TextureCOLORType getColorType() const = 0;

		virtual uint32_t getSampleCount() const = 0;

		virtual TextureUseForFlags getTextureUseFor() const = 0;
	};

	struct TextureTransitionInfo {
		RHITexture* texture;
		ResourceAccess newAccess;
		TextureLayout newLayout;
	};

}