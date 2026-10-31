#include "stest/peripheral.hpp"

namespace stest {

nlohmann::json Peripheral::call(std::string_view op, const nlohmann::json&) {
  throw UnknownOp(op);
}

static void check_declare(uint32_t off) {
  if ((off & 3u) != 0) throw std::invalid_argument("RegisterFile: register offset not word aligned");
  if (off >= RegisterFile::kMaxBlock)
    throw std::invalid_argument("RegisterFile: register offset beyond kMaxBlock");
}

void RegisterFile::declare(uint32_t off, uint32_t reset, uint32_t rw, uint32_t w1c) {
  check_declare(off);
  regs_[off >> 2] = Reg{reset, reset, rw, w1c, true, false};
}

void RegisterFile::declare_write_only(uint32_t off, uint32_t reset) {
  check_declare(off);
  regs_[off >> 2] = Reg{reset, reset, 0, 0, true, true};
}

RegisterFile::Reg& RegisterFile::find(uint32_t off, unsigned width, bool is_write) {
  if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, is_write);  // crosses registers
  if (off >= kMaxBlock || !regs_[off >> 2].declared) throw UnmodelledRegister(off, width, is_write);
  return regs_[off >> 2];
}
const RegisterFile::Reg& RegisterFile::find(uint32_t off, unsigned width, bool is_write) const {
  if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, is_write);  // crosses registers
  if (off >= kMaxBlock || !regs_[off >> 2].declared) throw UnmodelledRegister(off, width, is_write);
  return regs_[off >> 2];
}

uint32_t RegisterFile::read(uint32_t off, unsigned width) const {
  const Reg& r = find(off, width, false);
  return lane_extract(r.value, off, width);
}

void RegisterFile::write(uint32_t off, unsigned width, uint32_t v) {
  Reg& r = find(off, width, true);
  if (r.rw == 0 && r.w1c == 0) {
    if (r.write_only) return;
    throw ReadOnlyRegister(off, width);
  }
  uint32_t sh = (off & 3u) * 8;
  uint32_t lm = lane_mask(off, width);
  uint32_t vs = (v << sh) & lm;
  r.value = (r.value & ~(r.rw & lm)) | (vs & r.rw);
  r.value &= ~(vs & r.w1c);
}

void RegisterFile::reset() { for (auto& r : regs_) r.value = r.reset; }
uint32_t RegisterFile::get(uint32_t off) const { return find(off, 4, false).value; }
void RegisterFile::set(uint32_t off, uint32_t v) { find(off, 4, true).value = v; }
const uint32_t* RegisterFile::value_ptr(uint32_t off) const { return &find(off, 4, false).value; }

}  // namespace stest
