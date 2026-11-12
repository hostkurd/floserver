#pragma once

// What this computer's CPU can do, for the one runtime that cares (D31):
// Bun's regular Windows build needs AVX2, and a CPU without it gets Bun's
// "baseline" build, which both are in the signed list.

namespace floserver::core {

// Whether the CPU has AVX2. True where it can't be asked (not x86).
[[nodiscard]] bool cpu_has_avx2();

}  // namespace floserver::core
