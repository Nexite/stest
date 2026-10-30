# Interception speed gate (Plan 1, Task 2)

**Question:** can compile-time MMIO interception plus parent/child time sync stay within
the per-node gating budget of **0.40 wall seconds per virtual second** under a
lighting-like load (20 kHz software-PWM ISR + 1 kHz main-loop tick)?

**Gate rule:** `total = median(mmio_s) + median(sync_s) <= 0.40` -> PASS.

## What is measured
- `spike/bench_lighting_load.c` (built with the `stest_mmio_pass`, -O2, Release): 1 virtual
  second = 20,000 ISR invocations x 8 volatile peripheral accesses (TIM SR clear, GPIO BSRR x4,
  3 reads), 1,000 main-loop ticks x 50 accesses, 1,000 `uwTick` increments. About 232k
  intercepted accesses (the model's virtual clock ends at 5.8 ms at 25 ns/access).
- `spike/bench_mmio_impl.c` (separate TU, noinline, built without the pass): intended fast
  path. Out-of-window -> direct host access + `now_ns += 25`. In-window
  (`[0x40000000,0x60000000)`, `[0xE0000000,0xE0100000)`) -> 64-entry page table -> indirect call
  through a function pointer into an array-backed register store + `now_ns += 25`.
- `spike/bench_sync.cpp`: parent/child over `socketpair(AF_UNIX, SOCK_STREAM)`, 10,000 round
  trips of a 16-byte RUN_UNTIL / 16-byte STOPPED exchange (100 us quantum, the CAN worst case).
- `spike/run_gate.py`: Release build, 5 runs each, medians. Run with `uv run spike/run_gate.py`.

## Results

### macOS arm64 (Apple M3 Pro, macOS 27.0.1, Homebrew LLVM/clang 23.1.4)
| | runs (s) | median |
|---|---|---|
| mmio_s | 0.001191 0.000964 0.000851 0.000755 0.000678 | 0.000851 |
| sync_s | 0.078028 0.060411 0.052771 0.058782 0.060693 | 0.060411 |

total = 0.061262 s -> **PASS** (budget 0.40)

### Linux arm64 (Docker Desktop VM on the same M3 Pro, 12 vCPU, Ubuntu 24.04, clang 18.1.3)
| | runs (s) | median |
|---|---|---|
| mmio_s | 0.000432 0.000394 0.000453 0.000403 0.000393 | 0.000403 |
| sync_s | 0.091277 0.088855 0.089541 0.088412 0.092772 | 0.089541 |

total = 0.089944 s -> **PASS** (budget 0.40)

### Linux x86-64
Pending (CI, Task 8). Not run under emulation.

## Verdict
macOS arm64: PASS (0.061 of 0.40 s). Linux arm64: PASS (0.090 of 0.40 s).
Sync dominates by two orders of magnitude (~99%): MMIO interception costs ~2 ns per access
(under 1 ms per virtual second); the socket round trips cost 6-9 us each. Gate result: continue.

## Caveats
- The model is a trivial register store; real peripheral models (timer/GPIO side effects, IRQ
  evaluation) will cost more per access. Headroom is ~400x on MMIO, so this is not expected to
  flip the verdict, but the figure is a lower bound.
- Sync here is one node. Multi-node gating multiplies the sync term by node count; at 90 us /
  virtual-second per node the 0.40 budget leaves room for the 100 us quantum on several nodes,
  and shared-memory + futex / `os_sync_wait_on_address` remains the recorded remedy if needed.
- Docker Desktop on macOS runs Linux in a VM; socket timings there include VM scheduling.
