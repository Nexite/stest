# Plan 1 follow-ups

Carried out of Plan 1 execution: decisions made during implementation and deferred minor findings, for Plan 2+.

## Plan 2 entry items
- Typed model-to-model wiring (RCC clock tree → TIM/bxCAN, GPIO → EXTI, TIM OC → GPIO AF pin, RCC enable gating of peripheral access).
- `Bus::map` must reject ranges outside the MMIO windows (or derive windows from chip TOML).
- First CI run: copy Linux x86-64 gate numbers into docs/spikes/2026-10-30-interception-gate.md.

## Decisions made during execution
- Task 1: Ruling: pass registered only at PipelineStart (plan said both EPs) — accepted: no volatile accesses are created after pipeline start, isRequired() covers -O0, metadata marker guarantees once — cost if wrong: a late-created volatile access escapes instrumentation (would show as a segfault on peripheral address, loud).
- Task 1: parked — reviewer flagged commit trailer "Opus 5.5" as wrong — Ruling: session attribution instruction specifies Claude Opus 5.5; trailer is correct.
- Task 3: Ruling: in-window 64-bit accesses split into two 32-bit Bus accesses (low, high) — plan silent; peripherals are 32-bit — cost if wrong: a model seeing a split where hardware would fault.
- Task 3: Ruling: process-global clock/bus accessors + set_delivery_hook/access_cost setters in runtime API — plan says "hook pointer" without naming — cost if wrong: rename in Task 4/5.
- Task 3: Ruling: reviewer Minor 5 (misaligned cross-register access silently partial) elevated to Important — violates Global Constraint "Strict by default / no silent zeros" — cost if wrong: one extra fix item.
- Task 3: Ruling: carry to Task 4 — clock callbacks fired from charge()/advance() are unguarded against exceptions; Task 4 (first real clock events) must guard them → stest_rt_abort — cost if wrong: UB on throwing model event.
- Task 4: Ruling: SystemCoreClock supplied via registered getter (runtime must not link the firmware symbol) — keeps runtime testable standalone — cost if wrong: small API change in Task 5.
- Task 4: Ruling: add compile-only test of CMSIS 5.9.0 core_cm4.h against host shim — catches missing shim macros before Task 5 — cost: extra FetchContent.
- Task 4: Ruling: __ASM defined as __asm (not no-op as brief said) — no-op cannot parse `__ASM volatile(...)`; ARM asm now fails loudly at build — cost if wrong: none (louder failure).
- Task 5: Ruling: pin ST HAL/CMSIS device/cmsis_core FetchContent to Sakura's submodule commits read via `git ls-tree` (047a861 / dc04e41 / 1a2f783), without touching the Sakura repo — plan said run bootstrap; reading the tree avoids side effects — cost if wrong: none.
- Task 5: Ruling: wfi at quantum end yields the firmware-thread baton; firmware runs on own thread with strict one-at-a-time handoff — Task 4 carry — cost if wrong: redesign in Task 6.
- Task 5: Ruling: if some HAL modules don't compile on 64-bit host, default HAL_SOURCES becomes a curated list instead of patching ST code — firmware/HAL unchanged constraint — cost if wrong: users add modules manually.
- Task 5: Ruling: blinky uses HAL_Delay(499) then toggle so edges land at 500/1000/... (HAL_Delay adds one tick) — plan test values kept — cost if wrong: none.
- Task 5: Ruling: speed shortfall (1.07 s/virtual-s vs 0.4 budget, from per-access cost model) fixed now via deterministic poll fast-forward in the runtime: after a streak of ≥64 loads with no stores, no IRQ delivered, and only (addr,width,value) tuples already seen in the streak (≤8 distinct), advance clock to min(next_event, quantum_end). Models must schedule a clock event for any time-driven state change (contract documented). Plus a speed regression check. — Global Constraint speed target is binding; option (c) preserves results exactly — cost if wrong: a model with lazy time-dependent state could be overshot; contract + strictness mitigates.
- Task 6: Ruling: stest_rt_abort in control mode sends ERROR msg before exit 70, safe from firmware thread (Task 5 carry) — cost if wrong: crash messages lost.
- Task 6: Ruling: pytest run registered as CTest `python_tests` with STEST_SELFTEST_BIN; fault firmware one exe per variant — cost: build time.
- Task 7: Ruling: anchor static address from symbol table (ELF symtab / Mach-O nlist), not DWARF — runtime has no -g — cost if wrong: none.
- Task 7: Ruling: set_var range-checks ints (raise, no truncation) — strictness constraint — cost: stricter API.
- Task 7: Ruling: Minor 3 (DW_OP_addr 0 for gc'd statics indexed and read) elevated to Important — silent wrong value violates strictness — cost: one more fix item.
- Task 8: Ruling: include carries — atexit kill-all + Linux PDEATHSIG (Task 6), gate bench asserts pass applied + stderr passthrough + gate doc wording (Task 2), sim fixture moved to plugin (ruling 3), stderr tail 200 for reports — cost: slightly larger task.
- Task 8: Ruling: README accuracy minors (CMake 3.20 vs 3.23 mismatch; `pip install uv` fails on Ubuntu 24.04) elevated into fix round — wrong getting-started steps mislead users — cost: trivial.
- Final: Ruling: fix wave = I1 (correct docs/ledger, expose poll-skip knob via STEST_POLL_SKIP env + Sim(poll_skip=) + marker, regression test pinning counting-loop behaviour; keep jump-to-quantum-end since it is exact for value polling), I2 (FATAL_ERROR on compiler mismatch / CMSIS core headers in INCLUDE_DIRS), CTest TIMEOUT 30, stray dwarf.get_addr line. I3 → Plan 2 entry task (with Bus::map window rejection). First CI run needs user push — cost if wrong: dilation surprises users until they find the knob.

## Deferred minor findings
- Task 1: minor (deferred): idempotence marker vs full-LTO merged modules — document or drop.
- Task 1: minor (deferred): -O2 test stubs in same TU (can inline); pointer/64-bit/atomic paths untested.
- Task 1: minor (deferred): clang vs llvm-config version match unchecked; explicit CMAKE_C_COMPILER bypasses Apple check.
- Task 1: minor (deferred): xcode-ld workaround silently no-ops without xcrun.
- Task 1: minor (deferred): zeroext not discriminated by a runtime test on arm64 — consider an IR-level (FileCheck-style) test asserting zeroext attrs.
- Task 2: minor (deferred): gate doc unit slip "90 us" should be ~90 ms/virtual-s; "~2 ns"/"two orders" phrasing → "2-4 ns", "~70-200x".
- Task 2: minor (deferred → carry to Task 8): bench must assert the pass applied (stest_now_ns == 232000*25) and run_gate.py should surface stderr — prevents false PASS in CI.
- Task 2: minor (deferred): run_gate hard-codes Ninja; no CPU pinning/warmup for sync.
- Task 3: minor (deferred): in_window checks start address only (access straddling into window unflagged).
- Task 3: minor (deferred): Bus::map accepts ranges outside windows (never dispatched) — reject or document.
- Task 3: minor (deferred): RegisterFile::declare with unaligned offset unmatched — assert off%4==0.
- Task 3: minor (deferred): no e2e test of abort message with peripheral name / model-throws path; catch blocks without return may warn; volatile cast vs memcpy aliasing.
- Task 3: minor (deferred): fix-round abort tests not re-run on Linux/Docker.
- Task 4: minor (deferred): VECTPENDING ignores BASEPRI/FAULTMASK; CALIB NOREF inconsistent; st_catch_up loops per wrap on huge advance; FAULTMASK not cleared on exception return; __set_CONTROL/MSP/PSP silent no-ops (consider strict abort on non-zero CONTROL); no 10 s drift regression test.
- Task 5: minor (deferred): FATAL_ERROR if user INCLUDE_DIRS contain cmsis_gcc.h/cmsis_compiler.h/core_cm4.h; opt-out guard for section() macro; CTest TIMEOUT 30 on selftests; RCC CFGR MCOSEL bit 27 mask; PWR PUCR/PDCR reserved bits; standalone abort drops current-quantum events; duplicate quantum_end copies; run_ms overflow; track 64-bit ptr-cast hazards (DMA, UID/flash) before DMA/flash models.
- Task 5: minor (deferred): poll-exit time can differ <25 ns across quantum sizes (per-run deterministic); counting loops over constant loads compressed (documented); model contract only documented — enforce/remind in Plan 2+ model work.
- Task 6: minor (deferred → carry to Task 8): orphan spinning nodes if Python dies — add atexit kill-all in plugin/Sim; PR_SET_PDEATHSIG on Linux / socket watchdog on macOS later.
- Task 6: minor (deferred): sends lack wall-clock bound; merged log may be non-monotonic across quantum boundaries by < one access cost (deterministic); wrong-node blame when spin+crash same quantum; event JSON keys sorted (don't match on raw key order).
- Task 7: minor (deferred): Mach-O parser bounds checks / struct.error leak; dedup externals by (name,addr) & identical labels in ambiguity; TLS/loclist vars silently skipped; anonymous typedef'd members not hoisted; bit-field struct unreadable; stray `dwarf.get_addr` line; _check_int accepts bool / size-0 enum; no dSYM UUID check; test gaps (Mach-O parser, nested arrays, lexical blocks, extern decl in other CU).
- Task 7: minor (deferred): _cache never evicts superseded keys; docstring omits dSYM in cache key; round-2 not re-run in Docker (pure Python).
- Task 8: minor (deferred): ci.yml runs Python tests twice per job; gate doc x86 pending until CI; README "Plan 1" jargon; macOS SIGTERM bypasses atexit (documented); stest sections attached on skip/xfail.
- Task 8: minor (deferred): gc test doesn't discriminate weak vs strong registry; preexec_fn runs Python with other threads alive (pre-existing).
