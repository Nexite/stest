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
