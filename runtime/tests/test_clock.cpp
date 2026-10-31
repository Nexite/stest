#include <doctest/doctest.h>
#include <cstdint>
#include <vector>
#include "stest/clock.hpp"
using namespace stest;

TEST_CASE("clock: empty has no next event") {
  Clock c;
  CHECK(c.next_event_ns() == UINT64_MAX);
  CHECK(c.now_ns() == 0);
}
TEST_CASE("clock: same-time events fire in insertion order") {
  Clock c; std::vector<int> o;
  c.schedule_at(100, [&] { o.push_back(1); });
  c.schedule_at(100, [&] { o.push_back(2); });
  c.schedule_at(50, [&] { o.push_back(0); });
  CHECK(c.next_event_ns() == 50);
  c.advance(99); c.run_due();
  CHECK(o == std::vector<int>{0});
  c.advance(1); c.run_due();
  CHECK(o == std::vector<int>{0, 1, 2});
  CHECK(c.next_event_ns() == UINT64_MAX);
}
TEST_CASE("clock: cancel and chained scheduling") {
  Clock c; std::vector<int> o;
  auto id = c.schedule_at(10, [&] { o.push_back(9); });
  c.schedule_at(10, [&] { o.push_back(1); c.schedule_at(10, [&] { o.push_back(2); }); });
  c.cancel(id); c.cancel(id);
  c.advance(10); c.run_due();
  CHECK(o == std::vector<int>{1, 2});
}
TEST_CASE("clock: advance_to fires each event at its own time, then reaches the target") {
  Clock c;
  std::vector<std::pair<int, uint64_t>> o;  // (id, now seen by the callback)
  c.schedule_at(30, [&] { o.push_back({3, c.now_ns()}); });
  c.schedule_at(10, [&] {
    o.push_back({1, c.now_ns()});
    c.schedule_at(20, [&] { o.push_back({2, c.now_ns()}); });  // within the target: fires in order
    c.schedule_at(5, [&] { o.push_back({0, c.now_ns()}); });   // in the past: fires at now (10)
    c.schedule_at(100, [&] { o.push_back({9, c.now_ns()}); }); // beyond the target: stays
  });
  c.advance_to(50);
  CHECK(o == std::vector<std::pair<int, uint64_t>>{{1, 10}, {0, 10}, {2, 20}, {3, 30}});
  CHECK(c.now_ns() == 50);
  CHECK(c.next_event_ns() == 100);
  c.advance_to(40);  // never backwards
  CHECK(c.now_ns() == 50);
  c.advance(50);  // advance(ns) == advance_to(now + ns): fires the event at 100 too
  CHECK(o.back() == std::pair<int, uint64_t>{9, 100});
  CHECK(c.now_ns() == 100);
}
TEST_CASE("clock: run_due fires events scheduled at now without moving time") {
  Clock c;
  c.advance_to(70);
  uint64_t seen = 0;
  c.schedule_at(70, [&] { seen = c.now_ns(); });
  c.run_due();
  CHECK(seen == 70);
  CHECK(c.now_ns() == 70);
}
