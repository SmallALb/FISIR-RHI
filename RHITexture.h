#pragma once

#include "RHIResource.h"

namespace FISIR {
	using Texture_t = void*;

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

	class RHITexture : public RHIResource {
	public:
		virtual ~RHITexture() {}

		virtual TextureSize getTextureSize() const = 0;

		virtual TextureLayout getCurrentLayout() const = 0;

		virtual TextureCOLORType getColorType() const = 0;

		virtual uint32_t getSampleCount() const = 0;

		virtual TextureUseForFlags getTextureUseFor() const = 0;
		
		virtual uint16_t getLayerCount() const = 0;

		virtual uint16_t getMipLevelCount() const = 0;

		virtual TextureType getTextureType() const = 0;
	};


}