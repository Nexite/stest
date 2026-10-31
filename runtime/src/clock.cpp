#include "stest/clock.hpp"
#include <limits>

namespace stest {

EventId Clock::schedule_at(uint64_t t_ns, std::function<void()> fn) {
  EventId id = seq_++;
  events_.emplace(std::make_pair(t_ns, id), std::move(fn));
  time_of_[id] = t_ns;
  return id;
}

void Clock::cancel(EventId id) {
  auto it = time_of_.find(id);
  if (it == time_of_.end()) return;
  events_.erase({it->second, id});
  time_of_.erase(it);
}

void Clock::fire_until(uint64_t target) {
  while (!events_.empty() && events_.begin()->first.first <= target) {
    auto it = events_.begin();
    const uint64_t t = it->first.first;
    auto fn = std::move(it->second);
    time_of_.erase(it->first.second);
    events_.erase(it);
    if (t > now_) now_ = t;
    fn();
  }
  if (target > now_) now_ = target;
}

}  // namespace stest
