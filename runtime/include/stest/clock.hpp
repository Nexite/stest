#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>

namespace stest {

using EventId = uint64_t;

// Virtual time in nanoseconds. Deterministic: no wall clock involved.
//
// Discrete-event semantics: time moves only through advance_to()/advance(), which fire
// every event up to the target in (t, insertion seq) order, setting now to each event's
// time as it fires (never backwards: an event scheduled in the past fires at the current
// now). A callback therefore observes now == its scheduled time, however far the clock is
// being moved, and events it schedules at <= the target fire in order in the same call.
class Clock {
 public:
  uint64_t now_ns() const { return now_; }
  // Fire every event with t <= target as described above, then now = max(now, target).
  // Exceptions from callbacks propagate (the event that threw is already removed).
  void advance_to(uint64_t target) {
    if (next_event_ns() > target) {
      if (target > now_) now_ = target;
      return;
    }
    fire_until(target);
  }
  void advance(uint64_t ns) { advance_to(now_ + ns); }
  EventId schedule_at(uint64_t t_ns, std::function<void()> fn);
  void cancel(EventId id);                 // no-op if already fired/cancelled
  uint64_t next_event_ns() const {         // UINT64_MAX if none
    return events_.empty() ? UINT64_MAX : events_.begin()->first.first;
  }
  // Fires events with t <= now (advance_to(now)): e.g. ones just scheduled at now.
  void run_due() { advance_to(now_); }

 private:
  void fire_until(uint64_t target);
  uint64_t now_ = 0;
  uint64_t seq_ = 0;
  std::map<std::pair<uint64_t, uint64_t>, std::function<void()>> events_;
  std::unordered_map<EventId, uint64_t> time_of_;  // id -> t
};

}  // namespace stest
