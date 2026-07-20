#pragma once

#include <cstdint>

namespace FISIR {
	inline uint32_t HashCombine(uint32_t seed, uint32_t val) {
		return seed ^ (val + 0x9e3779b9 + (seed << 6) + (seed >> 2));
	}

	inline uint32_t HashPointer(const void* ptr) {
		if (!ptr) return 0;
		uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
		return (uint32_t)(addr ^ (addr >> 32));
	}
}
