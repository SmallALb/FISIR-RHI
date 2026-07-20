#pragma once

#include <cstdint>

namespace FISIR {
	class ExclusiveDepthStencil {
	public:
		enum Type {
			DepthNone = 0,
			DepthRead = 0x01,
			DepthWrite = 0x02,
			DepthMask = 0x0f,
			
			StencilNone = 0,
			StencilRead = 0x10,
			StencilWrite = 0x20,
			StencilMask = 0xf0,

			DN_SN = 0,
			DR_SN = DepthRead,
			DW_SN = DepthWrite,
			DN_SR = StencilRead,
			DW_SR = DepthWrite + StencilRead,
			DN_SW = StencilWrite,
			DR_SW = DepthRead + StencilWrite,
			DW_SW = DepthWrite + StencilWrite
		};

		ExclusiveDepthStencil(Type value = DN_SN): Value(value) {}

		inline bool IsUsingDepthStencil() const {return Value != DN_SN;}

		inline bool isUsingDepth() const {return GetDepth() != DepthNone; }

		inline bool isUsingStencil() const {return GetStencil() != StencilNone; }

		inline bool isDepthWrite() const {return GetDepth() == DepthWrite; }

		inline bool isDepthRead() const {return GetDepth() == DepthRead; }

		inline bool isStencilWrite() const {return GetStencil() == StencilWrite; }

		inline bool isStencilRead() const {return GetStencil() == StencilRead; }

		inline bool isWrite() const {return (isDepthWrite() || isStencilWrite()); }

		inline void setDepthWrite() { Value = (Type)(GetStencil() | DepthWrite); }
		
		inline void setStencilWrite() { Value = (Type)(GetDepth() | StencilWrite); }
	
		inline void setDWAndSW(bool depth, bool stencil)  {
			if (depth) setDepthWrite();
			if (stencil) setStencilWrite();
		}

		bool operator == (const ExclusiveDepthStencil& rhs) const {
			return Value == rhs.Value;
		}
		bool operator != (const ExclusiveDepthStencil& rhs) const {
			return Value != rhs.Value;
		}

		uint32_t GetHash() const {
			return (uint32_t)Value;
		}
	private:
		inline Type GetDepth() const { return (Type)(Value & DepthMask); }

		inline Type GetStencil() const {return (Type)(Value & StencilMask); }

	private:
		Type Value;
	};
}