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

TEST_CASE("RegisterFile: write to a fully read-only register throws") {
  RegisterFile rf;
  rf.declare(0, 0x5A, 0, 0);        // no writable, no W1C bit: read-only
  rf.declare(4, 0xF0, 0x0F, 0);     // mixed: RO bits are kept, no throw
  rf.declare(8, 0, 0, 0x3);         // W1C-only status register: writable
  rf.declare_write_only(12);        // write-only / model-handled (BSRR, KEYR, ...)
  try { rf.write(1, 1, 0xFF); FAIL("no throw"); }
  catch (const ReadOnlyRegister& e) { CHECK(e.off == 1); CHECK(e.width == 1); CHECK(e.is_write); }
  CHECK_THROWS_AS(rf.write(0, 4, 0), UnmodelledRegister);  // a ReadOnlyRegister is-an UnmodelledRegister
  CHECK(rf.read(0, 4) == 0x5A);
  CHECK_NOTHROW(rf.write(4, 4, 0xFFFFFFFF));
  CHECK(rf.read(4, 4) == 0xFF);
  rf.write(4, 4, 0);
  CHECK(rf.read(4, 4) == 0xF0);
  rf.set(8, 0x3);
  CHECK_NOTHROW(rf.write(8, 4, 0x1));
  CHECK(rf.read(8, 4) == 0x2);
  CHECK_NOTHROW(rf.write(12, 4, 0xFFFFFFFF));
  CHECK(rf.read(12, 4) == 0);  // stores nothing
  CHECK_THROWS_AS(rf.write(14, 4, 0), UnmodelledRegister);  // strictness checks still apply
}

namespace {
struct Dummy : Peripheral {
  RegisterFile rf;
  Dummy() { rf.declare(0, 7, 0xFF); rf.declare(4, 3, 0); }
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
  try { b.write(0x40010004, 2, 1); FAIL("no throw"); }
  catch (const UnmodelledAccess& e) {
    CHECK(e.read_only); CHECK(e.addr == 0x40010004); CHECK(e.width == 2); CHECK(e.is_write);
    CHECK(e.peripheral == "dummy");
  }
  CHECK(b.read(0x40010004, 4) == 3);
  try { b.read(0x40010010, 4); FAIL("no throw"); }
  catch (const UnmodelledAccess& e) { CHECK_FALSE(e.read_only); }
  CHECK_THROWS_AS(b.read(0x400103FE, 4), UnmodelledAccess);  // straddles end
  CHECK_THROWS_AS(b.map(0x40010200, 0x400, &d), std::invalid_argument);
  CHECK_THROWS_AS(d.call("x", {}), UnknownOp);
}

TEST_CASE("lane helpers: every offset x width") {
  // width 1: offsets 0..3; width 2: offsets 0..2; width 4: offset 0 (crossing combos are
  // rejected by callers, helpers just compute within the word)
  const uint32_t w = 0xAABBCCDD;
  struct Row { uint32_t off; unsigned width; uint32_t mask, extract; };
  const Row rows[] = {
      {0, 1, 0x000000FF, 0xDD}, {1, 1, 0x0000FF00, 0xCC}, {2, 1, 0x00FF0000, 0xBB},
      {3, 1, 0xFF000000, 0xAA}, {0, 2, 0x0000FFFF, 0xCCDD}, {1, 2, 0x00FFFF00, 0xBBCC},
      {2, 2, 0xFFFF0000, 0xAABB}, {0, 4, 0xFFFFFFFF, 0xAABBCCDD},
      {4, 1, 0x000000FF, 0xDD},  // offset taken modulo 4
      {6, 2, 0xFFFF0000, 0xAABB},
  };
  for (const Row& r : rows) {
    CAPTURE(r.off); CAPTURE(r.width);
    CHECK(lane_mask(r.off, r.width) == r.mask);
    CHECK(lane_extract(w, r.off, r.width) == r.extract);
    CHECK(lane_merge(w, r.off, r.width, 0) == (w & ~r.mask));
    CHECK(lane_merge(w, r.off, r.width, 0xFFFFFFFF) == (w | r.mask));
    CHECK(lane_merge(0, r.off, r.width, r.extract) == (w & r.mask));
  }
}

TEST_CASE("RegisterFile: misaligned or oversized declare throws") {
  RegisterFile rf;
  CHECK_THROWS_AS(rf.declare(0x402, 0, 0xFF), std::invalid_argument);
  CHECK_THROWS_AS(rf.declare(0x401, 0, 0xFF), std::invalid_argument);
  CHECK_THROWS_AS(rf.declare(0x400, 0, 0xFF), std::invalid_argument);
  CHECK_THROWS_AS(rf.declare_write_only(0x402), std::invalid_argument);
  CHECK_THROWS_AS(rf.declare(2, 0, 0xFF), std::invalid_argument);
  CHECK_NOTHROW(rf.declare(0x3FC, 0, 0xFF));
  CHECK(rf.read(0x3FC, 4) == 0);
  CHECK_THROWS_AS(rf.read(0x400, 4), UnmodelledRegister);  // beyond block: unmodelled
  CHECK_THROWS_AS(rf.write(0x10000, 4, 1), UnmodelledRegister);
}

TEST_CASE("Bus: mapping outside the MMIO windows throws") {
  struct D : Peripheral {
    std::string_view name() const override { return "d"; }
    uint32_t read(uint32_t, unsigned) override { return 0; }
    void write(uint32_t, unsigned, uint32_t) override {}
  } p;
  Bus b;
  CHECK_THROWS_AS(b.map(0xA0000000, 0x400, &p), std::invalid_argument);
  CHECK_THROWS_AS(b.map(0x3FFFFC00, 0x800, &p), std::invalid_argument);  // straddles window start
  CHECK_THROWS_AS(b.map(0x5FFFFC00, 0x800, &p), std::invalid_argument);  // straddles window end
  CHECK_THROWS_AS(b.map(0xE0000000, 0x200000, &p), std::invalid_argument);
  CHECK_NOTHROW(b.map(0x5FFFFC00, 0x400, &p));
  CHECK_NOTHROW(b.map(0xE000E000, 0x1000, &p));
}
