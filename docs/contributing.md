# Contributing

stest is Apache-2.0 and developed in the open at [github.com/Nexite/stest](https://github.com/Nexite/stest).

Start with these:

- [AGENTS.md](https://github.com/Nexite/stest/blob/main/AGENTS.md): repo map, how the build and runtime fit together, the non-negotiable rules (strict by default, determinism, firmware and ST sources compile unchanged) and the testing conventions. Read it first.
- [Design spec](https://github.com/Nexite/stest/blob/main/docs/superpowers/specs/2026-10-30-stest-sil-design.md): the authority when anything is unclear.
- [Adding a peripheral model](adding-a-peripheral.md): the model contract, register files, clock gating, interrupts, tests.
- [Adding a chip or part](adding-a-chip.md): the part TOML, the generator, a new family.
- [Interception speed gate results](https://github.com/Nexite/stest/blob/main/docs/spikes/2026-10-30-interception-gate.md).
