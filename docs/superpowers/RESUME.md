# Resuming stest work

Snapshot for picking the project back up in a fresh session. First written 2026-10-31; **updated 2026-11-10** when work was paused again mid-Plan 3 (during Task 6).

## Where things stand

| Item | State |
|---|---|
| Plan 1 — core + walking skeleton | **Merged** to `main` (PR #1) |
| Plan 2 — L4 peripherals, model wiring, HAL shims | **PR #2 open**, CI green, mergeable (`plan-2-peripherals`) — waiting on the user to merge |
| Plan 3 — CAN | **In progress** on branch `plan-3-can` (stacked on `plan-2-peripherals`) — Tasks 1–5 done and reviewed, **resume at Task 6** (partial work stashed, see below) |
| Plan 4 — STM32U3C5 + FDCAN | Not written yet (outline in the Plan 1 doc's "Plan series" table) |
| Plan 5 — Sakura integration (`tests/sil/` in `../Sakura-2025-2026/firmware/Sakura`) | Not written yet |

Working tree on `plan-3-can` is clean at `cd1c6ea`. Task 6 (DBC) was stopped mid-implementation; its **unreviewed partial work is in `git stash` as `WIP plan-3 Task 6 (DBC devices)…`** (tests written and seen failing; `python/stest/can/dbc.py`, `python/tests/test_can_dbc.py`, `python/tests/data/`, cantools added to `pyproject.toml`/`uv.lock`, edits to `can/__init__.py`, `can/bus.py`, `stest/__init__.py`). On resume either `git stash pop` and hand it to a fresh Task 6 implementer to finish (it must still go through review), or `git stash drop` and redo Task 6 from the brief. `plan-3-can` is **not pushed** yet.

## Read these first

1. `AGENTS.md` — repo map, commands (native + Docker exactly as CI), non-negotiable rules, gotchas.
2. `docs/superpowers/specs/2026-10-30-stest-sil-design.md` — the binding design.
3. `docs/superpowers/plans/2026-10-31-stest-plan-3-can.md` — the current plan (8 tasks).
4. `docs/adding-a-peripheral.md`, `docs/adding-a-chip.md` — how models/chips are built (incl. `deliver()`, level IRQs, clock-change signal).
5. Follow-ups from earlier plans: `docs/superpowers/plans/2026-10-30-plan-1-followups.md`, `docs/superpowers/plans/2026-10-31-plan-2-followups.md`.

## How the work is being run

Plans are executed with the **superpowers:subagent-driven-development** skill: one fresh implementer subagent per task, a task review (spec + quality) after each, fix rounds via the same implementer, then a whole-branch review on the most capable model, one fix wave, and a PR. The user chose this method for Plans 1–3.

The skill's workspace for Plan 3 is **`.superpowers/sdd/2026-10-31-stest-plan-3-can/`** (git-ignored, local to this machine):
- `progress.md` — the ledger. Its rulings, carries and deferred minors are authoritative; tasks with `Task N: complete` lines are done — do not re-dispatch them.
- `task-N-brief.md` / `task-N-report.md` — briefs and implementer reports (Task 1–3 reports document the APIs later tasks consume).
- `task-6-brief.md` — already extracted; reuse it.

To resume: invoke `superpowers:subagent-driven-development` with the Plan 3 file; it will find the ledger and continue at Task 6. Decide what to do with the Task 6 stash (above), then dispatch Task 6 with the context below.

If the ledger is missing (different machine / cleaned), everything important is reproduced below.

## Plan 3 progress

| Task | Commits | Notes |
|---|---|---|
| 1 Protocol v2 | `f8efe32`, `7a87201`, `75bc918` | RUN_UNTIL carries a timestamped inbox → `Peripheral::deliver(op, args, t_ns)`; ERROR carries the aborting quantum's events. Runtime now uses **discrete-event clock semantics** (`Clock::advance_to`): nodes stop at **exactly** the quantum end, every event ≤ T fires before STOPPED, callbacks see `now == their t`. `Gpio::deliver("drive_pin", {pin, level\|null})`, `Node.post`, `Pin.drive_at`. |
| 2 CAN frame model | `f68e8ac` | `python/stest/can/frame.py`, `log.py`: bit-exact length (stuffing + CRC-15), `lookahead_ns(500k) = 88 000`, arbitration key, candump/ASC. |
| 3 Bus engine | `ca488f7`, `a051c6d` | `python/stest/can/bus.py`, `device.py`, Sim wiring (`sim.can_bus`, quantum auto-capped to the lookahead). Fuzzed 100+ seeds × 7 quanta, no causality/determinism failures. |
| 4 bxCAN model | `71b2a72`, `0232bb9` | `chips/stm32l4/models/bxcan.{hpp,cpp}` (CAN1/CAN2, filters in CAN1's block, 3 mailboxes, 2×3 FIFOs, IRQs). Header documents event/deliver contract and known deviations (frame in flight dropped on INRQ; ABRQ of withdrawn mailbox completes at engine result; MCR.RESET scope; FMI-from-CAN2SB and no-RX-in-FINIT interpretations). `can_tx` also carries `nart`; leaving normal withdraws in-flight mailboxes via `can_tx_abort`. TXRQ clears RQCP/TXOK/ALST/TERR. |
| 5 Firmware e2e | `b9d8f0e`, `cd1c6ea` | `sim.node(..., can={"CAN2": bus})` wiring in `Node._can_flush`; bit-rate mismatch → NodeCrashed; unwired TX → NodeCrashed; `can=` names validated via the bxCAN `can_info` op. Firmware `selftest/firmware/can` (`can_node1`, `can_node2`, `can_badrate`), 10 e2e tests. Speed: 2 nodes × 2 virtual s ≈ 0.74 s native, ~0.84–0.91 s Docker (budget 1.6 s). |
| 6 DBC | — (WIP stash) | **Next.** Brief at `.superpowers/sdd/…/task-6-brief.md`. |
| 7–8 | — | error states/fault injection; logs/docs/flake/speed gate |

### Decisions already made (carry these into Task 4+)

- **Engine ↔ node API** (`python/stest/can/bus.py`): `_request(sender, key, frame, t_req, txfp, seq)`, `_abort(sender, key, t)`, `_set_node_mode(sender, mode, bitrate, t)`, per-sender hooks `on_rx(lf, t)` / `on_tx_result(key, result, t)`; results carry `seq`, and `ok` / `arb_lost` / `aborted` / `error`.
- **bxCAN mode event**: `{"kind":"can_mode","periph":"CANn","mode":"sleep|init|normal","bitrate":<int|null>}` on every mode change (bitrate when entering normal). Engine ACKs/delivers only to nodes in normal mode at SOF. Task 5's bit-rate-mismatch check uses this event.
- **TX requests** are emitted only in normal mode (TXRQ in init/sleep held until entering normal — verify vs RM0351). Event `{"kind":"can_tx","periph","mailbox","id","ext","rtr","dlc","data":"hex","txfp","seq"}`.
- **Abort**: `can_tx_abort` event with seq; an effective abort is acknowledged at `max(t_abort + lookahead, EOF of the request's last failed attempt)` (hardware is immediate — documented deviation); abort after SOF has no effect. Mailbox stays busy (TME=0) until the result; no reload while busy; stale results (seq mismatch) ignored.
- **Arbitration-lost flag** is observable at the winning frame's EOF (documented deviation).
- TEC/REC fault confinement is **Task 7**; Task 4 only sets TERR/LEC on ACK errors.
- Inbox messages a node can't apply are a strict abort (harness bug).
- Device `every()` requests are stamped at their due time, not the boundary.

### Open items to handle later in Plan 3

- Plan 4 carry: the valid-CAN-controller hint probes fixed names (CAN1–3, FDCAN1–3) — replace with model enumeration; the FDCAN model must implement the `can_info` op.
- Deferred minors from Tasks 4–5 (one node on two buses untested e2e; exact bit-rate comparison; AWUM/WKUIE abort at configure time; etc.) are in the ledger for the final review.

- Task 5: make the two-node quantum-independence test meaningful (frames/drives in (T−25 ns, T] on a 7 µs grid).
- Task 8: `spike/run_gate.py` hard-codes `build-gate/` (Docker run collides with the macOS build) — add `--build-dir`; reproduce/fix the `python_tests` flake under `ctest -j8`; re-measure speed at the 88 µs CAN quantum on x86 CI. **Speed watch:** the 88 µs cap costs ~0.22 s wall per virtual s per node natively (budget 0.4); if x86 exceeds the budget, stop and ask the user (shared-memory sync is the fallback).
- Final-wave candidate: `__WFI` returns at every quantum end (spurious wake-ups vs hardware) — should keep sleeping until an event/IRQ.
- Deferred minors from Tasks 1–3 are in the ledger; the final whole-branch review triages them.

## Practical notes

- Commands: see AGENTS.md. Docker image tag used locally is `stest-dev` (CI builds `stest-ci` from the same `docker/`).
- Commit trailer used throughout: the harness-instructed `Co-Authored-By:` line.
- The user has a native Linux box (`ssh nikhil@10.1.0.0`) but it has no toolchain installed; x86 numbers come from CI.
- Sakura firmware (`../Sakura-2025-2026/firmware/Sakura`) is read-only for stest work until Plan 5. One finding was reported to the user: `boards/steering_wheel/Core/Src/main.c` comments ADC2 as "CH14/PB11", but PB11 has no analog function on the L496 (CH14 is PC5).
