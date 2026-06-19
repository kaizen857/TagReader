# topplan-phases-0-5 Learnings

## 2026-06-19 Task: start-work
- Active plan: `.omo/plans/topplan-phases-0-5.md`.
- User requires per-task tests before release; failed tests must be fixed and rerun before dependent tasks or commits.
- Git writes are restricted to `git add` and `git commit`; reads are unrestricted.
- Commit messages in the plan are short Chinese descriptions.

## 2026-06-19 Task: research-results
- Parser architecture anchors: `src/core/TagPipeline.cpp:325` for `ReadMetadata`/`ReadLyrics`; `src/media/ContainerDetector.cpp:68` for `DetectTagFormat`/`ContainerFromTagFormat`; `src/core/TagFormat.hpp:6`; `src/core/ReadContext.hpp:16`.
- Public API forwarder is `src/TagReader.cpp:4`; public API shape must not change.
- Regression harness anchors: `test/regression/regression_tests.cpp:43` for `kTestCases`; `test/regression/regression_tests.cpp:4254` for `RunCase()` dispatch.
- Regression output roots: `/tmp/opencode/tagreader_regression`, `/tmp/opencode/tagreader_fuzz_corpus`, `/tmp/opencode/tagreader_security_samples`.
- Existing test style uses synthetic bytes plus ffmpeg-backed samples; no binary seeds should be committed.
- Task 1 should add `TR-AUDIT-032..035` in the single regression harness and preserve existing `TR-AUDIT-001..031`.

## 2026-06-19 Task 1: baseline regression lock
- Modified `test/regression/regression_tests.cpp` only for executable regression behavior; appended this note as required evidence.
- Added `TR-AUDIT-032` APE-over-ID3 baseline: APE title/artist/track win, ID3 album/genre/year fill missing APE fields.
- Added `TR-AUDIT-033` ID3v2-over-ID3v1 fallback: ID3v2 title/artist win, ID3v1 album/year/track/genre fill missing ID3v2 fields.
- Added `TR-AUDIT-034` cover cache reuse: same embedded PNG across files reuses one content-addressed path and does not rewrite cached PNG.
- Added `TR-AUDIT-035` malformed local field skip: valid APE title survives a truncated following item; album stays empty and `Read()` succeeds.
- Verification passed: `lsp_diagnostics test/regression/regression_tests.cpp` reported no diagnostics.
- Verification passed: `cmake -S . -B build`; `cmake --build build`.
- Verification passed: `./build/TagReaderRegressionTests --list` listed continuous `TR-AUDIT-001` through `TR-AUDIT-035`.
- Verification passed: `./build/TagReaderRegressionTests TR-AUDIT-032`; `TR-AUDIT-033`; `TR-AUDIT-034`; `TR-AUDIT-035`.
- Notable finding: all four new samples used existing synthetic/ffmpeg-backed helpers; no binary seeds or helper scripts were added, and no ffmpeg sample generation was skipped in this run.
