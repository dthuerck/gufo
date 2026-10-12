#pragma once

#include <hip/hip_runtime.h>

// HIP defines __noinline__ as an empty macro in ordinary C++ translation
// units. It breaks libstdc++ 16's [[__gnu__::__noinline__]] attribute when
// standard headers follow HIP headers. Load HIP first and remove that host
// macro before parsing any project headers. HIP compilation keeps its own
// keyword/attribute handling unchanged.
#if !defined(__HIP__) && defined(__noinline__)
#undef __noinline__
#endif
