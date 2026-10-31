/* Test globals for the Python variable-by-name tests (read/written from the host). */
#include <stdbool.h>
#include <stdint.h>

#define KEEP __attribute__((used))

enum mode { MODE_OFF, MODE_ON };
struct lamp { uint16_t duty; bool on; };

KEEP uint8_t u8 = 200;
KEEP int16_t s16 = -5;
KEEP float f = 1.5f;
KEEP enum mode mode = MODE_ON;
KEEP struct lamp lamps[3] = {{10, 1}, {20, 0}, {30, 1}};
KEEP static int count = 1;
KEEP uint16_t seen_duty;  /* tick_fn copies lamps[0].duty here (firmware-side observation) */

void tick_fn(void)
{
  static int calls;
  calls++;
  seen_duty = lamps[0].duty;
  (void)count;
}
