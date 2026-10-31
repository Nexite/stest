#include "stest/model_context.hpp"
#include <algorithm>
#include <cxxabi.h>
#include <cstdlib>
#include <memory>
#include "stest/cortexm.hpp"

namespace stest {

std::string demangled_type_name(const std::type_info& t) {
  int status = 0;
  std::unique_ptr<char, void (*)(void*)> s(abi::__cxa_demangle(t.name(), nullptr, nullptr, &status),
                                           std::free);
  return status == 0 && s ? std::string(s.get()) : std::string(t.name());
}

void ModelContext::add(Peripheral* p) {
  if (!p) throw std::invalid_argument("model context: null model");
  std::string n(p->name());
  if (!by_name_.emplace(n, p).second)
    throw std::invalid_argument("model context: duplicate model name: " + n);
  order_.push_back(p);
}

Peripheral* ModelContext::find(std::string_view name) const {
  auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : it->second;
}

ClockTree& ModelContext::clocks() const {
  if (!tree_) throw std::logic_error("model context: no clock tree registered (set_clock_tree)");
  return *tree_;
}

void ModelContext::set_irq_source(int irqn, const void* source, bool high) {
  std::vector<const void*>& v = irq_high_[irqn];
  auto it = std::find(v.begin(), v.end(), source);
  bool was = !v.empty();
  if (high && it == v.end())
    v.push_back(source);
  else if (!high && it != v.end())
    v.erase(it);
  else
    return;
  bool now = !v.empty();
  if (now != was) core_.set_irq_level(irqn, now);
}

}  // namespace stest
