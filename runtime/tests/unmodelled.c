#include "stest/mmio.h"
int main(void) {
  stest_rt_init();
  __stest_mmio_store32((volatile void *)0x40013800u, 1);
  return 0; /* unreachable: runtime aborts with exit 70 */
}
