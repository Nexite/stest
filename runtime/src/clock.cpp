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

uint64_t Clock::next_event_ns() const {
  return events_.empty() ? std::numeric_limits<uint64_t>::max() : events_.begin()->first.first;
}

void Clock::run_due() {
  while (!events_.empty() && events_.begin()->first.first <= now_) {
    auto it = events_.begin();
    auto fn = std::move(it->second);
    time_of_.erase(it->first.second);
    events_.erase(it);
    fn();
  }
}

}  // namespace stest
