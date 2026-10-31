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
  char m[64];
  snprintf(m, sizeof m, "record %d", i);
  expect(i < nrecs && recs[i].op == op && recs[i].addr == addr && recs[i].width == w &&
             (!check_v || recs[i].value == v), m);
}

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

  if (failures) return 1;
  puts("mmio_order OK");
  return 0;
}
