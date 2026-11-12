// Node lifecycle, firmware thread + baton handoff, event buffer, model registry.
// See stest/node.hpp for the threading model.
#include "stest/node.hpp"
#include <pthread.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include "stest/mmio.h"
#include "stest/runtime.hpp"

namespace stest {
namespace {

std::vector<Event> g_events;
std::map<std::string, Peripheral*, std::less<>> g_models;

// Shared between the loop thread and the firmware thread. Heap-allocated and
// co-owned by the firmware thread, so an abandoned (parked) firmware thread keeps
// waiting on a live condition variable after the Node is gone.
struct Baton {
  std::mutex m;
  std::condition_variable cv;
  bool fw_turn = false;  // true: firmware runs; false: loop runs
  bool exited = false;
  int rc = 0;

  // Firmware thread: hand the baton to the loop and wait for it to come back.
  void fw_yield() {
    std::unique_lock<std::mutex> l(m);
    fw_turn = false;
    cv.notify_all();
    cv.wait(l, [this] { return fw_turn; });
  }
  // Loop thread: hand the baton to the firmware and wait for it to come back.
  void loop_pass() {
    std::unique_lock<std::mutex> l(m);
    fw_turn = true;
    cv.notify_all();
    cv.wait(l, [this] { return !fw_turn; });
  }
};

Baton* g_baton = nullptr;  // of the live Node; read only on the firmware thread
void quantum_yield() {
  g_baton->fw_yield();
  poll_streak_break();  // the loop thread may have changed model/memory state
}

struct ThreadArg {
  std::shared_ptr<Baton> baton;
  FirmwareMain fw;
  void (*startup)(void);
};

void* firmware_thread(void* p) {
  std::unique_ptr<ThreadArg> arg(static_cast<ThreadArg*>(p));
  std::shared_ptr<Baton> b = arg->baton;
  {
    std::unique_lock<std::mutex> l(b->m);
    b->cv.wait(l, [&] { return b->fw_turn; });
  }
  if (arg->startup) arg->startup();
  int rc = arg->fw();
  {
    std::lock_guard<std::mutex> l(b->m);
    b->exited = true;
    b->rc = rc;
    b->fw_turn = false;
  }
  b->cv.notify_all();
  return nullptr;
}

bool g_node_alive = false;
}  // namespace

void emit_event(uint64_t t_ns, nlohmann::json data) { g_events.push_back({t_ns, std::move(data)}); }
std::vector<Event> take_events() {
  std::vector<Event> out;
  out.swap(g_events);
  return out;
}

void register_model(Peripheral* p) {
  std::string n(p->name());
  if (!g_models.emplace(n, p).second) throw std::invalid_argument("duplicate model name: " + n);
}
Peripheral* find_model(std::string_view name) {
  auto it = g_models.find(name);
  return it == g_models.end() ? nullptr : it->second;
}

struct Node::Impl {
  ChipDesc chip;
  std::unique_ptr<Cortexm> core;
  std::unique_ptr<ModelContext> ctx;
  std::shared_ptr<Baton> baton = std::make_shared<Baton>();
  pthread_t thread{};
  bool started = false;
  explicit Impl(const ChipDesc& c) : chip(c) {}
};

Node::Node(const ChipDesc& chip) {
  if (g_node_alive) throw std::logic_error("only one stest::Node may exist per process");
  stest_rt_init();
  impl_ = std::make_unique<Impl>(chip);
  impl_->core = std::make_unique<Cortexm>(runtime_bus(), runtime_clock(), chip.nvic_prio_bits,
                                          chip.irq_table, chip.n_irqs);
  impl_->core->install();
  g_node_alive = true;
  g_baton = impl_->baton.get();
  set_quantum_hook(&quantum_yield);
  impl_->core->set_quantum_hook(&quantum_yield);
  set_quantum_end(UINT64_MAX);
  impl_->ctx = std::make_unique<ModelContext>(runtime_clock(), *impl_->core, runtime_bus());
  if (chip.name) impl_->ctx->set_chip_name(chip.name);
  if (chip.init) chip.init(*impl_->ctx);
}

Node::~Node() {
  set_quantum_hook(nullptr);
  set_quantum_end(UINT64_MAX);
  if (impl_->started) {
    bool done;
    {
      std::lock_guard<std::mutex> l(impl_->baton->m);
      done = impl_->baton->exited;
    }
    if (done) pthread_join(impl_->thread, nullptr);
    else pthread_detach(impl_->thread);  // parked forever on its own (shared) baton
  }
  for (auto& [name, p] : g_models) runtime_bus().unmap(p);
  g_models.clear();
  g_events.clear();
  g_baton = nullptr;
  impl_->ctx.reset();
  impl_->core.reset();  // unmaps core blocks, uninstalls the core + delivery hook
  g_node_alive = false;
}

void Node::start(FirmwareMain fw) {
  if (impl_->started) throw std::logic_error("Node::start called twice");
  auto* arg = new ThreadArg{impl_->baton, fw, impl_->chip.startup};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8u << 20);  // firmware may use deep stacks
  int rc = pthread_create(&impl_->thread, &attr, &firmware_thread, arg);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    delete arg;
    throw std::runtime_error("cannot create firmware thread");
  }
  impl_->started = true;
}

Node::Stop Node::run_until(uint64_t t_ns) {
  if (!impl_->started) throw std::logic_error("Node::run_until before start");
  Baton& b = *impl_->baton;
  if (b.exited) return Stop::Exited;
  if (runtime_clock().now_ns() >= t_ns) return Stop::Quantum;
  set_quantum_end(t_ns);
  impl_->core->set_quantum_end(t_ns);
  b.loop_pass();
  return b.exited ? Stop::Exited : Stop::Quantum;
}

bool Node::exited() const { return impl_->baton->exited; }
int Node::exit_code() const { return impl_->baton->rc; }
uint64_t Node::now_ns() const { return runtime_clock().now_ns(); }
Cortexm& Node::core() { return *impl_->core; }
ModelContext& Node::models() { return *impl_->ctx; }
const ChipDesc& Node::chip() const { return impl_->chip; }

}  // namespace stest
