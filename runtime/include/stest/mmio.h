/* stest runtime C ABI. The LLVM MMIO pass rewrites firmware volatile accesses
 * into these calls (i8/i16 args and returns are zeroext). */
#ifndef STEST_MMIO_H
#define STEST_MMIO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

uint8_t  __stest_mmio_load8(const volatile void *addr);
uint16_t __stest_mmio_load16(const volatile void *addr);
uint32_t __stest_mmio_load32(const volatile void *addr);
uint64_t __stest_mmio_load64(const volatile void *addr);
void __stest_mmio_store8(volatile void *addr, uint8_t v);
void __stest_mmio_store16(volatile void *addr, uint16_t v);
void __stest_mmio_store32(volatile void *addr, uint32_t v);
void __stest_mmio_store64(volatile void *addr, uint64_t v);

/* Volatile memcpy/memmove/memset (the pass rewrites llvm.mem* with isvolatile).
 * Either memcpy side may be MMIO; overlap is handled like memmove. Each is split into
 * the widest naturally aligned accesses <= 4 bytes (aligned on both sides for memcpy,
 * else byte-wise), each going through the normal load/store path. */
void __stest_mmio_memcpy(volatile void *dst, const volatile void *src, size_t n);
void __stest_mmio_memset(volatile void *dst, int c, size_t n);

/* Prints "stest: " + formatted message to stderr, calls the abort hook if set
 * (stest::set_abort_hook; control mode sends ERROR), and exits with status 70. */
void stest_rt_abort(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
/* Current virtual time of the process-global clock, in nanoseconds. */
uint64_t stest_rt_now_ns(void);
/* Idempotent startup: reserves the peripheral/core address windows (Linux) or
 * asserts the host image lives above 4 GiB (macOS). Aborts on failure. */
void stest_rt_init(void);

#ifdef __cplusplus
}
#endif
#endif
