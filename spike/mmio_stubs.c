/* Recording stubs for the __stest_mmio_* ABI. Deliberately a separate translation
 * unit from the test so calls cannot be inlined: the real call ABI (e.g. zeroext
 * on i8/i16 arguments and returns) is exercised. */
#include "mmio_stubs.h"

rec_t recs[64];
int nrecs;
uint64_t scripted = 0xA5;

static void rec(int op, const volatile void *a, int w, uint64_t v) {
  if (nrecs < 64) recs[nrecs++] = (rec_t){op, (uint64_t)(uintptr_t)a, w, v};
}

uint8_t  __stest_mmio_load8 (const volatile void *a) { rec(OP_LOAD, a, 8,  0); return (uint8_t)scripted; }
uint16_t __stest_mmio_load16(const volatile void *a) { rec(OP_LOAD, a, 16, 0); return (uint16_t)scripted; }
uint32_t __stest_mmio_load32(const volatile void *a) { rec(OP_LOAD, a, 32, 0); return (uint32_t)scripted; }
uint64_t __stest_mmio_load64(const volatile void *a) { rec(OP_LOAD, a, 64, 0); return scripted; }
void __stest_mmio_store8 (volatile void *a, uint8_t  v) { rec(OP_STORE, a, 8,  v); }
void __stest_mmio_store16(volatile void *a, uint16_t v) { rec(OP_STORE, a, 16, v); }
void __stest_mmio_store32(volatile void *a, uint32_t v) { rec(OP_STORE, a, 32, v); }
void __stest_mmio_store64(volatile void *a, uint64_t v) { rec(OP_STORE, a, 64, v); }

