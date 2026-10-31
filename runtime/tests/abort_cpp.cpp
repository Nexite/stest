// Subprocess helper: each mode must end in stest_rt_abort (exit 70), never an escaping exception.
#include <cstring>
#include <stdexcept>
#include "stest/mmio.h"
#include "stest/runtime.hpp"
using namespace stest;
namespace {
struct Thrower : Peripheral {
  std::string_view name() const override { return "thrower"; }
  uint32_t read(uint32_t, unsigned) override { throw 42; }
  void write(uint32_t, unsigned, uint32_t) override {}
};
}  // namespace
int main(int argc, char** argv) {
  stest_rt_init();
  static Thrower t;
  runtime_bus().map(0x40030000, 0x10, &t);
  const char* m = argc > 1 ? argv[1] : "";
  if (!strcmp(m, "model_throw_int")) {
    __stest_mmio_load32((void*)0x40030000);
  } else if (!strcmp(m, "hook_throw_int")) {
    set_delivery_hook([] { throw 7; });
    __stest_mmio_store32((void*)0x40030004, 1);
  } else if (!strcmp(m, "hook_throw_std")) {
    set_delivery_hook([] { throw std::runtime_error("boom"); });
    __stest_mmio_store32((void*)0x40030004, 1);
  }
  return 0;
}
