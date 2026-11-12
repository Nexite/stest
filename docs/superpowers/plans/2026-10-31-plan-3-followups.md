# Plan 3 follow-ups

Decisions made during Plan 3 (CAN) execution and deferred findings, for Plan 4+.

## Carry into Plan 4
- Valid-controller hint: the "no such CAN controller" message probes fixed names (CAN1-3, FDCAN1-3); replace with an enumeration of the node's models that answer `can_info`.
- The FDCAN model must implement the `can_info` call (`{"type": ..., "role": ...}`) so the bus wiring and the hint find it.
- Speed gate: the Linux x86-64 row for the CAN quantum (88 us, plus the gated `--can-two-node` row) is pending the first CI run of this branch (artifact `gate-can-linux-x86_64`); record it in `docs/spikes/2026-10-30-interception-gate.md` and apply the plan's rule: above 0.40 (0.80 for the two-node row), stop and ask the user (sync cost reduction: shared memory + futex / `os_sync_wait_on_address`).

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
