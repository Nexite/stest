// GPIO HAL-shim C API (stest/hal_api.h, MODELS GPIO=hal) on the process-global runtime bus and
// clock: register state after each operation, the virtual time charged (one access cost per
// register access ST's stm32l4xx_hal_gpio.c makes), and no hal-mode warning for these accesses.
#include <doctest/doctest.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>
#include "stest/cortexm.hpp"
#include "stest/hal_api.h"
#include "stest/model_context.hpp"
#include "stest/runtime.hpp"
#include "stm32l4/models/gpio.hpp"
using namespace stest;
using nlohmann::json;

namespace {
const IrqHandler kTable[16 + 82] = {};
constexpr uint64_t GPIOA = 0x48000000, GPIOC = 0x48000800;
enum : uint32_t { MODER = 0x00, OTYPER = 0x04, OSPEEDR = 0x08, PUPDR = 0x0C, ODR = 0x14, LCKR = 0x1C, AFRH = 0x24 };

struct Rig {
  Bus& bus = runtime_bus();
  Clock& clock = runtime_clock();
  Cortexm core{bus, clock, 4, kTable, 16 + 82};
  ModelContext ctx{clock, core, bus};
  stm32l4::Gpio a{"GPIOA", json{{"port", "A"}}, ctx};
  stm32l4::Gpio c{"GPIOC", json{{"port", "C"}, {"moder_reset", 0xFFFFFFFFu}}, ctx};
  Rig() {
    a.reset();
    c.reset();
    bus.map(GPIOA, 0x400, &a);
    bus.map(GPIOC, 0x400, &c);
    bus.set_hal_owned(&a, "GPIO");
    bus.set_hal_owned(&c, "GPIO");
  }
  ~Rig() {
    bus.unmap(&a);
    bus.unmap(&c);
  }
  uint64_t accesses_since(uint64_t t0) const { return (clock.now_ns() - t0) / access_cost_ns(); }
};

template <class F>
std::string capture_stderr(F f) {
  std::fflush(stderr);
  FILE* tmp = std::tmpfile();
  REQUIRE(tmp);
  int saved = dup(2);
  dup2(fileno(tmp), 2);
  f();
  std::fflush(stderr);
  dup2(saved, 2);
  close(saved);
  std::string out;
  std::rewind(tmp);
  for (int ch; (ch = std::fgetc(tmp)) != EOF;) out += static_cast<char>(ch);
  std::fclose(tmp);
  return out;
}
}  // namespace

TEST_CASE("gpio hal api: init/write/toggle/read keep registers consistent, ST's access count") {
  Rig r;
  std::string err = capture_stderr([&] {
    uint64_t t = r.clock.now_ns();
    // PA5 output push-pull, no pull, high speed (GPIO_MODE_OUTPUT_PP = 1, SPEED_FREQ_HIGH = 2)
    stest_gpio_hal_init('A', 1u << 5, 0x1, 0, 2, 0);
    CHECK(r.accesses_since(t) == 8);  // OSPEEDR, OTYPER, PUPDR, MODER read-modify-writes
    CHECK(((r.a.read(MODER, 4) >> 10) & 3u) == 1u);
    CHECK(((r.a.read(OSPEEDR, 4) >> 10) & 3u) == 2u);
    CHECK(r.a.read(OTYPER, 4) == 0);

    t = r.clock.now_ns();
    stest_gpio_hal_write('A', 1u << 5, 1);
    CHECK(r.accesses_since(t) == 1);  // BSRR
    CHECK(r.a.read(ODR, 4) == (1u << 5));
    CHECK(r.a.pin_level(5) == 1);
    t = r.clock.now_ns();
    CHECK(stest_gpio_hal_read('A', 1u << 5) == 1);
    CHECK(r.accesses_since(t) == 1);  // IDR
    t = r.clock.now_ns();
    stest_gpio_hal_toggle('A', 1u << 5);
    CHECK(r.accesses_since(t) == 2);  // ODR, BSRR
    CHECK(r.a.read(ODR, 4) == 0);
    stest_gpio_hal_write('A', 1u << 5, 0);  // BRR
    CHECK(r.a.read(ODR, 4) == 0);

    // PC9 AF7 open-drain pull-up (GPIO_MODE_AF_OD = 0x12): AFRH and OTYPER set
    t = r.clock.now_ns();
    stest_gpio_hal_init('C', 1u << 9, 0x12, 1, 0, 7);
    CHECK(r.accesses_since(t) == 10);
    CHECK(((r.c.read(AFRH, 4) >> 4) & 0xFu) == 7u);
    CHECK(r.c.read(OTYPER, 4) == (1u << 9));
    CHECK(((r.c.read(PUPDR, 4) >> 18) & 3u) == 1u);
    CHECK(((r.c.read(MODER, 4) >> 18) & 3u) == 2u);
    // analog: no PUPDR access, MODER only
    t = r.clock.now_ns();
    stest_gpio_hal_init('C', 1u << 0, 0x3, 0, 0, 0);
    CHECK(r.accesses_since(t) == 2);

    t = r.clock.now_ns();
    stest_gpio_hal_deinit('C', 1u << 9);
    CHECK(r.accesses_since(t) == 10);
    CHECK(((r.c.read(MODER, 4) >> 18) & 3u) == 3u);
    CHECK(r.c.read(AFRH, 4) == 0);
    CHECK(r.c.read(OTYPER, 4) == 0);
    CHECK(r.c.read(PUPDR, 4) == 0);
  });
  CHECK(err.empty());  // shim accesses on the firmware's behalf never warn
}

TEST_CASE("gpio hal api: LCKR key sequence locks the configuration") {
  Rig r;
  const uint32_t kLckk = 1u << 16;
  stest_gpio_hal_lckr_write('A', kLckk | (1u << 5));
  stest_gpio_hal_lckr_write('A', 1u << 5);
  stest_gpio_hal_lckr_write('A', kLckk | (1u << 5));
  (void)stest_gpio_hal_lckr_read('A');
  CHECK((stest_gpio_hal_lckr_read('A') & kLckk) != 0);
  CHECK(r.a.read(LCKR, 4) == (kLckk | (1u << 5)));
  const uint32_t moder = r.a.read(MODER, 4);
  stest_gpio_hal_init('A', 1u << 5, 0x1, 0, 0, 0);  // locked pin keeps its mode
  CHECK(r.a.read(MODER, 4) == moder);
}

TEST_CASE("gpio hal api: firmware MMIO to the same hal-owned block warns") {
  Rig r;
  std::string err = capture_stderr([&] { r.bus.write(GPIOA + 0x18, 4, 1u << 5); });
  CHECK(err == "stest: warning: firmware accessed GPIOA BSRR directly while GPIO is in hal mode\n");
}
