// Keep HIP before the standard headers to exercise the host macro collision.
#include <hip/hip_runtime.h>

#include <chrono>
#include <format>

#ifdef __noinline__
#error "HIP's __noinline__ macro must not leak into host C++ headers"
#endif

[[gnu::__noinline__]] static int Answer() {
  return 42;
}

int main() {
  const auto duration = std::chrono::seconds(Answer());
  return std::format("{}", duration.count()) == "42" ? 0 : 1;
}
