#include "stest/bus.hpp"
#include <algorithm>
#include <cstdio>
#include "stest/mmio_windows.hpp"

namespace stest {

void Bus::map(uint64_t base, uint32_t size, Peripheral* p) {
  if (size == 0 || !p) throw std::invalid_argument("bus map: empty range or null peripheral");
  auto inside = [&](uint64_t wb, uint64_t ws) { return base >= wb && base + size <= wb + ws; };
  if (!inside(kPeriphBase, kPeriphSize) && !inside(kCoreBase, kCoreSize))
    throw std::invalid_argument("bus map: range outside the MMIO windows");
  for (const auto& m : maps_)
    if (base < m.base + m.size && m.base < base + size)
      throw std::invalid_argument("bus map: overlapping range");
  auto g = gates_.find(p);
  auto h = hal_.find(p);
  maps_.push_back({base, size, p, g == gates_.end() ? nullptr : g->second.get(),
                   h == hal_.end() ? nullptr : h->second.get()});
  std::sort(maps_.begin(), maps_.end(),
            [](const Mapping& a, const Mapping& b) { return a.base < b.base; });
}

const Bus::Mapping* Bus::find(uint64_t addr, unsigned width) const {
  auto it = std::upper_bound(maps_.begin(), maps_.end(), addr,
                             [](uint64_t a, const Mapping& m) { return a < m.base; });
  if (it == maps_.begin()) return nullptr;
  --it;
  if (addr + width > it->base + it->size) return nullptr;
  return &*it;
}

void Bus::set_gate(Peripheral* p, std::function<bool()> clocked, std::string what) {
  if (!clocked) throw std::invalid_argument("bus set_gate: empty predicate");
  install_gate(p, Gate{nullptr, 0, std::move(clocked), std::move(what)});
}

void Bus::set_gate(Peripheral* p, const uint32_t* word, uint32_t mask, std::string what) {
  if (!word || !mask) throw std::invalid_argument("bus set_gate: null word or empty mask");
  install_gate(p, Gate{word, mask, {}, std::move(what)});
}

void Bus::install_gate(Peripheral* p, Gate gate) {
  bool mapped = false;
  for (const auto& m : maps_) mapped |= m.p == p;
  if (!mapped) throw std::invalid_argument("bus set_gate: peripheral not mapped");
  auto& g = gates_[p];
  g = std::make_unique<Gate>(std::move(gate));
  for (auto& m : maps_)
    if (m.p == p) m.gate = g.get();
}

void Bus::set_hal_owned(Peripheral* p, std::string type) {
  bool mapped = false;
  for (const auto& m : maps_) mapped |= m.p == p;
  if (!mapped) throw std::invalid_argument("bus set_hal_owned: peripheral not mapped");
  auto& h = hal_[p];
  h = std::make_unique<HalOwned>(HalOwned{std::move(type), {}});
  for (auto& m : maps_)
    if (m.p == p) m.hal = h.get();
}

// Slow path, hal-owned peripherals only: one warning per register word.
void Bus::hal_access(const Mapping& m, uint64_t addr) {
  const uint32_t off = static_cast<uint32_t>(addr - m.base) & ~3u;
  if (!m.hal->warned.insert(off).second) return;
  std::string reg(m.p->reg_name(off));
  if (reg.empty()) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "+0x%03x", static_cast<unsigned>(off));
    reg = buf;
  }
  std::fprintf(stderr, "stest: warning: firmware accessed %s %s directly while %s is in hal mode\n",
               std::string(m.p->name()).c_str(), reg.c_str(), m.hal->type.c_str());
  std::fflush(stderr);
}

Peripheral* Bus::require_clock(uint64_t addr, unsigned width, bool is_write) const {
  const Mapping* m = find(addr, width);
  if (!m) throw UnmodelledAccess(addr, width, is_write);
  if (m->gate && !m->gate->open()) gated(*m, addr, width, is_write);
  return m->p;
}

void Bus::gated(const Mapping& m, uint64_t addr, unsigned width, bool is_write) {
  char tail[48];
  std::snprintf(tail, sizeof tail, " (%s%u 0x%08x)", is_write ? "write" : "read", width * 8,
                static_cast<unsigned>(addr));
  throw ClockDisabled("access to " + std::string(m.p->name()) + " while its clock is disabled (" +
                          m.gate->what + ")" + tail,
                      addr, width, is_write);
}

uint32_t Bus::read(uint64_t addr, unsigned width) {
  const Mapping* m = find(addr, width);
  if (!m) throw UnmodelledAccess(addr, width, false);
  if (__builtin_expect(m->gate != nullptr, 0) && !m->gate->open()) gated(*m, addr, width, false);
  if (__builtin_expect(m->hal != nullptr, 0)) hal_access(*m, addr);
  try {
    return m->p->read(static_cast<uint32_t>(addr - m->base), width);
  } catch (const UnmodelledRegister&) {
    throw UnmodelledAccess(addr, width, false, std::string(m->p->name()));
  }
}

void Bus::write(uint64_t addr, unsigned width, uint32_t v) {
  const Mapping* m = find(addr, width);
  if (!m) throw UnmodelledAccess(addr, width, true);
  if (__builtin_expect(m->gate != nullptr, 0) && !m->gate->open()) gated(*m, addr, width, true);
  if (__builtin_expect(m->hal != nullptr, 0)) hal_access(*m, addr);
  try {
    m->p->write(static_cast<uint32_t>(addr - m->base), width, v);
  } catch (const ReadOnlyRegister&) {
    throw UnmodelledAccess(addr, width, true, std::string(m->p->name()), /*ro=*/true);
  } catch (const UnmodelledRegister&) {
    throw UnmodelledAccess(addr, width, true, std::string(m->p->name()));
  }
}

void Bus::unmap(Peripheral* p) {
  maps_.erase(std::remove_if(maps_.begin(), maps_.end(), [p](const Mapping& m) { return m.p == p; }),
              maps_.end());
  gates_.erase(p);
  hal_.erase(p);
}

void Bus::reset_all() { for (auto& m : maps_) m.p->reset(); }

}  // namespace stest
