#include "stest/bus.hpp"
#include <algorithm>
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
  maps_.push_back({base, size, p, g == gates_.end() ? nullptr : g->second.get()});
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

void Bus::gated(const Mapping& m, uint64_t addr, unsigned width, bool is_write) {
  throw ClockDisabled("access to " + std::string(m.p->name()) + " while its clock is disabled (" +
                          m.gate->what + ")",
                      addr, width, is_write);
}

uint32_t Bus::read(uint64_t addr, unsigned width) {
  const Mapping* m = find(addr, width);
  if (!m) throw UnmodelledAccess(addr, width, false);
  if (__builtin_expect(m->gate != nullptr, 0) && !m->gate->open()) gated(*m, addr, width, false);
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
}

void Bus::reset_all() { for (auto& m : maps_) m.p->reset(); }

}  // namespace stest
