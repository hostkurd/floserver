#include "floserver/core/cpu_features.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace floserver::core {

bool cpu_has_avx2() {
#ifdef _WIN32
    constexpr DWORD kAvx2Feature = 40;  // PF_AVX2_INSTRUCTIONS_AVAILABLE
    return IsProcessorFeaturePresent(kAvx2Feature) != FALSE;
#elif defined(__x86_64__) || defined(__i386__)
    return __builtin_cpu_supports("avx2") != 0;
#else
    return true;
#endif
}

}  // namespace floserver::core
