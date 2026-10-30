/* Verifies the stest MMIO pass: volatile accesses become __stest_mmio_* calls,
 * in program order; non-volatile accesses are left alone. The recording stubs
 * live in this file. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OP_LOAD, OP_STORE };
typedef struct { int op; uint64_t addr; int width; uint64_t value; } rec_t;

static rec_t recs[64];
static int nrecs;
static uint64_t scripted = 0xA5;

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

  if (failures) return 1;
  puts("mmio_order OK");
  return 0;
}
