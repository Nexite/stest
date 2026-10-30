#include "stest/bus.hpp"
#include <algorithm>

namespace stest {

void Bus::map(uint64_t base, uint32_t size, Peripheral* p) {
  if (size == 0 || !p) throw std::invalid_argument("bus map: empty range or null peripheral");
  for (const auto& m : maps_)
    if (base < m.base + m.size && m.base < base + size)
      throw std::invalid_argument("bus map: overlapping range");
  maps_.push_back({base, size, p});
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

uint32_t Bus::read(uint64_t addr, unsigned width) {
  const Mapping* m = find(addr, width);
  if (!m) throw UnmodelledAccess(addr, width, false);
  try {
    return m->p->read(static_cast<uint32_t>(addr - m->base), width);
  } catch (const UnmodelledRegister&) {
    throw UnmodelledAccess(addr, width, false, std::string(m->p->name()));
  }
}

void Bus::write(uint64_t addr, unsigned width, uint32_t v) {
  const Mapping* m = find(addr, width);
  if (!m) throw UnmodelledAccess(addr, width, true);
  try {
    m->p->write(static_cast<uint32_t>(addr - m->base), width, v);
  } catch (const UnmodelledRegister&) {
    throw UnmodelledAccess(addr, width, true, std::string(m->p->name()));
  }
}

void Bus::reset_all() { for (auto& m : maps_) m.p->reset(); }

}  // namespace stest
