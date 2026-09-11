#ifndef USBSCPI_ATOMIC_H
#define USBSCPI_ATOMIC_H

/*
 * Acquire/release primitives for the component's cross-thread state.
 *
 * Used by the SPSC ring buffer and by the stream glue's attach flag. Both have
 * exactly one writer per variable, so a full atomic RMW is never needed — only
 * ordered load and store.
 *
 * Deliberately NOT C11 <stdatomic.h>. MSVC's support for it is recent and
 * gated behind /experimental:c11atomics, and CMakeLists.txt pins non-MSVC
 * builds to c_std_99 on purpose (the embedded toolchains follow it). The
 * compiler builtins below work in C99 on every toolchain this component
 * targets: GCC and Clang cover Linux, MinGW, ESP-IDF, Pico SDK, and the bare
 * ARM cross-compilers.
 *
 * There is deliberately no `volatile` fallback. `volatile` orders the compiler
 * but not the CPU, which is exactly the bug these primitives exist to fix; a
 * silent fallback would reintroduce it on precisely the weakly-ordered targets
 * (aarch64, Xtensa SMP) where it matters. A toolchain without either
 * implementation must fail the build.
 */

#if defined(__GNUC__) || defined(__clang__)

#define usbscpi_load_acquire(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define usbscpi_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))

#include <intrin.h>

/* x86/x64 loads carry acquire and stores carry release in hardware, so only
 * the compiler needs restraining. This is NOT true of MSVC on ARM64, which is
 * why the guard above is narrow rather than `defined(_MSC_VER)`. */
#define usbscpi_load_acquire(p)     (_ReadWriteBarrier(), *(p))
#define usbscpi_store_release(p, v) do { _ReadWriteBarrier(); *(p) = (v); } while (0)

#else
#error "usbscpi: no acquire/release implementation for this compiler. \
Add one rather than falling back to volatile, which does not order the CPU."
#endif

#endif /* USBSCPI_ATOMIC_H */
