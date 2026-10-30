#include <doctest/doctest.h>
#include "stest/bus.hpp"
using namespace stest;

TEST_CASE("RegisterFile: W1C clears only written bits") {
  RegisterFile rf; rf.declare(0, 0, 0, 0xF); rf.set(0, 0xF);
  rf.write(0, 4, 0x1);
  CHECK(rf.read(0, 4) == 0xE);
}
TEST_CASE("RegisterFile: rw_mask protects read-only bits; reset values") {
  RegisterFile rf; rf.declare(4, 0xA5, 0x0F, 0);
  CHECK(rf.read(4, 4) == 0xA5);
  rf.write(4, 4, 0xFFFFFFFF);
  CHECK(rf.read(4, 4) == 0xAF);
  rf.reset();
  CHECK(rf.read(4, 4) == 0xA5);
}
TEST_CASE("RegisterFile: sub-word lanes") {
  RegisterFile rf; rf.declare(0, 0x11223344, 0xFFFFFFFF, 0);
  CHECK(rf.read(1, 1) == 0x33);
  CHECK(rf.read(2, 2) == 0x1122);
  rf.write(1, 1, 0xAB);
  CHECK(rf.read(0, 4) == 0x1122AB44);
}
TEST_CASE("RegisterFile: undeclared throws") {
  RegisterFile rf; rf.declare(0, 0, 0xFF);
  CHECK_THROWS_AS(rf.read(8, 4), UnmodelledRegister);
  CHECK_THROWS_AS(rf.write(8, 4, 1), UnmodelledRegister);
}

TEST_CASE("RegisterFile: access crossing a register boundary throws") {
  RegisterFile rf; rf.declare(0, 0, 0xFFFFFFFF); rf.declare(4, 0, 0xFFFFFFFF);
  CHECK_THROWS_AS(rf.read(2, 4), UnmodelledRegister);
  CHECK_THROWS_AS(rf.write(2, 4, 1), UnmodelledRegister);
  CHECK_THROWS_AS(rf.read(3, 2), UnmodelledRegister);
  CHECK_NOTHROW(rf.read(2, 2));
}

namespace {
struct Dummy : Peripheral {
  RegisterFile rf;
  Dummy() { rf.declare(0, 7, 0xFF); }
  std::string_view name() const override { return "dummy"; }
  uint32_t read(uint32_t o, unsigned w) override { return rf.read(o, w); }
  void write(uint32_t o, unsigned w, uint32_t v) override { rf.write(o, w, v); }
};
}  // namespace

TEST_CASE("Bus: routing, unmapped, unmodelled register") {
  Bus b; Dummy d;
  b.map(0x40010000, 0x400, &d);
  CHECK(b.read(0x40010000, 4) == 7);
  b.write(0x40010000, 4, 9);
  CHECK(b.read(0x40010000, 1) == 9);
  try { b.write(0x40013800, 4, 1); FAIL("no throw"); }
  catch (const UnmodelledAccess& e) {
    CHECK(e.addr == 0x40013800); CHECK(e.width == 4); CHECK(e.is_write); CHECK(e.peripheral.empty());
  }
  try { b.read(0x40010010, 4); FAIL("no throw"); }
  catch (const UnmodelledAccess& e) { CHECK(e.peripheral == "dummy"); CHECK_FALSE(e.is_write); }
  CHECK_THROWS_AS(b.read(0x400103FE, 4), UnmodelledAccess);  // straddles end
  CHECK_THROWS_AS(b.map(0x40010200, 0x400, &d), std::invalid_argument);
  CHECK_THROWS_AS(d.call("x", {}), UnknownOp);
}
