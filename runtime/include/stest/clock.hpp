#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>

namespace stest {

using EventId = uint64_t;

// Virtual time in nanoseconds. Deterministic: no wall clock involved.
class Clock {
 public:
  uint64_t now_ns() const { return now_; }
  void advance(uint64_t ns) { now_ += ns; }
  EventId schedule_at(uint64_t t_ns, std::function<void()> fn);
  void cancel(EventId id);                 // no-op if already fired/cancelled
  uint64_t next_event_ns() const;          // UINT64_MAX if none
  // Fires events with t <= now, ordered by (t, insertion seq). Callbacks may
  // schedule further events; those due now also fire.
  void run_due();

 private:
  uint64_t now_ = 0;
  uint64_t seq_ = 0;
  std::map<std::pair<uint64_t, uint64_t>, std::function<void()>> events_;
  std::unordered_map<EventId, uint64_t> time_of_;  // id -> t
};

}  // namespace stest
