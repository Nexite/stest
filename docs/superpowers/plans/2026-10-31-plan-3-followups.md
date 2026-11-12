# Plan 3 follow-ups

Decisions made during Plan 3 (CAN) execution and deferred findings, for Plan 4+.

## CAN sync speed (x86 gate failed; planned fix, not scheduled yet)

PR #3's CI (run 43167244660) measured the 88 us CAN quantum on x86-64 (numbers in
`docs/spikes/2026-10-30-interception-gate.md`, "x86-64 result"): single node 0.22 (PASS, budget
0.40), **two nodes on one bus 1.13 for the pair (FAIL, budget 0.80)**, no-ACK worst case 1.18.
A two-board CAN test therefore runs slower than real time on x86, which misses the requirement
that the first user project's SIL suite run at least as fast as real time. The user deferred
the fix (decide before or inside Plan 4).

Planned fix, in order:
1. **Profile first.** The socket round-trip benchmark (`sync_s`, 0.22 per node per virtual s
   at 88 us) explains less than half of the two-node figure (~0.56 per node). Profile the
   two-node gate (`spike/gate_can_two_node.py`) on x86 (CI or a Linux x86 box) with py-spy or
   cProfile and split the time into: socket send/recv and wakeups, protocol encode/decode
   (`protocol.py`), `Sim` lockstep loop and `Node` bookkeeping (`sim.py`, `node.py`), bus
   engine work per quantum (`can/bus.py`), and native firmware time.
2. **Shared-memory handoff (the fallback the plan names).** Replace the per-quantum socket
   round trip (RUN_UNTIL -> STOPPED) with a shared-memory control block per node plus a futex
   (Linux) / `os_sync_wait_on_address` or `__ulock_wait` (macOS) wake-up; keep the socket for
   HELLO, READ_MEM/WRITE_MEM, MODEL_CALL and errors. Inbox and events move through a
   shared-memory ring. Protocol version bump; byte-identical event logs required
   (determinism tests at several quanta must stay green).
3. **Cut per-quantum Python work.** Skip the engine and inbox processing for quanta with no
   bus activity (fast path when no node reported a CAN event and no device is due), avoid
   per-quantum allocations in `Sim`/`Node`, and issue RUN_UNTIL to all nodes before waiting on
   any (check the loop already overlaps nodes).
4. **If still over budget:** move the lockstep loop and the bus engine's hot path to C++
   (a `_stest` extension or a coordinator process), keeping the Python API.

Acceptance: `spike/run_gate.py --quantum-us 88 --can-two-node` <= 0.80 on CI x86-64, standard
gate unchanged or better, all determinism tests green, and `test_can_speed` with
`STEST_PERF_STRICT=1` passing on x86.

## Carry into Plan 4
- Valid-controller hint: the "no such CAN controller" message probes fixed names (CAN1-3, FDCAN1-3); replace with an enumeration of the node's models that answer `can_info`.
- The FDCAN model must implement the `can_info` call (`{"type": ..., "role": ...}`) so the bus wiring and the hint find it.

## High priority
- No e2e test of one node on two buses (spec section 4.6: dual-controller boards, gateways); needs a firmware variant with CAN1 and CAN2 both wired.
- bxCAN MCR.AWUM / IER.WKUIE abort at configure time: a CubeMX project with AutoWakeUp enabled aborts at init even if it never sleeps. Fix before Plan 5 (model the bit as accepted while the controller never sleeps on bus activity, or model wake-up).

## Deferred findings (final review triage)
- Engine/wiring errors (`can/bus.py` request/abort/result paths, `Node._can_flush`) raise a plain `StestError` that bypasses `Sim._fail`: the nodes are not killed and the Sim is not marked dead.
- `Bxcan::reset()` / `Gpio::reset()` do not cancel scheduled `deliver` events.
- `_apply_aborts` uses `list.pop(0)` / `insort` (O(n) per operation).
- `CanBus.errors` and `bus.log` are unbounded (about 1.4 MB per virtual second in the no-ACK worst case). Idea: collapse consecutive identical ACK errors with a count, or a `max_errors` ring; keep the log whole for export.
- Lenient `put_events` drop can lose an oversized partial line (Task 1).
- `frame.py`: verify a golden `bit_length() == 50` test and external CRC / bit-stuffing vectors exist (Task 2).
- bxCAN (Task 4): document that INRQ during reception drops the in-flight frame, and that ABRQ of a withdrawn mailbox completes at the engine result. Verify against RM0351: MCR.RESET scope, bit-0 handling in 32-bit list vs mask mode, FMI numbering from CAN2SB, no RX while FINIT, init/sleep keep TEC/REC, mailboxes pending across bus-off.
- Self-test firmware `Error_Handler` is a silent WFI loop (an init failure looks like a hang).
- Task 6 tests: "contend ... arbitration by id" wording (it is one sender's queue order); a duplicate assert; `test.dbc` has 5 messages where the text says 3; no true cross-match negative test (a standard-only id queried as extended).
- Task 7: no test of a bus-off node as the only possible ACKer; `_inject` entries whose target never sends never expire (document).
- Task 8: the perf-isolation AST guard misses from-imports, helpers, class and async tests; the CTest check errors instead of skipping outside a CTest build; a relative `--stest-can-log DIR` is resolved at teardown cwd (resolve in `pytest_configure`); no two-bus failure-section order test; `--stest-can-log` file names collide across modules (use nodeid-derived names); no tests for the gate scripts.
- WFE models the ARMv7-M event register (`__SEV`, exception entry/return, SCR.SEVONPEND) but no other event sources (external event input, multi-core SEV, debug events).
- WFE with PRIMASK set and SEVONPEND clear wakes on a pending enabled IRQ (`Impl::sleep` uses `select(false)` for WFE too). ARMv7-M's "ignoring PRIMASK" wake rule is WFI-only; hardware WFE keeps sleeping unless SEVONPEND is set. Permitted as a spurious wake and deterministic, but the test/comments present it as the architectural rule: use `select(true)` for WFE or document it as spurious.
- `clear_pend` (ICPR write while a level line stays asserted) does `set_pend(false)` then `set_pend(true)`, so with SEVONPEND set it raises an event although the IRQ never stopped pending. Harmless edge case.

## Rulings (final review; all judged to hold)
- Engine <-> node API: `_request` / `_abort` / `_set_node_mode`, `on_rx` / `on_tx_result`, results bound to a request sequence number.
- `can_mode` event carries the bitrate; ACK and delivery only to nodes in normal mode at SOF.
- Inbox messages a node cannot apply are a strict abort.
- Discrete-event `Clock::advance_to`; nodes stop at exactly the quantum end.
- Device `every()` / `send()` requests are stamped at the callback's due time.
- ABRQ is acknowledged at max(t_abort + lookahead, EOF of the last failed attempt): documented deviation of at most 44 bit times.
- Unwired CAN TX is a strict abort.
- cantools / python-can (LGPL-3.0) accepted as a dependency, mitigated: lazy import and the optional extra `stest[dbc]` (user may veto).
- The bus engine is the single owner of TEC/REC (absolute values plus an epoch delivered to bxCAN).
- Bus-off recovery is a timed deliver op (rejoin one lookahead after 128 x 11 recessive bits).
- `inject_errors` applies to frames with SOF >= call time; `target` filters by sender.
- Python `CanDevice`s are always error active.
- Same-ID different-data contention is a bit error.
- REC resets to 120 per RM0351 (overrides the plan's 127).
- ESR TEC shows the low byte of the 9-bit counter.
- x86 gate numbers come from CI (the Linux box has no toolchain and is not to be modified).
- WFI sleeps across quantum ends until an exception is pending or taken (final fix wave); WFE models the event register so ST's `__SEV(); __WFE(); __WFE();` sleep entry wakes on one interrupt (re-review fix).
- `docs/superpowers/RESUME.md` removed from the branch (stale, named first-user-project paths).
- `test_can_speed`'s 1.6 s wall budget is opt-in (`STEST_PERF_STRICT=1`); the gated CAN speed figure is `spike/run_gate.py --can-two-node` (final fix wave).
