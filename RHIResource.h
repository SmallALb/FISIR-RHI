#pragma once

#include <cstdint>
#include <typeinfo>
#include "../Base/Object.h"

namespace FISIR {
	class RHICommandContext;
	

	enum class ResourceAccess {
		Undefined = 0,
		ReadOnly = 1,
		WriteOnly = 2,
		ReadWrite = 3,
		TransferSrc = 4,
		TransferDst = 5,
	};

	enum class TextureLayout {
		Undefined = 0,
		ColorAttachmentOptimal = 1,
		DepthStencilAttachmentOptimal = 2,
		ShaderReadOnlyOptimal = 3,
		TransferSrcOptimal = 4,
		TransferDstOptimal = 5,
		Storage = 6,
	};

	enum class BufferLayout {
		Undefined = 0,
		VertexBuffer = 1,
		IndexBuffer = 2,
		UniformBuffer = 3,
		StorageBuffer = 4,
	};

	enum MemType : uint32_t {
		MemTypNone = 0,
		MemTypeDeviceLocal = 0x00000001,
		MemTypHostVisable = 0x00000002,
		MemTypHostCoherent = 0x00000004,
		MemTypHostCached = 0x00000008,
	};


	using MemTypeFlags = uint32_t;

	enum class Type {
		NLL,
		Buffer,
		Texture,
		Sampler,
		RenderPass,
		Pipeline,
	};


	class RHIResource : public Object {
	public:
		virtual ~RHIResource() {}

		inline virtual const char* outPutString() const override {
			return "RHIResource";
		}

		virtual Type getResourceType() const { return Type::NLL; }

		virtual size_t getSize() const { return 0; }

		virtual void* getResourceAPIHandle() const = 0;

		virtual MemType getResourceMemType() const {return MemType::MemTypNone;}

		template<class T>
		T* as() {
			return static_cast<T*>(changeOtherHandle(typeid(T)));
		}
	private:
		virtual void* changeOtherHandle(const std::type_info& typ) { return nullptr; };
	};

}