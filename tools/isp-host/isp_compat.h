/* isp_compat.h - the few compiler-specific pieces of isphost.c: GCC/Clang builtins vs MSVC intrinsics (S10eIsp builds
   the core with MSVC in the EWDK; the test builds use zig/clang). */
#ifndef ISP_COMPAT_H
#define ISP_COMPAT_H
#if (defined(_MSC_VER) && !defined(__clang__)) || defined(ISP_COMPAT_MSVC)   /* ISP_COMPAT_MSVC: check the MSVC branch with clang */
#include <intrin.h>
#define ISP_TLS __declspec(thread)
#define ISP_ADD64(p, v) ((void)_InterlockedExchangeAdd64((volatile __int64 *)(p), (__int64)(v)))
/* full barrier (stronger than the release the GCC branch asks for) */
#define ISP_STORE32_REL(p, v) ((void)_InterlockedExchange((volatile long *)(p), (long)(v)))
#else
#define ISP_TLS _Thread_local
#define ISP_ADD64(p, v) ((void)__atomic_add_fetch((p), (v), __ATOMIC_RELAXED))
#define ISP_STORE32_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#endif
#endif
