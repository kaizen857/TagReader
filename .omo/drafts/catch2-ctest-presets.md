---
slug: catch2-ctest-presets
status: plan-written
intent: clear
pending-action: write .omo/plans/catch2-ctest-presets.md
approach: Adopt Catch2 v3 + CTest + CMakePresets with side-by-side migration: add Catch2 coverage first, verify parity against old custom runners, then delete old runner-only code and update docs.
---

# Draft: catch2-ctest-presets

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->
- C1 | CMakePresets routes all normal/sanitize/fuzz builds under root `build/` and exports compile commands | active | `.omo/evidence/task-1-catch2-ctest-presets.txt`
- C2 | Catch2 v3 dependency and CTest discovery are integrated without deleting old tests | active | `.omo/evidence/task-2-catch2-ctest-presets.txt`
- C3 | `TR-AUDIT-001..056` are represented as Catch2/CTest-addressable cases while old runner remains available for parity | active | `.omo/evidence/task-3-catch2-ctest-presets.txt`
- C4 | Specialty regression tests are migrated to Catch2 and matched against old executables | active | `.omo/evidence/task-4-catch2-ctest-presets.txt`
- C5 | Security smoke and sample/corpus generators are registered safely in CTest without default fuzz runs | active | `.omo/evidence/task-5-catch2-ctest-presets.txt`
- C6 | Old custom runner targets/files are removed only after full parity; documentation and agent notes are updated | active | `.omo/evidence/task-6-catch2-ctest-presets.txt`

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->
- Catch2 acquisition | `find_package(Catch2 3 CONFIG QUIET)` first, `FetchContent` fallback pinned to a v3 tag | Supports system/offline packages while keeping first-run usability | yes
- Build layout | `build/default`, `build/sanitize`, `build/fuzz` under root `build/` | Meets user requirement and replaces root-level `build-*` clutter | yes
- clangd | `.clangd` points `CompilationDatabase: build/default`; preset sets `CMAKE_EXPORT_COMPILE_COMMANDS=ON` | Avoids fragile root symlink and gives clangd a stable database path | yes
- Migration strategy | tests-after, side-by-side parity before deletion | User explicitly requested new tests fully cover old cases before deleting old tests | no
- Manual CLI | Keep `TagReaderTest` as a manual field-printing tool, not a Catch2 test | It is documented as artificial/manual validation, not a unit/regression runner | yes

## Findings (cited - path:lines)
- `CMakeLists.txt` currently defines all test executables directly in the root build script: `TagReaderTest`, `TagReaderSecuritySmoke`, `TagReaderRegressionTests`, specialty regression executables, and optional `TagReaderFuzz`.
- `test/regression/regression_tests.cpp` currently contains a custom `TestCase` array with `TR-AUDIT-001..056`, custom `Expect()`, `--list`, single-case selection, and `RunTrAuditNNN()` functions.
- `test/security/security_smoke.cpp` is argument-driven and validates cover cache reuse/concurrency/pollution over one or more generated audio samples.
- `test/corpus/generate_corpus.py` and `test/security/generate_samples.py` write generated assets outside the repository by default and must remain non-committed generated data.
- `docs/DESIGN.md` and `AGENTS.md` currently describe the old custom executable workflow and must be updated when CTest becomes authoritative.
- Catch2 v3 official CMake integration supports `Catch2::Catch2WithMain`, `include(Catch)`, and `catch_discover_tests()`.
- CTest supports presets, labels, regex selection, fixtures, resource locks, and output-on-failure.
- CMakePresets can centralize binary directories and cache variables; `CMAKE_EXPORT_COMPILE_COMMANDS=ON` emits `compile_commands.json` in the build directory for Ninja/Makefile generators.

## Decisions (with rationale)
- Use Catch2 v3 rather than GoogleTest/doctest/Boost.Test because this repo has many named scenario-style regression cases and Catch2 maps those cleanly to `TEST_CASE` names/tags with low migration overhead.
- Do not collapse all tests into one opaque CTest entry; preserve `TR-AUDIT-*` in test names so `ctest -R TR-AUDIT-032` can run one case.
- Keep old targets temporarily during migration and give new Catch2 targets distinct names until parity is proven.
- Use CTest fixtures/resource locks for generated samples and shared cover cache directories instead of relying on manual shell ordering.
- Keep libFuzzer opt-in through the fuzz preset; default `ctest` must never require libFuzzer or run unbounded fuzzing.

## Scope IN
- Add Catch2 v3 dependency integration and CTest registration.
- Add `CMakePresets.json` for default/sanitize/fuzz workflows under root `build/`.
- Add `.clangd` or equivalent project-local clangd configuration pointing to `build/default`.
- Migrate `TR-AUDIT-001..056`, specialty regression tests, security smoke, and generator smoke into CTest-accessible tests.
- Remove obsolete custom test runner code/targets only after parity.
- Update `AGENTS.md`, `docs/DESIGN.md`, and `test/corpus/README.md`.

## Scope OUT (Must NOT have)
- Do not change `TagReader::Read()` public API or parser behavior.
- Do not remove `TagReaderTest` manual CLI.
- Do not commit generated audio samples, fuzz corpus, cover cache, or build output.
- Do not leave root-level `build-sanitize` / `build-fuzz` as documented paths.
- Do not make fuzz a default `ctest` dependency.

## Open questions
- None. User approved the default approach.

## Approval gate
status: approved-and-plan-written
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
