// clusterlod.h implementation unit.
// meshoptimizer's demo/clusterlod.h is header-only: it needs the meshoptimizer core API to be
// declared first, and the implementation is enabled by defining CLUSTERLOD_IMPLEMENTATION in
// exactly one translation unit. Both are handled here so the rest of the project can simply
// include <clusterlod.h> and call clodBuild/clodLocalIndices.
#include <meshoptimizer.h>

#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
