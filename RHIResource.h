#pragma once

#include "RHITypes.h"
#include <cstdint>
#include <atomic>
#include <typeinfo>
#include "../Base/Object.h"

namespace FISIR {
	class RHICommandContext;
	


	class RHIResource {
	public:
		virtual ~RHIResource() {}

		virtual Type getResourceType() const { return Type::NLL; }

		virtual size_t getSize() const { return 0; }

		virtual void* getResourceAPIHandle() const = 0;

		virtual MemType getResourceMemType() const {return MemType::MemTypNone;}
		
		template<class T>
		T* as() {
			return static_cast<T*>(changeOtherHandle(typeid(T)));
		}
	private:
		std::atomic_bool waitTag {0};
		virtual void* changeOtherHandle(const std::type_info& typ) { return nullptr; };
	};

}