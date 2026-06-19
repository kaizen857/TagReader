# ANALYSIS.md 漏洞修复执行计划

## TL;DR
> **Summary**: 按 `ANALYSIS.md` 的“漏洞与缺陷详情”逐项修复 TagReader 的 APEv2、ID3v2.2、FFmpeg packet、输入路径 TOCTOU、无 iconv 编码降级和封面导出策略问题。每个 bug 任务采用“先加回归 → 失败确认 → 修复 → 重跑到通过 → 单独 commit”的闭环。
> **Deliverables**:
> - `TR-AUDIT-026` 至 `TR-AUDIT-031` 回归用例
> - 对应源码修复与必要文档更新
> - 每个 bug 一个独立 commit
> - 最终普通构建、sanitizer 构建、目标回归和 smoke 验证证据
> **Effort**: Large
> **Parallel**: YES - 4 waves
> **Critical Path**: Task 1 → Task 7 → Task 4 → Task 8 → Final Verification

## Context

### Original Request
用户要求：根据当前 `ANALYSIS.md` 文档中的“漏洞与缺陷详情”，编写详细完整的代码修改/编写计划；每个任务完成后必须有对应测试验证是否真正完成、是否引入额外 bug；测试不通过则循环修改直到通过；每修复一个 bug 需要提交对应 commit。

### Interview Summary
- 当前项目在 Linux 开发，但后续计划兼容 Windows。
- 封面导出策略：未提供导出地址时默认导出到系统临时目录；提供导出地址时默认信任用户地址，只建议做读写权限检验。
- 计划必须覆盖当前 `ANALYSIS.md` 中保留的 `Policy-001`、`Medium-001`、`Medium-002`、`Medium-003`、`Low-001`、`Low-002`。
- 不执行源码修改；本文件是给 `/start-work` 执行的实施计划。

### Metis Review (gaps addressed)
- 已加入“测试先行 → 修复前失败确认 → 修复 → 重跑 → commit”闭环。
- 已避免把受信导出目录 symlink 重新定为漏洞。
- 已明确不新增 CI/lint/测试框架，不实现 ID3 压缩解码，不进行大规模架构重构。
- 已明确每个验收标准都使用具体命令和 case id。
- 已明确采用用户最新策略：本轮实现 `Read(path)` 默认导出到系统临时目录；`Read(path, coverExportDir)` 使用调用方显式目录且默认受信，只做读写权限可用性检查。

## Work Objectives

### Core Objective
将 `ANALYSIS.md` 保留的问题转化为可执行修复：每个问题有专门回归测试、源码修复、验证命令、失败重试规则和独立 commit。

### Deliverables
- `test/regression/regression_tests.cpp` 增加 `TR-AUDIT-026` 至 `TR-AUDIT-031`，并更新 `kTestCases`。
- `src/formats/ape/ApeParser.cpp` 修正 APEv2 `tagSize` 语义。
- `src/formats/id3/Id3Parser.cpp` 统一 ID3v2.2 metadata/lyrics tag flag gate。
- `src/cover/CoverDecoder.cpp` 使用 padded/owned `AVPacket` 输入 FFmpeg decoder。
- `src/media/FfmpegSession.cpp` 收紧输入路径 symlink/TOCTOU 顺序，避免先打开后拒绝 symlink。
- `src/text/TextCodec.cpp` 和构建配置明确无 iconv 策略，并提供测试用 CMake 开关模拟 no-iconv 环境。
- 封面导出默认临时目录/显式目录权限策略与用户最新产品策略一致。

### Definition of Done
- `cmake -S . -B build` 退出码 0。
- `cmake --build build` 退出码 0。
- `./build/TagReaderRegressionTests TR-AUDIT-026` 至 `TR-AUDIT-031` 均退出码 0 且输出 `PASS`。
- `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON` 退出码 0。
- `cmake --build build-sanitize` 退出码 0。
- sanitizer 构建下 `./build-sanitize/TagReaderRegressionTests TR-AUDIT-026`、`TR-AUDIT-027`、`TR-AUDIT-028`、`TR-AUDIT-029`、`TR-AUDIT-030`、`TR-AUDIT-031` 均退出码 0。
- `python3 test/security/generate_samples.py` 成功或因缺少 `ffmpeg` CLI 明确跳过音频样本。
- 对生成的 security 样本，`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [...]` 隔离目录运行通过。
- 每个 bug 修复 commit 已创建，commit 消息与本计划指定消息一致。

### Must Have
- 每个任务先添加或扩展回归测试，并在修复前确认该 case 暴露问题或能覆盖目标行为。
- 每个任务测试失败时必须继续修改同一任务代码，直到该任务目标命令通过。
- 每个 bug 一个 commit；测试和修复同属同一 bug 时放入同一 commit。
- 所有样本在测试运行时生成，不提交二进制音频 seed。
- 面向用户文档和注释使用中文；源码风格保持现有 C++23 写法。

### Must NOT Have
- 不新增测试框架、CI workflow、formatter 或 lint 配置。
- 不把用户显式传入的封面导出目录 symlink 作为默认漏洞修复。
- 不实现 ID3v2.2 压缩解码；只拒绝当前不支持的 flags。
- 不把 FFmpeg metadata dictionary 改成字段来源。
- 不降低 C++ 标准，不写 C++17/C++20 降级实现。
- 不提交 `/tmp/opencode` 下的样本文件。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after with failure-first confirmation; existing executable `TagReaderRegressionTests`.
- QA policy: Every task has agent-executed scenarios and exact commands.
- Evidence: `.omo/evidence/task-{N}-{slug}.txt` for command output; `.omo/evidence/task-{N}-{slug}-diff.txt` for relevant diff summaries.
- Failure loop: If a task command fails, executor must inspect failure, modify only files in that task scope, rebuild if needed, rerun the same task command, and repeat until success before committing.

## Execution Strategy

### Parallel Execution Waves
> Target: independent bug tasks can run in parallel after shared test-harness planning. Each task touches different code paths but all edit `test/regression/regression_tests.cpp`; use serial merge or assign non-overlapping case ids to avoid conflicts.

Wave 1: Task 1 foundation/test harness reservation
Wave 2: Tasks 2, 3, 5, 6 independent bug fixes plus Task 7 default export policy
Wave 3: Task 4 FFmpeg packet padding, after Task 7 enables default-temp export tests
Wave 4: Task 8 full integration verification and commit audit

### Dependency Matrix
| Task | Blocks | Blocked By |
|---|---|---|
| 1 | 2, 3, 5, 6, 7, 8 | None |
| 2 | 8 | 1 |
| 3 | 8 | 1 |
| 4 | 8 | 1, 7 |
| 5 | 8 | 1 |
| 6 | 8 | 1 |
| 7 | 4, 8 | 1 |
| 8 | Final Verification | 2, 3, 4, 5, 6, 7 |

### Agent Dispatch Summary
| Wave | Task Count | Categories |
|---|---:|---|
| 1 | 1 | unspecified-high |
| 2 | 5 | unspecified-high, quick |
| 3 | 1 | unspecified-high |
| 4 | 1 | unspecified-high |

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.

- [x] 1. Reserve regression IDs and shared helpers

  **What to do**: Update `test/regression/regression_tests.cpp` to reserve `TR-AUDIT-026` through `TR-AUDIT-031` in `kTestCases`; add only shared helper functions needed by later tasks if they are generic and conflict-free, such as APE footer builder, ID3v2.2 builder, and cover temp directory helper. Keep every new case initially implemented as a failing placeholder only if the same commit is immediately followed by the matching bug task; otherwise add no placeholders that break full test runs.
  **Must NOT do**: Do not add incomplete implemented cases to `kTestCases` that make `./build/TagReaderRegressionTests --list` advertise unavailable tests. Do not commit this task separately unless it contains only non-breaking helper additions.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: Shared C++ test harness edits affect all later tasks.
  - Skills: [] - No specialized skill needed.
  - Omitted: [`security-research`] - This is implementation planning/execution, not new vulnerability discovery.

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: 2, 3, 5, 6, 7, 8 | Blocked By: None

  **References**:
  - Pattern: `test/regression/regression_tests.cpp:34` - `kTestCases` list format.
  - Pattern: `test/regression/regression_tests.cpp:93` - temp root helper pattern.
  - Pattern: `test/regression/regression_tests.cpp:168` - endian append helpers.
  - Pattern: `test/regression/regression_tests.cpp:3463` - dispatch pattern for individual `TR-AUDIT` cases.
  - Pattern: `AGENTS.md` - no binary seeds committed; generated samples go under `/tmp/opencode`.

  **Acceptance Criteria**:
  - [ ] `cmake --build build` exits 0.
  - [ ] `./build/TagReaderRegressionTests --list` exits 0 and lists only implemented cases.
  - [ ] `git diff --check -- test/regression/regression_tests.cpp` has no output.

  **QA Scenarios**:
  ```
  Scenario: Harness remains usable
    Tool: Bash
    Steps: Run `cmake --build build && ./build/TagReaderRegressionTests --list`.
    Expected: Exit code 0; list includes prior `TR-AUDIT-001`..`TR-AUDIT-025` and no broken placeholder cases.
    Evidence: .omo/evidence/task-1-regression-harness.txt

  Scenario: Invalid case still fails cleanly
    Tool: Bash
    Steps: Run `./build/TagReaderRegressionTests TR-AUDIT-999`.
    Expected: Non-zero exit with usage or unknown-case message; no crash.
    Evidence: .omo/evidence/task-1-regression-harness-error.txt
  ```

  **Commit**: NO | Message: `test(regression): reserve audit regression helpers` | Files: [`test/regression/regression_tests.cpp`]

- [x] 2. Fix APEv2 footer `tagSize` semantics

  **What to do**: Add `TR-AUDIT-026` that generates a minimal audio-backed MP3, appends a spec-compliant footer-only APEv2 tag where footer `tagSize == itemBytes.size() + 32`, and verifies `Title=SpecTitle`, `LYRICS=SpecLyrics`, and `Track=7` are parsed. Add a header-present APEv2 subcase where header exists before items and footer `tagSize` still excludes header, verifying the parser reads only item bytes. Add a malformed subcase with `tagSize < 32` and one with `tagSize > fileSize`, both returning empty APE fields without exception. Then change `src/formats/ape/ApeParser.cpp` so `ReadApeMetadata()` and `ReadApeLyrics()` interpret footer `tagSize` as including footer and item bytes but excluding header: item offset `fileSize - tagSize`, item size `tagSize - 32`; if header flag is set, validate optional header at `itemOffset - 32` without including it in item payload.
  **Must NOT do**: Do not break APEv1 skip behavior. Do not change APE priority over ID3. Do not read header bytes as item bytes. Do not remove APE item count/value size limits.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: Binary format semantics and regression generation require careful offset reasoning.
  - Skills: [] - Existing harness is enough.
  - Omitted: [`security-research`] - The issue is already identified.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 8 | Blocked By: 1

  **References**:
  - Bug report: `ANALYSIS.md:80` - Medium-001 details.
  - Code: `src/formats/ape/ApeParser.cpp:177` - metadata parser.
  - Code: `src/formats/ape/ApeParser.cpp:300` - lyrics parser.
  - Existing tests: `test/regression/regression_tests.cpp:2562` - `TR-AUDIT-016` valid APE fields.
  - Existing tests: `test/regression/regression_tests.cpp:2925` - `TR-AUDIT-020` malformed APE wrap guard.

  **Acceptance Criteria**:
  - [ ] Before code fix, `./build/TagReaderRegressionTests TR-AUDIT-026` fails because `title` is empty or wrong.
  - [ ] After code fix, `cmake --build build` exits 0.
  - [ ] After code fix, `./build/TagReaderRegressionTests TR-AUDIT-026` exits 0 and prints `TR-AUDIT-026 PASS`.
  - [ ] Existing `./build/TagReaderRegressionTests TR-AUDIT-016`, `TR-AUDIT-017`, `TR-AUDIT-018`, `TR-AUDIT-020`, `TR-AUDIT-023` all exit 0.
  - [ ] Sanitizer build `./build-sanitize/TagReaderRegressionTests TR-AUDIT-026` exits 0.

  **QA Scenarios**:
  ```
  Scenario: Spec footer-only APEv2 parses fields
    Tool: Bash
    Steps: Run `cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-026`.
    Expected: Exit code 0; output includes `TR-AUDIT-026 footer-only title=SpecTitle lyrics=SpecLyrics track=7` and `TR-AUDIT-026 PASS`.
    Evidence: .omo/evidence/task-2-ape-tag-size.txt

  Scenario: Malformed APE sizes are rejected safely
    Tool: Bash
    Steps: Run `./build/TagReaderRegressionTests TR-AUDIT-020 && ./build/TagReaderRegressionTests TR-AUDIT-026`.
    Expected: Both exit 0; malformed subcase prints empty APE fields or explicit rejection marker; no exception escapes.
    Evidence: .omo/evidence/task-2-ape-tag-size-error.txt
  ```

  **Commit**: YES | Message: `fix(ape): honor APEv2 footer tag size semantics` | Files: [`src/formats/ape/ApeParser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 3. Reject unsupported ID3v2.2 tag flags in metadata path

  **What to do**: Add `TR-AUDIT-027` that generates an MP3 with ID3v2.2 header flags `0x40` and `TT2=FlagTitle`, verifying `MusicTag.title()` remains empty or falls back only to a valid lower-priority tag. Include a legal v2.2 `flags=0x00` control sample with `TT2=LegalTitle`, verifying metadata still parses. Then update `ReadID3v2Metadata()` to call `Id3v22TagFlagsAreSupported(tagView.flags)` before `ReadID3v22Frames()` and return on unsupported flags, matching `ReadID3Lyrics()`.
  **Must NOT do**: Do not implement ID3v2.2 compression. Do not reject legal `flags=0x00`. Do not alter v2.3/v2.4 parsing.

  **Recommended Agent Profile**:
  - Category: `quick` - Reason: Small targeted parser guard plus regression.
  - Skills: [] - Existing ID3 helper patterns are sufficient.
  - Omitted: [`security-research`] - No new audit required.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 8 | Blocked By: 1

  **References**:
  - Bug report: `ANALYSIS.md:103` - Medium-002 details.
  - Code: `src/formats/id3/Id3Parser.cpp:95` - metadata parser missing guard.
  - Code: `src/formats/id3/Id3Parser.cpp:113` - lyrics parser guard to mirror.
  - Existing tests: `test/regression/regression_tests.cpp:1638` - strict ID3 number parsing patterns.
  - Existing tests: `test/regression/regression_tests.cpp:2196` - ID3v2.4 extended header patterns.

  **Acceptance Criteria**:
  - [ ] Before code fix, `./build/TagReaderRegressionTests TR-AUDIT-027` fails because `title` is `FlagTitle`.
  - [ ] After code fix, `cmake --build build` exits 0.
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-027` exits 0 and prints `TR-AUDIT-027 PASS`.
  - [ ] Existing `./build/TagReaderRegressionTests TR-AUDIT-006`, `TR-AUDIT-007`, `TR-AUDIT-010`, `TR-AUDIT-012`, `TR-AUDIT-021`, `TR-AUDIT-025` all exit 0.

  **QA Scenarios**:
  ```
  Scenario: Unsupported v2.2 compression flag is skipped
    Tool: Bash
    Steps: Run `cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-027`.
    Expected: Exit code 0; output includes `TR-AUDIT-027 unsupported-v22-flags-skipped` and `TR-AUDIT-027 PASS`.
    Evidence: .omo/evidence/task-3-id3-v22-flags.txt

  Scenario: Legal v2.2 metadata still parses
    Tool: Bash
    Steps: Run `./build/TagReaderRegressionTests TR-AUDIT-027`.
    Expected: Output includes legal control title `LegalTitle`; unsupported sample title is empty or valid fallback only.
    Evidence: .omo/evidence/task-3-id3-v22-flags-error.txt
  ```

  **Commit**: YES | Message: `fix(id3): reject unsupported v2.2 metadata flags` | Files: [`src/formats/id3/Id3Parser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 4. Use padded FFmpeg packet input for cover decoding

  **What to do**: Add `TR-AUDIT-028` that builds two APIC samples: one valid tiny PNG/JPEG cover and one intentionally truncated cover ending at decoder-sensitive boundary. The test must run both `TagReader::Read(path)` default system-temp export and `TagReader::Read(path, explicitDir)` explicit export. Update `ConvertImageToPng()` so decoder input is owned by FFmpeg and padded: use `av_new_packet(packet.get(), static_cast<int>(size))`, `std::memcpy(packet->data, data, size)`, and rely on FFmpeg packet allocation zero padding; alternatively allocate `size + AV_INPUT_BUFFER_PADDING_SIZE`, zero tail, and transfer ownership with `av_packet_from_data()`. Keep `size > int::max()` guard.
  **Must NOT do**: Do not leave `packet->data` pointing to caller memory. Do not remove malformed cover skip behavior. Do not increase cover input/output limits. Do not skip the `TagReader::Read(path)` default-temp subcase; Task 7 defines that as required final behavior.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: FFmpeg C API lifetime and sanitizer verification are subtle.
  - Skills: [] - Existing C++/FFmpeg patterns suffice.
  - Omitted: [`security-research`] - Known API misuse, no new audit.

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 8 | Blocked By: 1, 7

  **References**:
  - Bug report: `ANALYSIS.md:123` - Medium-003 details.
  - Code: `src/cover/CoverDecoder.cpp:177` - decoder entry.
  - Code: `src/cover/CoverDecoder.cpp:202` - current external packet assignment.
  - Existing tests: `test/regression/regression_tests.cpp:2012` - cover export valid/malformed image pattern.
  - Existing tests: `test/regression/regression_tests.cpp:2307` - cover decoding budget pattern.
  - Build config: `CMakeLists.txt:10` - sanitizer option.

  **Acceptance Criteria**:
  - [ ] Task 7 is already complete and `./build/TagReaderRegressionTests TR-AUDIT-031` exits 0 before this task starts.
  - [ ] Before code fix, sanitizer build case `TR-AUDIT-028` fails or documents the existing unpadded path via targeted assertion/instrumented expectation.
  - [ ] After code fix, `cmake --build build` exits 0.
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-028` exits 0 and prints `TR-AUDIT-028 default-temp-safe`, `TR-AUDIT-028 explicit-dir-safe`, and `TR-AUDIT-028 PASS`.
  - [ ] `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize` exits 0.
  - [ ] `./build-sanitize/TagReaderRegressionTests TR-AUDIT-028` exits 0 with no ASAN/UBSAN report.
  - [ ] Existing `TR-AUDIT-011`, `TR-AUDIT-013`, `TR-AUDIT-014`, `TR-AUDIT-023` all exit 0.

  **QA Scenarios**:
  ```
  Scenario: Truncated cover does not OOB under sanitizer
    Tool: Bash
    Steps: Run `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize && ./build-sanitize/TagReaderRegressionTests TR-AUDIT-028`.
    Expected: Exit code 0; output includes `TR-AUDIT-028 default-temp-safe`, `TR-AUDIT-028 explicit-dir-safe`, and no sanitizer report.
    Evidence: .omo/evidence/task-4-cover-packet-padding.txt

  Scenario: Valid cover export still works
    Tool: Bash
    Steps: Run `./build/TagReaderRegressionTests TR-AUDIT-011 && ./build/TagReaderRegressionTests TR-AUDIT-028`.
    Expected: Both exit 0; valid cover path exists and malformed/truncated cover is skipped or fails gracefully.
    Evidence: .omo/evidence/task-4-cover-packet-padding-error.txt
  ```

  **Commit**: YES | Message: `fix(cover): pad FFmpeg decoder packet input` | Files: [`src/cover/CoverDecoder.cpp`, `test/regression/regression_tests.cpp`]

- [x] 5. Reorder input path symlink checks and reduce TOCTOU exposure

  **What to do**: Add `TR-AUDIT-029` with a Linux symlink sample that points to a valid generated audio file. Verify `TagReader::Read(symlinkPath)` rejects before FFmpeg probe work and returns an error containing `symbolic link` or `symlink`. Then update `OpenContext()` to check `std::filesystem::is_symlink(filePath, ec)` before `file_size()`, `last_write_time()`, `ifstream.open()`, and `avformat_open_input()`. Keep cross-platform behavior: use standard filesystem checks first; optional POSIX fd binding is out of scope unless implemented minimally without breaking Windows compatibility. Also compare `file_size`/`last_write_time` after `ifstream.open()` if a non-invasive identity recheck is feasible; otherwise document residual race in `ANALYSIS.md`/`docs/DESIGN.md` as not fully eliminated.
  **Must NOT do**: Do not rewrite the entire FFmpeg input stack to custom AVIO. Do not introduce Linux-only required code paths that break Windows builds. Do not reject normal regular files.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: Filesystem behavior and cross-platform constraints require care.
  - Skills: [] - No special skill.
  - Omitted: [`security-research`] - Known low-severity issue.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 8 | Blocked By: 1

  **References**:
  - Bug report: `ANALYSIS.md:141` - Low-001 details.
  - Code: `src/media/FfmpegSession.cpp:43` - current open order.
  - Code: `src/core/TagPipeline.cpp:45` - path validation before `OpenContext()`.
  - Existing tests: `test/regression/regression_tests.cpp:93` - temp root helper.

  **Acceptance Criteria**:
  - [ ] Before code fix, `./build/TagReaderRegressionTests TR-AUDIT-029` fails because symlink is opened before rejection or error marker is not produced at the expected point.
  - [ ] After code fix, `cmake --build build` exits 0.
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-029` exits 0 and prints `TR-AUDIT-029 PASS`.
  - [ ] A normal generated MP3 path still succeeds through `TagReader::Read()` in the same case.
  - [ ] If running on a platform where symlink creation is unavailable, case prints `TR-AUDIT-029 symlink-unavailable-skip` and exits 0; Linux must not skip.

  **QA Scenarios**:
  ```
  Scenario: Symlink input is rejected before processing
    Tool: Bash
    Steps: Run `cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-029` on Linux.
    Expected: Exit code 0; output includes `TR-AUDIT-029 symlink-rejected-before-open` and `TR-AUDIT-029 PASS`.
    Evidence: .omo/evidence/task-5-input-symlink-order.txt

  Scenario: Regular input still parses
    Tool: Bash
    Steps: Run `./build/TagReaderRegressionTests TR-AUDIT-029`.
    Expected: Control regular MP3 sample parses media info or known metadata; symlink sample rejects cleanly.
    Evidence: .omo/evidence/task-5-input-symlink-order-error.txt
  ```

  **Commit**: YES | Message: `fix(media): reject symlink inputs before opening` | Files: [`src/media/FfmpegSession.cpp`, `test/regression/regression_tests.cpp`]

- [x] 6. Make no-iconv legacy encoding fallback explicit and testable

  **What to do**: Add `TR-AUDIT-030` covering Iconv and simulated no-Iconv behavior. Implement these CMake options exactly: `TAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS` default `OFF`, and `TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV` default `OFF`. Normal configure uses `find_package(Iconv)`; if Iconv is unavailable or force-disabled and fallback is not explicitly allowed, CMake must fail with a clear message. If fallback is explicitly allowed, build succeeds with a compile definition such as `TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=1`, and runtime logs/`TR-AUDIT-030` must print `TR-AUDIT-030 no-iconv-explicit-fallback`. In normal build with Iconv found, verify GB18030/GBK-like sample still decodes through iconv path using existing `TR-AUDIT-019` behavior and print `TR-AUDIT-030 iconv-enabled`.
  **Must NOT do**: Do not silently keep Latin-1 fallback for arbitrary local encodings in no-iconv builds without an explicit opt-in CMake option. Do not break UTF-8, UTF-16 BOM, or real Latin-1 decoding if opt-in is enabled. Do not claim Windows always has iconv.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: Build-option policy and encoding behavior affect portability.
  - Skills: [] - CMake/source edit only.
  - Omitted: [`security-research`] - No new vulnerability research.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 8 | Blocked By: 1

  **References**:
  - Bug report: `ANALYSIS.md:159` - Low-002 details.
  - Code: `src/text/TextCodec.cpp:130` - legacy encoding detection.
  - Build: `CMakeLists.txt:42` - optional Iconv lookup.
  - Existing tests: `test/regression/regression_tests.cpp:2821` - `TR-AUDIT-019` GB18030 detection.

  **Acceptance Criteria**:
  - [ ] Before policy fix, `cmake -S . -B build-no-iconv-forced -DTAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS=ON` succeeds or reaches the old optional path, proving the current behavior is not explicit enough.
  - [ ] After fix, normal build with Iconv still passes `TR-AUDIT-019` and `TR-AUDIT-030`.
  - [ ] `cmake -S . -B build-no-iconv-forced -DTAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS=ON` fails at configure time with a clear message mentioning `TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV`.
  - [ ] `cmake -S . -B build-no-iconv-allow -DTAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS=ON -DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON` exits 0.
  - [ ] `cmake --build build-no-iconv-allow` exits 0.
  - [ ] `./build-no-iconv-allow/TagReaderRegressionTests TR-AUDIT-030` exits 0 and prints `TR-AUDIT-030 no-iconv-explicit-fallback`.
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-019` and `TR-AUDIT-030` exit 0.

  **QA Scenarios**:
  ```
  Scenario: Iconv-enabled build preserves legacy decoding
    Tool: Bash
    Steps: Run `cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-019 && ./build/TagReaderRegressionTests TR-AUDIT-030`.
    Expected: Both exit 0; output includes `TR-AUDIT-030 iconv-enabled`.
    Evidence: .omo/evidence/task-6-no-iconv-policy.txt

  Scenario: No-iconv policy is explicit
    Tool: Bash
    Steps: Run `cmake -S . -B build-no-iconv-forced -DTAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS=ON`; then run `cmake -S . -B build-no-iconv-allow -DTAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS=ON -DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON && cmake --build build-no-iconv-allow && ./build-no-iconv-allow/TagReaderRegressionTests TR-AUDIT-030`.
    Expected: First configure fails with explicit opt-in message; second configure/build/test exits 0 and prints `TR-AUDIT-030 no-iconv-explicit-fallback`.
    Evidence: .omo/evidence/task-6-no-iconv-policy-error.txt
  ```

  **Commit**: YES | Message: `fix(text): make no-iconv encoding fallback explicit` | Files: [`CMakeLists.txt`, `src/text/TextCodec.cpp`, `test/regression/regression_tests.cpp`, `AGENTS.md` if build instructions change]

- [x] 7. Implement default-temp cover export and permission policy

  **What to do**: Implement the clarified `Policy-001` behavior in code. Add a helper in the core/cover-export path that resolves missing cover export directory to `std::filesystem::temp_directory_path() / "tagreader-covers"` or another deterministic TagReader-owned child directory under the system temp directory. `TagReader::Read(path)` must use that default directory for cover export; `TagReader::Read(path, coverExportDir)` must use the caller directory. Add explicit permission validation for the actual cover export directory: create directories if needed, verify directory exists, verify writing and reading a temporary probe file succeeds, then remove the probe. Keep user-provided directory trusted; do not reject symlink directories by default. Add `TR-AUDIT-031` verifying default-temp export, explicit-dir export, non-writable-dir error, and symlink-dir acceptance/platform skip.
  **Must NOT do**: Do not preserve old no-side-effect behavior for `Read(path)`; this task intentionally changes it to default system-temp cover export per user policy. Do not reject symlink export dirs by default. Do not create files outside cover export logic. Do not leave permission probe files behind.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: API contract, docs, tests, and cross-platform filesystem behavior intersect.
  - Skills: [] - No special skill.
  - Omitted: [`security-research`] - Policy alignment, not vulnerability discovery.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 4, 8 | Blocked By: 1

  **References**:
  - Policy report: `ANALYSIS.md:73` - Policy-001 details.
  - Pipeline: `src/core/TagPipeline.cpp` - `ValidateCoverExportDir()` and `ReadTag()` flow.
  - Cover cache: `src/cover/CoverCache.cpp` - actual write path and cache validation.
  - Public API: `include/TagReader.hpp` - `Read()` overloads.
  - Docs: `docs/DESIGN.md` - architecture contract.

  **Acceptance Criteria**:
  - [ ] Before code fix, `./build/TagReaderRegressionTests TR-AUDIT-031` fails because `TagReader::Read(path)` does not export cover to default temp.
  - [ ] `TR-AUDIT-031` exits 0 and prints `TR-AUDIT-031 default-temp-cover-export`.
  - [ ] Default cover path is under `std::filesystem::temp_directory_path()` and contains a TagReader-specific child directory.
  - [ ] Explicit writable directory sample exports a cover and reuses cache.
  - [ ] Explicit non-writable directory sample fails with `cover export` or `cover cache` error and leaves no partial files.
  - [ ] Explicit symlink-to-directory export dir is accepted when read/write permission works on platforms supporting symlinks.
  - [ ] `git diff --check -- src/core/TagPipeline.cpp src/cover/CoverCache.cpp docs/DESIGN.md ANALYSIS.md test/regression/regression_tests.cpp` has no output.

  **QA Scenarios**:
  ```
  Scenario: Cover export permission policy works
    Tool: Bash
    Steps: Run `cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-031`.
    Expected: Exit code 0; output includes `TR-AUDIT-031 default-temp-cover-export`, writable-dir success, non-writable-dir rejection or platform skip, and symlink-dir accepted or platform skip.
    Evidence: .omo/evidence/task-7-cover-export-policy.txt

  Scenario: Existing security smoke still passes
    Tool: Bash
    Steps: Run `python3 test/security/generate_samples.py`, then run `./build/TagReaderSecuritySmoke <isolated-cover-dir> <generated-audio-file>` for each generated audio file in an isolated dir.
    Expected: Exit code 0 for generated audio files; if ffmpeg CLI missing, generator reports skipped audio samples without failing the task.
    Evidence: .omo/evidence/task-7-cover-export-policy-error.txt
  ```

  **Commit**: YES | Message: `fix(cover): align export directory permission policy` | Files: [`src/core/TagPipeline.cpp`, `src/cover/CoverCache.cpp`, `include/TagReader.hpp` if API docs change, `docs/DESIGN.md`, `ANALYSIS.md`, `test/regression/regression_tests.cpp`]

- [x] 8. Run full integration verification and audit commits

  **What to do**: After Tasks 2-7 are committed individually, run the complete validation suite and verify commit granularity. Run ordinary configure/build, all new `TR-AUDIT-026..031`, selected existing nearby regressions, sanitizer configure/build, sanitizer new regressions, security smoke, and `git diff --check`. Verify `git log --oneline -10` contains one commit per bug fix with the specified messages. If any validation fails, return to the responsible task, fix, rerun its task tests, amend only if explicitly permitted by repository workflow; otherwise create a follow-up fix commit tied to the same bug and document it.
  **Must NOT do**: Do not mark final verification complete until all task-specific evidence exists. Do not squash per-bug commits unless user explicitly requests. Do not skip sanitizer for `Medium-003`.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: Multi-command integration QA and git audit.
  - Skills: [] - Existing tooling enough.
  - Omitted: [`security-research`] - Verification, not new audit.

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: Final Verification | Blocked By: 2, 3, 4, 5, 6, 7

  **References**:
  - Build commands: `AGENTS.md` - repository-specific build/test usage.
  - CMake: `CMakeLists.txt:10` - sanitizer option.
  - Regression harness: `test/regression/regression_tests.cpp:62` - executable usage.

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build` exits 0.
  - [ ] `cmake --build build` exits 0.
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-026` through `TR-AUDIT-031` all exit 0.
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-001` through `TR-AUDIT-025` selected or all-case loop exits 0; if running all individually is too slow, at minimum run adjacent cases listed in Tasks 2-7.
  - [ ] `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON` exits 0.
  - [ ] `cmake --build build-sanitize` exits 0.
  - [ ] `./build-sanitize/TagReaderRegressionTests TR-AUDIT-026` through `TR-AUDIT-031` all exit 0.
  - [ ] `git diff --check` has no output.
  - [ ] `git log --oneline -10` shows separate commits for APE, ID3, cover packet, media symlink, no-iconv, and cover policy tasks.

  **QA Scenarios**:
  ```
  Scenario: Full normal and sanitizer validation
    Tool: Bash
    Steps: Run configure/build commands, new regression cases, selected existing regressions, sanitizer configure/build, and sanitizer new regression cases.
    Expected: Every command exits 0; no sanitizer report.
    Evidence: .omo/evidence/task-8-full-integration.txt

  Scenario: Commit granularity is correct
    Tool: Bash
    Steps: Run `git log --oneline -10` and inspect messages against this plan.
    Expected: Separate commits exist for each bug; no unrelated files staged or committed.
    Evidence: .omo/evidence/task-8-full-integration-error.txt
  ```

  **Commit**: NO | Message: `verification only` | Files: []

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback -> fix -> re-run -> present again -> wait for okay.
- [x] F1. Plan Compliance Audit — oracle
- [x] F2. Code Quality Review — unspecified-high
- [x] F3. Real Manual QA — unspecified-high
- [x] F4. Scope Fidelity Check — deep

## Commit Strategy
- Commit after each bug task passes its task-specific tests.
- Include both regression test and implementation in the same bug commit.
- Required messages:
  - `fix(ape): honor APEv2 footer tag size semantics`
  - `fix(id3): reject unsupported v2.2 metadata flags`
  - `fix(cover): pad FFmpeg decoder packet input`
  - `fix(media): reject symlink inputs before opening`
  - `fix(text): make no-iconv encoding fallback explicit`
  - `fix(cover): align export directory permission policy`
- Do not commit Task 1 unless it contains independent non-breaking helpers.
- Do not commit Task 8; it is verification only.

## Success Criteria
- All `ANALYSIS.md` retained issues are addressed or explicitly policy-aligned.
- Every bug has a dedicated `TR-AUDIT-026+` regression.
- Every bug commit contains its test and fix.
- Normal and sanitizer builds pass.
- Existing nearby regression cases remain green.
- Final verification agents approve and user explicitly accepts the consolidated verification before completion.
