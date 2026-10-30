#include "stest/peripheral.hpp"

namespace stest {

nlohmann::json Peripheral::call(std::string_view op, const nlohmann::json&) {
  throw UnknownOp(op);
}

void RegisterFile::declare(uint32_t off, uint32_t reset, uint32_t rw, uint32_t w1c) {
  regs_[off] = Reg{reset, reset, rw, w1c};
}

RegisterFile::Reg& RegisterFile::find(uint32_t off, unsigned width, bool is_write) {
  auto it = regs_.find(off & ~3u);
  if (it == regs_.end()) throw UnmodelledRegister(off, width, is_write);
  return it->second;
}
const RegisterFile::Reg& RegisterFile::find(uint32_t off, unsigned width, bool is_write) const {
  auto it = regs_.find(off & ~3u);
  if (it == regs_.end()) throw UnmodelledRegister(off, width, is_write);
  return it->second;
}

static uint32_t lane_mask(uint32_t off, unsigned width) {
  uint32_t m = width >= 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);
  return m << ((off & 3u) * 8);
}

uint32_t RegisterFile::read(uint32_t off, unsigned width) const {
  const Reg& r = find(off, width, false);
  return (r.value & lane_mask(off, width)) >> ((off & 3u) * 8);
}

void RegisterFile::write(uint32_t off, unsigned width, uint32_t v) {
  Reg& r = find(off, width, true);
  uint32_t sh = (off & 3u) * 8;
  uint32_t lm = lane_mask(off, width);
  uint32_t vs = (v << sh) & lm;
  r.value = (r.value & ~(r.rw & lm)) | (vs & r.rw);
  r.value &= ~(vs & r.w1c);
}

void RegisterFile::reset() { for (auto& kv : regs_) kv.second.value = kv.second.reset; }
uint32_t RegisterFile::get(uint32_t off) const { return find(off, 4, false).value; }
void RegisterFile::set(uint32_t off, uint32_t v) { find(off, 4, true).value = v; }

}  // namespace stest
