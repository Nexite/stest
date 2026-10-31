/* stest host replacement for CMSIS-Core's cmsis_compiler.h (+ cmsis_gcc.h).
 *
 * Put chips/cmsis_host on the include path BEFORE the CMSIS-Core Include dir so that
 * core_cm4.h's `#include "cmsis_compiler.h"` picks this file up. That quoted include
 * searches core_cm4.h's own directory first, so the CMSIS-Core dir used must not
 * contain the toolchain headers (cmsis_compiler.h, cmsis_gcc.h, ...):
 * stest_cmsis_core_include() (cmake/StestFetchST.cmake) stages a copy without them. Firmware is compiled
 * for the host (x86-64 / arm64), so no ARM assembly appears here:
 *  - barriers (__NOP/__DSB/__DMB/__ISB) are compiler barriers;
 *  - __WFI/__WFE and the PRIMASK/BASEPRI/FAULTMASK/IPSR intrinsics call the stest
 *    runtime C API (stest/cortexm_c.h, runtime/include must be on the include path);
 *  - __REV/__REV16/__REVSH/__RBIT/__CLZ/__ROR/__SSAT/__USAT are portable C;
 *  - exclusive accesses (__LDREX* / __STREX*) are plain accesses that always succeed
 *    (single host thread);
 *  - stack-pointer / CONTROL / FPSCR / xPSR accessors are dummies.
 * Not provided: ARMv8-M (TrustZone, MSPLIM/PSPLIM, LDA/STL) and the DSP/SIMD
 * intrinsics (__SADD8, __SMLAD, ...).
 * Plain C11; also valid C++. The guard names match CMSIS's so the real
 * cmsis_compiler.h / cmsis_gcc.h become no-ops if reached afterwards.
 */
#ifndef __CMSIS_COMPILER_H
#define __CMSIS_COMPILER_H
#ifndef __CMSIS_GCC_H
#define __CMSIS_GCC_H
#endif

#include <stdint.h>
#include "stest/cortexm_c.h"
#include "stest/mmio.h" /* stest_rt_abort */

/* ---- attribute / keyword macros ---- */
#ifndef __ASM
/* Inline asm reaches the HOST assembler: only portable statements (e.g. "" ::: "memory",
 * "nop") assemble. ARM instructions fail at build time rather than being dropped. */
#define __ASM __asm
#endif
#ifndef __INLINE
#define __INLINE inline
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#endif
#ifndef __NO_RETURN
#define __NO_RETURN __attribute__((__noreturn__))
#endif
#ifndef __USED
#define __USED __attribute__((used))
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif
#ifndef __PACKED
#define __PACKED __attribute__((packed, aligned(1)))
#endif
#ifndef __PACKED_STRUCT
#define __PACKED_STRUCT struct __attribute__((packed, aligned(1)))
#endif
#ifndef __PACKED_UNION
#define __PACKED_UNION union __attribute__((packed, aligned(1)))
#endif
#ifndef __ALIGNED
#define __ALIGNED(x) __attribute__((aligned(x)))
#endif
#ifndef __RESTRICT
#define __RESTRICT __restrict
#endif
#ifndef __COMPILER_BARRIER
#define __COMPILER_BARRIER() __asm volatile("" ::: "memory")
#endif

/* ---- unaligned access helpers (same packed-struct technique as cmsis_gcc.h) ---- */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#pragma GCC diagnostic ignored "-Wattributes"
#ifndef __UNALIGNED_UINT32 /* deprecated in CMSIS */
struct __attribute__((packed)) T_UINT32 { uint32_t v; };
#define __UNALIGNED_UINT32(x) (((struct T_UINT32 *)(x))->v)
#endif
#ifndef __UNALIGNED_UINT16_WRITE
__PACKED_STRUCT T_UINT16_WRITE { uint16_t v; };
#define __UNALIGNED_UINT16_WRITE(addr, val) (void)((((struct T_UINT16_WRITE *)(void *)(addr))->v) = (val))
#endif
#ifndef __UNALIGNED_UINT16_READ
__PACKED_STRUCT T_UINT16_READ { uint16_t v; };
#define __UNALIGNED_UINT16_READ(addr) (((const struct T_UINT16_READ *)(const void *)(addr))->v)
#endif
#ifndef __UNALIGNED_UINT32_WRITE
__PACKED_STRUCT T_UINT32_WRITE { uint32_t v; };
#define __UNALIGNED_UINT32_WRITE(addr, val) (void)((((struct T_UINT32_WRITE *)(void *)(addr))->v) = (val))
#endif
#ifndef __UNALIGNED_UINT32_READ
__PACKED_STRUCT T_UINT32_READ { uint32_t v; };
#define __UNALIGNED_UINT32_READ(addr) (((const struct T_UINT32_READ *)(const void *)(addr))->v)
#endif
#pragma GCC diagnostic pop

/* ---- Mach-O (macOS) host adjustments for GCC-style embedded code ----
 * 1. Apple clang targets predefine `__weak` as an Objective-C GC qualifier, which
 *    makes ST's `#ifndef __weak / #define __weak __attribute__((weak))` (hal_def.h,
 *    included after this file) a no-op: HAL weak defaults would become strong and
 *    clash with firmware overrides. Give `__weak` its GCC meaning.
 * 2. Mach-O section specifiers must be "segment,section"; ELF-style names used by
 *    embedded code (e.g. HAL's __RAM_FUNC `section(".RamFunc")`, `.noinit`) are a
 *    compile error there. Placement is meaningless on the host, so on macOS the
 *    `section` attribute is dropped (mapped to `used`). Limitation: firmware that
 *    uses `section(` as an ordinary identifier does not compile on macOS. */
#if defined(__APPLE__)
#undef __weak
#define __weak __attribute__((weak))
#define section(name) used
#define __section__(name) used
#endif

/* Startup-file macros: meaningless on the host (no vector table / reset handler). */
#ifndef __VECTOR_TABLE_ATTRIBUTE
#define __VECTOR_TABLE_ATTRIBUTE __attribute__((used))
#endif

/* ---- barriers / hints ---- */
#define __NOP() __COMPILER_BARRIER()
#define __DSB() __COMPILER_BARRIER()
#define __DMB() __COMPILER_BARRIER()
#define __ISB() __COMPILER_BARRIER()
#define __SEV() __COMPILER_BARRIER()
#define __WFI() stest_rt_wfi()
#define __WFE() stest_rt_wfi()
#define __BKPT(value) stest_rt_abort("__BKPT(%d) reached", (int)(value))

/* ---- core registers (runtime C API) ---- */
__STATIC_FORCEINLINE void __enable_irq(void) { stest_rt_enable_irq(); }
__STATIC_FORCEINLINE void __disable_irq(void) { stest_rt_disable_irq(); }
__STATIC_FORCEINLINE uint32_t __get_PRIMASK(void) { return stest_rt_get_primask(); }
__STATIC_FORCEINLINE void __set_PRIMASK(uint32_t priMask) { stest_rt_set_primask(priMask); }
__STATIC_FORCEINLINE void __enable_fault_irq(void) { stest_rt_set_faultmask(0u); }
__STATIC_FORCEINLINE void __disable_fault_irq(void) { stest_rt_set_faultmask(1u); }
__STATIC_FORCEINLINE uint32_t __get_FAULTMASK(void) { return stest_rt_get_faultmask(); }
__STATIC_FORCEINLINE void __set_FAULTMASK(uint32_t faultMask) { stest_rt_set_faultmask(faultMask); }
__STATIC_FORCEINLINE uint32_t __get_BASEPRI(void) { return stest_rt_get_basepri(); }
__STATIC_FORCEINLINE void __set_BASEPRI(uint32_t basePri) { stest_rt_set_basepri(basePri); }
__STATIC_FORCEINLINE void __set_BASEPRI_MAX(uint32_t basePri) { stest_rt_set_basepri_max(basePri); }
__STATIC_FORCEINLINE uint32_t __get_IPSR(void) { return stest_rt_get_ipsr(); }
__STATIC_FORCEINLINE uint32_t __get_xPSR(void) { return stest_rt_get_ipsr(); }
__STATIC_FORCEINLINE uint32_t __get_APSR(void) { return 0u; }

/* Dummies: the host stack is not the firmware's, and there is one privileged mode. */
__STATIC_FORCEINLINE uint32_t __get_CONTROL(void) { return 0u; }
__STATIC_FORCEINLINE void __set_CONTROL(uint32_t control) { (void)control; }
__STATIC_FORCEINLINE uint32_t __get_MSP(void) { return 0u; }
__STATIC_FORCEINLINE void __set_MSP(uint32_t topOfMainStack) { (void)topOfMainStack; }
__STATIC_FORCEINLINE uint32_t __get_PSP(void) { return 0u; }
__STATIC_FORCEINLINE void __set_PSP(uint32_t topOfProcStack) { (void)topOfProcStack; }
__STATIC_FORCEINLINE uint32_t __get_FPSCR(void) { return 0u; }
__STATIC_FORCEINLINE void __set_FPSCR(uint32_t fpscr) { (void)fpscr; }

/* ---- data processing ---- */
#if defined(__has_builtin)
#if __has_builtin(__builtin_bitreverse32)
#define STEST_HAVE_BITREVERSE32 1
#endif
#endif
__STATIC_FORCEINLINE uint32_t __REV(uint32_t value) { return __builtin_bswap32(value); }
__STATIC_FORCEINLINE uint32_t __REV16(uint32_t value) {
  return ((value >> 8) & 0x00FF00FFu) | ((value << 8) & 0xFF00FF00u);
}
__STATIC_FORCEINLINE int16_t __REVSH(int16_t value) {
  return (int16_t)__builtin_bswap16((uint16_t)value);
}
__STATIC_FORCEINLINE uint32_t __ROR(uint32_t op1, uint32_t op2) {
  op2 %= 32u;
  return op2 == 0u ? op1 : (op1 >> op2) | (op1 << (32u - op2));
}
__STATIC_FORCEINLINE uint32_t __RBIT(uint32_t value) {
#ifdef STEST_HAVE_BITREVERSE32
  return __builtin_bitreverse32(value);
#else
  uint32_t r = 0u;
  for (int i = 0; i < 32; ++i) { r = (r << 1) | (value & 1u); value >>= 1; }
  return r;
#endif
}
__STATIC_FORCEINLINE uint8_t __CLZ(uint32_t value) {
  return value == 0u ? 32u : (uint8_t)__builtin_clz(value);
}
__STATIC_FORCEINLINE int32_t __SSAT(int32_t val, uint32_t sat) {
  if (sat >= 1u && sat <= 32u) {
    const int32_t max = (int32_t)((1u << (sat - 1u)) - 1u);
    const int32_t min = -1 - max;
    if (val > max) return max;
    if (val < min) return min;
  }
  return val;
}
__STATIC_FORCEINLINE uint32_t __USAT(int32_t val, uint32_t sat) {
  if (sat <= 31u) {
    const uint32_t max = (1u << sat) - 1u;
    if (val > (int32_t)max) return max;
    if (val < 0) return 0u;
  }
  return (uint32_t)val;
}
__STATIC_FORCEINLINE uint32_t __RRX(uint32_t value) { return value >> 1; } /* carry-in: 0 */

/* ---- exclusive / unprivileged accesses: plain accesses (single host thread) ---- */
__STATIC_FORCEINLINE uint8_t __LDREXB(volatile uint8_t *addr) { return *addr; }
__STATIC_FORCEINLINE uint16_t __LDREXH(volatile uint16_t *addr) { return *addr; }
__STATIC_FORCEINLINE uint32_t __LDREXW(volatile uint32_t *addr) { return *addr; }
__STATIC_FORCEINLINE uint32_t __STREXB(uint8_t value, volatile uint8_t *addr) { *addr = value; return 0u; }
__STATIC_FORCEINLINE uint32_t __STREXH(uint16_t value, volatile uint16_t *addr) { *addr = value; return 0u; }
__STATIC_FORCEINLINE uint32_t __STREXW(uint32_t value, volatile uint32_t *addr) { *addr = value; return 0u; }
__STATIC_FORCEINLINE void __CLREX(void) {}
__STATIC_FORCEINLINE uint8_t __LDRBT(volatile uint8_t *ptr) { return *ptr; }
__STATIC_FORCEINLINE uint16_t __LDRHT(volatile uint16_t *ptr) { return *ptr; }
__STATIC_FORCEINLINE uint32_t __LDRT(volatile uint32_t *ptr) { return *ptr; }
__STATIC_FORCEINLINE void __STRBT(uint8_t value, volatile uint8_t *ptr) { *ptr = value; }
__STATIC_FORCEINLINE void __STRHT(uint16_t value, volatile uint16_t *ptr) { *ptr = value; }
__STATIC_FORCEINLINE void __STRT(uint32_t value, volatile uint32_t *ptr) { *ptr = value; }

#endif /* __CMSIS_COMPILER_H */
