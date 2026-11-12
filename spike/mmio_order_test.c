/* Verifies the stest MMIO pass: volatile accesses become __stest_mmio_* calls,
 * in program order; non-volatile accesses are left alone. The recording stubs
 * live in this file. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mmio_stubs.h"

#define GPIOA_BSRR ((volatile uint32_t *)0x48000018u)
#define GPIOA_IDR  ((volatile uint32_t *)0x48000010u)
#define SR         ((volatile uint16_t *)0x40006404u)
#define FREG       ((volatile float *)0x40001000u)
#define BREG       ((volatile uint8_t *)0x40002000u)

static uint32_t plain; /* non-volatile: must NOT be instrumented */
static int failures;

static void expect(int ok, const char *msg) {
  if (!ok) { printf("FAIL: %s\n", msg); failures++; }
}

static void expect_rec(int i, int op, uint64_t addr, int w, uint64_t v, int check_v) {
  char m[160];
  snprintf(m, sizeof m, "record %d (nrecs=%d)", i, nrecs);
  if (i < nrecs) snprintf(m + strlen(m), 60, " got op=%d addr=%llx w=%d", recs[i].op, (unsigned long long)recs[i].addr, recs[i].width);
  expect(i < nrecs && recs[i].op == op && recs[i].addr == addr && recs[i].width == w &&
             (!check_v || recs[i].value == v), m);
}

#define expect_mem(c, m) expect((c), m)

extern void ir_memset7(volatile void *);
extern void ir_memset_var(volatile void *, uint8_t, uint64_t);
extern void ir_memmove12(volatile void *, volatile void *);
extern void ir_memcpy_var(volatile void *, volatile void *, uint64_t);
extern void ir_memset_nonvolatile(void *);

int main(void) {
  *GPIOA_BSRR = 0x20;
  *GPIOA_BSRR = 0x200000;
  plain = 7;
  uint32_t idr = *GPIOA_IDR;
  *SR = 0xFFFE;

  expect(nrecs == 4, "exactly 4 records for the basic sequence");
  expect_rec(0, OP_STORE, 0x48000018u, 32, 0x20, 1);
  expect_rec(1, OP_STORE, 0x48000018u, 32, 0x200000, 1);
  expect_rec(2, OP_LOAD, 0x48000010u, 32, 0, 0);
  expect_rec(3, OP_STORE, 0x40006404u, 16, 0xFFFE, 1);
  expect(idr == 0xA5, "load returns scripted value");
  expect(plain == 7, "plain store untouched");

  nrecs = 0;
  *FREG = 1.5f; /* bit pattern 0x3FC00000 */
  expect(nrecs == 1, "float store: one record");
  expect_rec(0, OP_STORE, 0x40001000u, 32, 0x3FC00000u, 1);

  nrecs = 0;
  *BREG |= 1; /* RMW */
  expect(nrecs == 2, "u8 RMW: two records");
  expect_rec(0, OP_LOAD, 0x40002000u, 8, 0, 0);
  expect_rec(1, OP_STORE, 0x40002000u, 8, 0xA5 | 1, 1);

  /* Sub-32-bit ABI: values must arrive/return zero-extended (needs zeroext). */
  nrecs = 0;
  *(volatile uint8_t *)0x40003000u = 0xFF;
  *(volatile uint16_t *)0x40003004u = 0xFFFF;
  expect(nrecs == 2, "narrow stores: two records");
  expect_rec(0, OP_STORE, 0x40003000u, 8, 0xFF, 1);
  expect_rec(1, OP_STORE, 0x40003004u, 16, 0xFFFF, 1);
  scripted = 0x80;
  uint32_t b8 = *(volatile uint8_t *)0x40003000u;
  expect(b8 == 0x80, "u8 load 0x80 not sign-extended");
  scripted = 0x8000;
  uint32_t b16 = *(volatile uint16_t *)0x40003004u;
  expect(b16 == 0x8000, "u16 load 0x8000 not sign-extended");

  /* Volatile aggregate copies become __stest_mmio_memcpy calls (the runtime
   * splits them into aligned accesses). clang emits a volatile memcpy for volatile
   * struct assignment at both -O0 and -O2; assert the exact arguments. */
  struct S { uint32_t a, b; };
  volatile struct S *P = (volatile struct S *)0x40000000u;
  nrecs = 0;
  *P = (struct S){1, 2};
  expect_mem(nrecs == 1 && recs[0].op == OP_MEMCPY && recs[0].addr == 0x40000000u &&
                 recs[0].width == 8, "struct store: one memcpy, dst 0x40000000, len 8");
  if (nrecs == 1) {
    static const uint32_t want[2] = {1, 2};
    expect(memcmp((const void *)(uintptr_t)recs[0].src, want, 8) == 0, "struct store: src holds {1,2}");
  }

  struct Z { uint8_t b[6]; };
  volatile struct Z *Q = (volatile struct Z *)0x40000010u;
  volatile struct Z *R = (volatile struct Z *)0x40000020u;
  nrecs = 0;
  *Q = *R; /* volatile -> volatile */
  expect(nrecs == 1 && recs[0].op == OP_MEMCPY && recs[0].addr == 0x40000010u &&
             recs[0].src == 0x40000020u && recs[0].width == 6, "vol->vol copy: exact dst/src/len");

  struct Z host = {{1, 2, 3, 4, 5, 6}};
  nrecs = 0;
  *R = host; /* volatile dst, plain src */
  expect(nrecs == 1 && recs[0].op == OP_MEMCPY && recs[0].addr == 0x40000020u &&
             recs[0].src == (uint64_t)(uintptr_t)&host && recs[0].width == 6,
         "copy out: exact dst/src/len");
  nrecs = 0;
  host = *R; /* plain dst, volatile src */
  expect(nrecs == 1 && recs[0].op == OP_MEMCPY && recs[0].addr == (uint64_t)(uintptr_t)&host &&
             recs[0].src == 0x40000020u && recs[0].width == 6, "copy in: exact dst/src/len");

  /* IR-level cases (mmio_mem_ir.ll): real volatile memset/memmove, runtime lengths. */
  nrecs = 0;
  ir_memset7((void *)0x40000030u);
  expect(nrecs == 1 && recs[0].op == OP_MEMSET && recs[0].addr == 0x40000030u &&
             recs[0].width == 7 && recs[0].value == 90, "memset const: exact dst/byte/len");
  nrecs = 0;
  ir_memset_var((void *)0x40000038u, 0xC3, 11);
  expect(nrecs == 1 && recs[0].op == OP_MEMSET && recs[0].addr == 0x40000038u &&
             recs[0].width == 11 && recs[0].value == 0xC3, "memset var: exact dst/byte/len");
  nrecs = 0;
  ir_memmove12((void *)0x40000040u, (void *)0x40000044u); /* overlapping */
  expect(nrecs == 1 && recs[0].op == OP_MEMCPY && recs[0].addr == 0x40000040u &&
             recs[0].src == 0x40000044u && recs[0].width == 12, "memmove: exact dst/src/len");
  nrecs = 0;
  ir_memcpy_var((void *)0x40000050u, (void *)0x40000060u, 13);
  expect(nrecs == 1 && recs[0].op == OP_MEMCPY && recs[0].addr == 0x40000050u &&
             recs[0].src == 0x40000060u && recs[0].width == 13, "memcpy var: exact dst/src/len");
  nrecs = 0;
  ir_memset_nonvolatile(&host);
  expect(nrecs == 0 && host.b[0] == 1, "non-volatile memset untouched");

  if (failures) return 1;
  puts("mmio_order OK");
  return 0;
}
