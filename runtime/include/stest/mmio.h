/* stest runtime C ABI. The LLVM MMIO pass rewrites firmware volatile accesses
 * into these calls (i8/i16 args and returns are zeroext). */
#ifndef STEST_MMIO_H
#define STEST_MMIO_H
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

/* Prints "stest: " + formatted message to stderr and exits with status 70. */
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
