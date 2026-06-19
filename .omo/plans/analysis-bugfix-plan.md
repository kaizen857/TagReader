# ANALYSIS.md Bug Report 修复计划

## TL;DR
> **Summary**: 基于 `ANALYSIS.md` 的 `TR-AUDIT-001` 到 `TR-AUDIT-015`，逐项修复 TagReader 的 parser DoS、规范兼容、封面缓存/解码、文本编码和公开数据模型风险。每项 bug 必须先做最小产品代码修复，再新增轻量 C++ 回归测试验证，通过后只提交该项相关文件。
> **Deliverables**:
> - 新增轻量 C++ 回归测试目标 `TagReaderRegressionTests`。
> - 15 个独立 bug 修复，覆盖 `TR-AUDIT-001` 到 `TR-AUDIT-015`。
> - 每项 bug 对应一个验证用例入口：`./build/TagReaderRegressionTests TR-AUDIT-XXX`。
> - 每项 bug 一个独立 git commit，格式固定为 `fix(TR-AUDIT-XXX): 修复<中文摘要>`。
> **Effort**: XL
> **Parallel**: NO - 用户要求每项验证通过后提交，提交边界必须串行锁定。
> **Critical Path**: 任务 1 测试基建 → 任务 2-31 逐项修复/验证/提交 → Final Verification Wave

## Context

### Original Request
用户要求：请根据 `ANALYSIS.md` 文档中的 Bug Report 报告，制定一份完整详细的 bug 修复计划，将 bug 修复计划细致到代码层面，以任务形式规划。每一个任务执行后都应该有一个验证任务，编写测试代码确认任务是否正确完成、bug 是否真的被修复。验证通过后为这一个已经完成的任务提交 git。

### Interview Summary
- 覆盖范围：`ANALYSIS.md` 中全部 15 条 finding：`TR-AUDIT-001` 到 `TR-AUDIT-015`。
- 测试策略：新增轻量 C++ regression test 可执行目标，不引入 Catch2/GoogleTest。
- 提交策略：每个 `TR-AUDIT-*` 独立修复、独立验证、独立 commit。
- 计划语言：中文。
- 执行边界：Prometheus 只产出计划，不实现、不修复、不提交。

### Research Summary
- 未检测到 `openspec/` 或 `.specify/`。
- `docs/DESIGN.md` 明确仓库没有 CI、lint、formatter 或单元测试框架；现有构建目标为 `TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke`，可选 fuzz 目标 `TagReaderFuzz`。
- 标准构建命令：`cmake -S . -B build`，`cmake --build build`。
- 推荐新增测试入口：`test/regression/regression_tests.cpp`，在 `CMakeLists.txt` 中新增 `TagReaderRegressionTests` 并链接 `TagReaderCore`。
- 代码定位：MP4 在 `src/formats/mp4/*`；Ogg 在 `src/formats/ogg-vorbis/OggVorbisParser.cpp`；FLAC 在 `src/formats/flac/FlacParser.cpp`；ID3 在 `src/formats/id3/Id3Frames.cpp`；文本在 `src/text/*`；封面在 `src/cover/*`；公开模型在 `include/Tag.hpp`。

### Metis Review (gaps addressed)
- 固定 commit message 格式，避免执行代理自行发挥。
- 新增统一测试目标 `TagReaderRegressionTests`，避免 15 个 target 污染 CMake。
- 测试样本动态生成到 `/tmp/opencode/tagreader_regression/TR-AUDIT-XXX/`，不提交大型二进制 fixture。
- 每个 bug 拆成两个任务：修复任务、验证并提交任务。
- 验证失败时不得 commit，不得继续下一项。
- 每次 commit 前必须 `git status` / `git diff`，只 stage 当前 bug 相关文件。

## Work Objectives

### Core Objective
把 `ANALYSIS.md` 中 15 条 Bug Report 转化为可验证、可提交、可审计的逐项修复工作流：每项修复都有代码层改动点、专属回归测试、可执行命令、二元验收标准和独立 commit。

### Deliverables
- `CMakeLists.txt` 新增 `TagReaderRegressionTests` 目标。
- `test/regression/regression_tests.cpp` 及必要 helper，支持按 `TR-AUDIT-XXX` 运行单项测试。
- 产品代码修复：`src/formats/mp4/*`、`src/formats/ogg-vorbis/*`、`src/formats/flac/*`、`src/formats/id3/*`、`src/text/*`、`src/cover/*`、`include/Tag.hpp`。
- 每项提交的 evidence：`/tmp/opencode/tagreader_regression/TR-AUDIT-XXX/` 下保留测试样本、stdout/stderr、命令记录或测试程序输出。

### Definition of Done (verifiable conditions with commands)
- `cmake -S . -B build` exit code `0`。
- `cmake --build build` exit code `0`。
- `./build/TagReaderRegressionTests --list` 输出包含 `TR-AUDIT-001` 到 `TR-AUDIT-015`。
- 每项 bug 完成时，`./build/TagReaderRegressionTests TR-AUDIT-XXX` exit code `0` 且 stdout 包含 `TR-AUDIT-XXX PASS`。
- 封面/cache 相关项额外运行 `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_covers <动态生成音频样本>`，exit code `0`。
- 每项 bug 验证通过后存在一个独立 commit，`git log --oneline -1` 匹配 `fix(TR-AUDIT-XXX): 修复...`。

### Must Have
- 每个 `TR-AUDIT-*` 独立修复、独立验证、独立 commit。
- 每个验证任务必须新增或扩展 C++ 回归测试代码，不能只运行人工程序。
- 测试样本动态生成；大型二进制样本不进入仓库。
- 所有验收由 agent 执行，不依赖用户手动确认。
- 保持公开入口只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`。
- 元数据继续直接读原始字节解析；不得改用 FFmpeg `AVDictionary`。
- 最终 `MusicTag` 文本字段保持 UTF-8。

### Must NOT Have
- 不引入 Catch2、GoogleTest、CTest 大矩阵、CI workflow、lint 或 formatter。
- 不把多个 bug 合并到同一 commit。
- 验证失败时不得 commit。
- 不提交大型二进制音频或图片 fixture。
- 不做与 `ANALYSIS.md` finding 无关的重构、格式化、架构清理。
- 不扩展新音频格式，不扩展 `MusicTag` 字段。
- 不把 `TagReaderTest` 的人工输出当成自动验收。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after + lightweight C++ executable `TagReaderRegressionTests`；每项修复后补对应 regression case。
- QA policy: 每个 bug 的验证任务必须包含 happy path 和 failure/edge case，必须输出 `TR-AUDIT-XXX PASS`。
- Evidence: `/tmp/opencode/tagreader_regression/TR-AUDIT-XXX/`。
- Commit policy: 每个验证任务末尾执行 `git status --short`、`git diff`、只 `git add` 当前 bug 相关文件、`git commit -m "fix(TR-AUDIT-XXX): 修复..."`。
- Stop policy: 任一验证命令失败、测试输出不匹配、或工作树包含其他 bug 未验证改动时，立即停止；不得 commit，不得继续下一项。

## Execution Strategy

### Parallel Execution Waves
> Target: 本计划故意串行执行。原因：用户明确要求每个已完成任务验证通过后提交 git；并行执行会污染工作树和 commit 边界。

Wave 1: 测试基建搭建（任务 1）
Wave 2: MP4 高/中风险（任务 2-5）
Wave 3: Vorbis/Ogg/FLAC 资源上限（任务 6-11）
Wave 4: ID3/text 逻辑与规范修复（任务 12-21）
Wave 5: cover/cache/API hardening（任务 22-31）
Wave 6: Final Verification Wave

### Dependency Matrix (full, all tasks)
- 任务 1 阻塞全部验证任务：3,5,7,9,11,13,15,17,19,21,23,25,27,29,31。
- 每个修复任务阻塞其对应验证任务：2→3、4→5、6→7、8→9、10→11、12→13、14→15、16→17、18→19、20→21、22→23、24→25、26→27、28→29、30→31。
- 每个验证任务的 commit 阻塞下一项修复：3→4、5→6、7→8、9→10、11→12、13→14、15→16、17→18、19→20、21→22、23→24、25→26、27→28、29→30。
- Final Verification Wave 阻塞于任务 31。

### Agent Dispatch Summary (wave → task count → categories)
- Wave 1 → 1 task → `unspecified-high`
- Wave 2 → 4 tasks → `deep` / `unspecified-high`
- Wave 3 → 6 tasks → `unspecified-high`
- Wave 4 → 10 tasks → `unspecified-high`
- Wave 5 → 10 tasks → `unspecified-high`
- Final → 4 review tasks → oracle / unspecified-high / unspecified-high / deep

## Global Implementation Guardrails
- 每次开始任务前运行 `git status --short`；若存在与当前任务无关的未提交修改，停止并报告。
- 产品代码修复任务不得写测试代码，除非任务 1 的测试基建需要接口占位；测试代码只在对应验证任务添加。
- 验证任务可以修改 `test/regression/regression_tests.cpp` 和必要 helper；不得改产品代码。
- 每个 commit 前必须检查 `git diff --stat` 和 `git diff`，只 stage 当前 bug 的产品代码、测试代码、CMake 变更（如任务 1）或文档（仅 TR-AUDIT-015 需要文档时）。
- 所有测试临时文件写到 `/tmp/opencode/tagreader_regression/TR-AUDIT-XXX/`；测试开始先删除该目录再重建。
- 所有 regression case 固定通过测试 helper 调用系统 `ffmpeg` 生成最小音频容器，再 patch 对应 tag/atom/block 字节；执行前先运行 `ffmpeg -version`，若不可用则整个 regression 目标以 exit code `2` 失败并输出 `ffmpeg required`，不得跳过或假通过。
- 不允许修改 `ANALYSIS.md` 作为修复的一部分；该文件是输入审计报告。

## TODOs
> Implementation + Test = paired tasks per user request. 每个 bug 都有修复任务和验证+提交任务。

- [x] 1. 搭建统一轻量回归测试目标

  **What to do**: 在 `CMakeLists.txt` 的 `TagReaderSecuritySmoke` 后新增 `add_executable(TagReaderRegressionTests test/regression/regression_tests.cpp)`，调用 `tagreader_enable_sanitizers(TagReaderRegressionTests)` 并链接 `TagReaderCore`。新增 `test/regression/regression_tests.cpp`，实现：命令行 `--list`、单项 `TR-AUDIT-XXX` dispatch、`Expect()` helper、临时目录 helper、二进制写文件 helper、stdout 输出 `TR-AUDIT-XXX PASS/FAIL`。
  **Must NOT do**: 不引入 Catch2/GoogleTest；不新增 CTest/CI；不提交二进制 fixture；不写任何具体 bug regression case，只放 15 个 case 名称占位并让未实现 case 返回失败，后续验证任务逐项填充。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要改 CMake 和测试入口，但不涉及 parser 修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`, `webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: [3,5,7,9,11,13,15,17,19,21,23,25,27,29,31] | Blocked By: []

  **References**:
  - Pattern: `CMakeLists.txt:84-109` - `TagReaderTest` 与 `TagReaderSecuritySmoke` 的 target/link/sanitizer 写法。
  - Pattern: `test/security/security_smoke.cpp:21-187` - 简单 main、stderr、exit code 模式。
  - Pattern: `test/fuzz/tagreader_fuzz.cpp` - 临时文件/目录生成思路。

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build` exit code `0`。
  - [ ] `cmake --build build` exit code `0` 且生成 `./build/TagReaderRegressionTests`。
  - [ ] `./build/TagReaderRegressionTests --list` exit code `0`，stdout 逐行包含 `TR-AUDIT-001` 到 `TR-AUDIT-015`。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-001` 在 case 尚未实现时 exit code 非 `0` 且 stdout/stderr 包含 `TR-AUDIT-001 not implemented`，证明 dispatch 生效但不会假通过。

  **QA Scenarios**:
  ```
  Scenario: 列出所有 regression case
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests --list
    Expected: exit code 0；stdout 包含 TR-AUDIT-001 和 TR-AUDIT-015；不存在重复编号。
    Evidence: /tmp/opencode/tagreader_regression/harness/list.txt

  Scenario: 未实现 case 不得假通过
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-001
    Expected: exit code 非 0；输出包含 TR-AUDIT-001 not implemented。
    Evidence: /tmp/opencode/tagreader_regression/harness/unimplemented.txt
  ```

  **Commit**: YES | Message: `test: 添加回归测试入口` | Files: [`CMakeLists.txt`, `test/regression/regression_tests.cpp`]

- [x] 2. 修复 TR-AUDIT-001：MP4 size 0 atom 不再逐字节 sibling 恢复

  **What to do**: 在 `src/formats/mp4/Mp4AtomReader.cpp` 中删除或停用 `FindNextMp4SiblingAfterSizeZero()` 的逐字节扫描路径。修改 `ForEachMp4ChildAtom()`：遇到 `atom.size == 0` 时，将该 atom 视为延伸到当前 parent limit；对 `WalkMp4IlstItems()` 这类 child traversal，处理当前 atom 后立即停止当前 sibling loop，不再从 payload 中寻找下一个 sibling。`size==0` 出现在 item/data 子层时固定返回 `Mp4WalkResult::Malformed`。
  **Must NOT do**: 不新增宽松扫描预算；不把 payload 字节当 atom header；不改变 `size==1` extended size 行为；不扩大 `kMaxMp4Atoms`。

  **Recommended Agent Profile**:
  - Category: `deep` - MP4 walker 状态影响多路径，需要谨慎保持正常 MP4 行为。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: [3] | Blocked By: [1]

  **References**:
  - Audit: `ANALYSIS.md:40-72` - TR-AUDIT-001 原始风险和修复建议。
  - Pattern: `src/formats/mp4/Mp4AtomReader.cpp` - `ReadMp4AtomHeader()`、`FindNextMp4SiblingAfterSizeZero()`、`ForEachMp4ChildAtom()`。
  - Pattern: `src/formats/mp4/Mp4Parser.cpp` - `WalkMp4IlstItems()` 调用 walker 的 metadata 路径。

  **Acceptance Criteria**:
  - [ ] 源码中不再存在从 size-zero payload offset 逐字节扫描到 parent limit 的 sibling 恢复循环。
  - [ ] 正常 `moov/udta/meta/ilst` metadata 样本仍可解析。
  - [ ] malformed size-zero child 不触发长扫描，测试程序在小于 1 秒内完成。

  **QA Scenarios**:
  ```
  Scenario: 合法 MP4 metadata 仍可解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-001
    Expected: exit code 0；stdout 包含 TR-AUDIT-001 PASS；内部断言 title == "mp4-normal"。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-001/pass.txt

  Scenario: size-zero atom 不触发 payload sibling 扫描
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-001
    Expected: 测试生成含 size==0 child 和 1MiB 假 header payload 的样本；解析结果固定为 title 为空且不读取 payload 中伪 sibling；耗时 < 1s。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-001/size-zero.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-001): 修复MP4 size-zero扫描` | Files: [`src/formats/mp4/Mp4AtomReader.cpp`]

- [x] 3. 验证并提交 TR-AUDIT-001

  **What to do**: 在 `test/regression/regression_tests.cpp` 实现 `TR-AUDIT-001` case。测试 helper 固定用 `ffmpeg` 生成最小 M4A 音频，然后 patch atom tree 生成两个样本：一个正常 ilst title 样本，一个包含 `size==0` child 且 payload 中布置大量伪 atom type 的 malformed 样本。测试固定调用公开入口 `TagReader::Read(path)`。
  **Must NOT do**: 不改产品代码；不提交二进制样本；不使用人工 `TagReaderTest` 输出作为判断。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要编写二进制样本生成和自动断言。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: [4] | Blocked By: [2]

  **References**:
  - Test pattern: `test/security/security_smoke.cpp:148-187` - main/exit code。
  - Corpus pattern: `test/corpus/generate_corpus.py` - MP4 atom 样本生成思路。
  - Audit: `ANALYSIS.md:66-72` - fuzz 可达性和修复建议。

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build` exit code `0`。
  - [ ] `cmake --build build` exit code `0`。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-001` exit code `0`，stdout 包含 `TR-AUDIT-001 PASS`。
  - [ ] `git status --short` 只显示 TR-AUDIT-001 相关产品代码和测试文件。
  - [ ] `git commit -m "fix(TR-AUDIT-001): 修复MP4 size-zero扫描"` 成功。

  **QA Scenarios**:
  ```
  Scenario: size-zero malformed 样本快速失败
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-001
    Expected: exit code 0；stdout 包含 TR-AUDIT-001 PASS 和 size-zero recovery disabled；证据目录含 malformed.m4a。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-001/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/mp4/Mp4AtomReader.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-001): 修复MP4 size-zero扫描"
    Expected: commit 成功；git log --oneline -1 包含 fix(TR-AUDIT-001)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-001/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-001): 修复MP4 size-zero扫描` | Files: [`src/formats/mp4/Mp4AtomReader.cpp`, `test/regression/regression_tests.cpp`]

- [x] 4. 修复 TR-AUDIT-002：Ogg logical stream state 查找避免 O(n²)

  **What to do**: 在 `src/formats/ogg-vorbis/OggVorbisParser.cpp` 将 `states` 从 `std::vector<VorbisStreamState>` 改为 `std::unordered_map<std::uint32_t, VorbisStreamState>`，删除或替换 `FindState()` 线性查找。新增 `kMaxOggLogicalStreams = 256` 常量；遇到新 serial 且 map size 已达上限时停止 comment 搜索并返回 false/not found，不继续分配状态。
  **Must NOT do**: 不提高 `kMaxOggPages` 或 `kMaxOggScannedBytes`；不改变单 logical stream 正常解析行为；不把超限作为顶层崩溃错误。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 状态容器替换，需要维护 Ogg page 状态机语义。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [5] | Blocked By: [3]

  **References**:
  - Audit: `ANALYSIS.md:74-106` - TR-AUDIT-002。
  - Pattern: `src/formats/ogg-vorbis/OggVorbisParser.cpp` - `FindState()`、`ReadOggVorbisCommentEntries()`、`kMaxOggPages`。

  **Acceptance Criteria**:
  - [ ] Ogg state lookup 不再对每页执行 vector linear scan。
  - [ ] 超过 256 个 logical stream serial 时安全停止，不抛未捕获异常。
  - [ ] 正常单 stream Vorbis comment 样本仍可读取 title/artist。

  **QA Scenarios**:
  ```
  Scenario: 正常单 stream Ogg metadata 保持可解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-002
    Expected: exit code 0；stdout 包含 TR-AUDIT-002 PASS；内部断言 title == "ogg-normal"。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-002/pass.txt

  Scenario: 大量 serial 不形成 O(n²) 慢路径
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-002
    Expected: 生成 300 个不同 serial 的小 Ogg page；测试在 1 秒内返回；stdout 包含 logical-stream-limit。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-002/many-serials.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-002): 修复Ogg状态查找放大` | Files: [`src/formats/ogg-vorbis/OggVorbisParser.cpp`]

- [x] 5. 验证并提交 TR-AUDIT-002

  **What to do**: 在 `test/regression/regression_tests.cpp` 实现 `TR-AUDIT-002`。测试 helper 生成两类 Ogg 样本：正常 single serial identification/comment packet；300 个不同 serial 小 page 的异常样本。异常样本断言不会超时、不会崩溃、不会产生 metadata。
  **Must NOT do**: 不使用 wall-clock 作为唯一断言；必须同时断言输出/状态；不改产品代码。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要构造 Ogg page bytes。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [6] | Blocked By: [4]

  **References**:
  - Corpus pattern: `test/corpus/generate_corpus.py` - Ogg continuation/serial 样本生成。
  - Test target: `test/regression/regression_tests.cpp` - case dispatch。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-002` exit code `0`，stdout 包含 `TR-AUDIT-002 PASS`。
  - [ ] evidence 目录保存 `many_serials.ogg` 和测试输出。
  - [ ] commit 成功且只包含 Ogg parser 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: 多 serial 资源边界验证
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-002
    Expected: exit code 0；stdout 包含 many serials rejected without quadratic scan。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-002/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/ogg-vorbis/OggVorbisParser.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-002): 修复Ogg状态查找放大"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-002)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-002/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-002): 修复Ogg状态查找放大` | Files: [`src/formats/ogg-vorbis/OggVorbisParser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 6. 修复 TR-AUDIT-003：Vorbis Comment 数量上限与剩余字节预检

  **What to do**: 在 `src/formats/vorbis/VorbisCommentLimits.hpp` 新增共享常量 `inline constexpr std::uint32_t kMaxVorbisComments = 4096;`，并在 FLAC 和 Ogg Vorbis comment walker 中引用该常量。修改 `ForEachFlacVorbisCommentEntry()` 和 `ForEachVorbisCommentEntry()`：读取 `commentCount` 后先检查 `commentCount <= kMaxVorbisComments`，再检查 `commentCount <= (size - cursor) / 4`，否则返回 false/malformed。保持每个 comment length 的既有边界检查。
  **Must NOT do**: 不吞掉合法 4096 条以内 comment；不更改 key/value normalization 策略；不把 malformed block 变成成功但字段为空，除非现有调用路径已有该容错。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 两个格式分支需同步修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [7] | Blocked By: [5]

  **References**:
  - Audit: `ANALYSIS.md:108-140` - TR-AUDIT-003。
  - Pattern: `src/formats/flac/FlacParser.cpp` - `ForEachFlacVorbisCommentEntry()`。
  - Pattern: `src/formats/ogg-vorbis/OggVorbisParser.cpp` - `ForEachVorbisCommentEntry()`。

  **Acceptance Criteria**:
  - [ ] FLAC 和 Ogg comment count 均有相同上限和 `remaining / 4` 快速拒绝。
  - [ ] 合法少量 Vorbis Comment 仍解析。
  - [ ] `UINT32_MAX` commentCount 的短 payload 不进入长循环。

  **QA Scenarios**:
  ```
  Scenario: 合法 Vorbis comment 解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-003
    Expected: exit code 0；stdout 包含 TR-AUDIT-003 PASS；内部断言 FLAC/Ogg 正常 title 可读。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-003/pass.txt

  Scenario: 巨大 commentCount 快速拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-003
    Expected: 构造 commentCount=0xffffffff 且 payload 只有 4-16 字节；测试在 1 秒内完成；stdout 包含 comment-count-limit。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-003/limit.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-003): 限制Vorbis注释数量` | Files: [`src/formats/vorbis/VorbisCommentLimits.hpp`, `src/formats/flac/FlacParser.cpp`, `src/formats/ogg-vorbis/OggVorbisParser.cpp`]

- [x] 7. 验证并提交 TR-AUDIT-003

  **What to do**: 在 regression test 中实现 `TR-AUDIT-003`。动态生成 FLAC Vorbis Comment block 与 Ogg comment packet：正常 count=1；异常 count=`0xffffffff`/`4097` 且剩余字节不足。固定断言：异常输入不产生长循环、不崩溃，且 `MusicTag` 的 title/artist/album 均为空字符串。
  **Must NOT do**: 不改产品代码；不把耗时阈值作为唯一判断。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要构造 FLAC/Ogg 两类二进制样本。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [8] | Blocked By: [6]

  **References**:
  - Corpus pattern: `test/corpus/generate_corpus.py` - `flac` / `ogg` 样本分类。
  - Audit: `ANALYSIS.md:134-140` - fuzz 可达性与修复建议。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-003` exit code `0`，stdout 包含 `TR-AUDIT-003 PASS`。
  - [ ] evidence 保存 `comment_count_max.flac` 与 `comment_count_max.ogg`。
  - [ ] commit 成功且只包含 FLAC/Ogg parser 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: FLAC 与 Ogg commentCount 上限
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-003
    Expected: exit code 0；stdout 包含 flac comment-count-limit 和 ogg comment-count-limit。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-003/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/vorbis/VorbisCommentLimits.hpp src/formats/flac/FlacParser.cpp src/formats/ogg-vorbis/OggVorbisParser.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-003): 限制Vorbis注释数量"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-003)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-003/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-003): 限制Vorbis注释数量` | Files: [`src/formats/vorbis/VorbisCommentLimits.hpp`, `src/formats/flac/FlacParser.cpp`, `src/formats/ogg-vorbis/OggVorbisParser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 8. 修复 TR-AUDIT-004：MP4 已有封面后跳过后续 covr payload

  **What to do**: 在 `src/formats/mp4/Mp4Parser.cpp` 的 `ReadMp4ItemAtom()` 入口增加快速返回：如果 `atomType == "covr" && !metadata.coverPath.empty()`，直接跳过整个 item，不进入 child traversal，不调用 `ReadMp4AtomPayload()`。在 `ReadMp4DataAtom()` 的 `covr` 分支保留防御性 `metadata.coverPath` 检查，形成双保险。
  **Must NOT do**: 不改变第一张封面的读取/解码；不改变非 `covr` item；不扩大 `kMaxCoverInputBytes`。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 小范围性能/资源修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: [9] | Blocked By: [7]

  **References**:
  - Audit: `ANALYSIS.md:142-174` - TR-AUDIT-004。
  - Pattern: `src/formats/mp4/Mp4Parser.cpp` - `ReadMp4ItemAtom()`、`ReadMp4DataAtom()`、`Mp4MetadataPayloadLimit()`。

  **Acceptance Criteria**:
  - [ ] 第二个及后续 `covr` item 在已有 `metadata.coverPath` 时不读取 payload。
  - [ ] 第一张有效 `covr` 仍导出 PNG。
  - [ ] 多 `covr` 样本不重复写 cover cache。

  **QA Scenarios**:
  ```
  Scenario: 第一张 MP4 封面正常导出
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-004
    Expected: exit code 0；stdout 包含 TR-AUDIT-004 PASS；coverPath 非空且 PNG 存在。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-004/pass.txt

  Scenario: 后续 covr payload 被跳过
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-004
    Expected: 样本包含两个 covr；第二个 payload 为接近测试预算的大块无效 bytes；读取仍成功且 cover cache mtime 不因第二个 covr 改变。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-004/skip-second-covr.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-004): 跳过重复MP4封面` | Files: [`src/formats/mp4/Mp4Parser.cpp`]

- [x] 9. 验证并提交 TR-AUDIT-004

  **What to do**: 在 regression test 中实现 `TR-AUDIT-004`。生成含两个 `covr/data` atom 的 MP4/M4A 样本，第一张固定为测试源码内 1x1 PNG bytes，第二张固定为 2MiB payload。断言 `coverPath` 稳定、只产生一个 PNG、重复读取 mtime 不变。额外运行 `TagReaderSecuritySmoke` 验证 cover cache 行为。
  **Must NOT do**: 不修改产品代码；不提交大图片 fixture；测试中大 payload 固定为 2MiB，不使用范围值。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要 MP4 + cover filesystem 断言。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: [10] | Blocked By: [8]

  **References**:
  - Smoke pattern: `test/security/security_smoke.cpp:77-145` - cover cache mtime/并发/污染检查。
  - Audit: `ANALYSIS.md:168-174` - fuzz 可达性和修复建议。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-004` exit code `0`。
  - [ ] `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_covers /tmp/opencode/tagreader_regression/TR-AUDIT-004/multi_covr.m4a` exit code `0`。
  - [ ] commit 成功且只包含 MP4 parser 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: 多 covr 不重复导出
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-004
    Expected: exit code 0；stdout 包含 single-cover-export 和 TR-AUDIT-004 PASS。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-004/stdout.txt

  Scenario: security smoke 覆盖 cache 复用
    Tool: Bash
    Steps: ./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_covers /tmp/opencode/tagreader_regression/TR-AUDIT-004/multi_covr.m4a
    Expected: exit code 0；stdout 包含 coverPath；重复读取 mtime 不变。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-004/security-smoke.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-004): 跳过重复MP4封面` | Files: [`src/formats/mp4/Mp4Parser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 10. 修复 TR-AUDIT-005：FLAC 已有封面后跳过后续 PICTURE block

  **What to do**: 在 `src/formats/flac/FlacParser.cpp` 的 `ReadFlacMetadataBlocks()` 处理 block type 6 前检查 `metadata.coverPath.empty()`：若已有封面，使用 block header length 直接跳过 payload，不调用 `ReadRange()` 读取大 block，不调用 `ReadFlacPictureEntry()`。在 `ReadFlacPictureEntry()` 开头也增加 `if (!metadata.coverPath.empty()) return;` 防御性返回。
  **Must NOT do**: 不改变第一张 front cover type 3 的解析；不处理非 type 3 图片；不扩大封面输入上限。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - FLAC block walker 小范围资源修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [11] | Blocked By: [9]

  **References**:
  - Audit: `ANALYSIS.md:176-208` - TR-AUDIT-005。
  - Pattern: `src/formats/flac/FlacParser.cpp` - `ReadFlacMetadataBlocks()`、`ReadFlacPictureEntry()`。

  **Acceptance Criteria**:
  - [ ] 第二个及后续 PICTURE block 在已有 coverPath 时不读取/解码 payload。
  - [ ] 第一张有效 PICTURE 仍导出 PNG。
  - [ ] 多 PICTURE 样本读取不会重复解码。

  **QA Scenarios**:
  ```
  Scenario: 第一张 FLAC PICTURE 正常导出
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-005
    Expected: exit code 0；stdout 包含 TR-AUDIT-005 PASS；coverPath 非空。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-005/pass.txt

  Scenario: 后续 PICTURE block 被跳过
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-005
    Expected: 样本包含两个 PICTURE block；第二个大 payload 不影响读取；stdout 包含 skip-second-picture。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-005/skip-second-picture.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-005): 跳过重复FLAC封面` | Files: [`src/formats/flac/FlacParser.cpp`]

- [x] 11. 验证并提交 TR-AUDIT-005

  **What to do**: 在 regression test 中实现 `TR-AUDIT-005`。动态生成 FLAC 样本：STREAMINFO + 两个 PICTURE block；第一张固定为测试源码内 1x1 PNG bytes，第二张固定为 2MiB payload。断言只产生一个 coverPath，重复读取 mtime 不变，`TagReaderSecuritySmoke` 对该样本通过。
  **Must NOT do**: 不改产品代码；不提交图片 fixture；不把“未崩溃”作为唯一验收。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要 FLAC metadata block 与 cover 断言。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [12] | Blocked By: [10]

  **References**:
  - Corpus pattern: `test/corpus/generate_corpus.py` - FLAC PICTURE 样本生成。
  - Smoke pattern: `test/security/security_smoke.cpp:77-145` - cover cache 检查。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-005` exit code `0`，stdout 包含 `TR-AUDIT-005 PASS`。
  - [ ] `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_covers /tmp/opencode/tagreader_regression/TR-AUDIT-005/multi_picture.flac` exit code `0`。
  - [ ] commit 成功且只包含 FLAC parser 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: 多 PICTURE 不重复导出
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-005
    Expected: exit code 0；stdout 包含 single-flac-cover-export 和 TR-AUDIT-005 PASS。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-005/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/flac/FlacParser.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-005): 跳过重复FLAC封面"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-005)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-005/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-005): 跳过重复FLAC封面` | Files: [`src/formats/flac/FlacParser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 12. 修复 TR-AUDIT-006：ID3 malformed frame 后尝试同步合法后续 frame

  **What to do**: 在 `src/formats/id3/Id3Frames.cpp` 为 ID3v2.2 和 ID3v2.3/2.4 metadata/lyrics walker 增加 shared resync helper：遇到非法 frame id、`frameSize == 0`、frame size 超过剩余 tag、v2.4 非 syncsafe size 时，先判断是否明确 padding（当前位置起若全 0 或 frame id 首字节为 0 则停止）；否则从 `cursor + 1` 到 `limit - headerSize` 查找下一个 `IsLikelyId3FrameId()` / `IsLikelyId3v22FrameId()` 且 size 字段合法、payload 在 limit 内的 header。找到则把 cursor 移到该 header 继续；找不到才 break。
  **Must NOT do**: 不在真正 padding 后继续扫描；不越过 tag limit；不接受压缩/加密 frame；不改变正常 frame 顺序解析。

  **Recommended Agent Profile**:
  - Category: `deep` - ID3 frame walker 多分支共享逻辑，风险较高。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [13] | Blocked By: [11]

  **References**:
  - Audit: `ANALYSIS.md:210-242` - TR-AUDIT-006。
  - Pattern: `src/formats/id3/Id3Frames.cpp` - `ReadID3v22Frames()`、`ReadID3v23Or24Frames()`、`ReadID3v22LyricsFrames()`、`ReadID3v23Or24LyricsFrames()`。
  - Helper: `src/formats/id3/Id3Frames.cpp` - `IsLikelyId3FrameId()`、`IsLikelyId3v22FrameId()`。

  **Acceptance Criteria**:
  - [ ] 坏 frame 后跟合法 `TIT2` 时仍解析 title。
  - [ ] 坏 frame 后跟合法 `USLT`/`TXXX` lyrics 时仍解析歌词。
  - [ ] 真 padding 仍停止，不扫描 padding 噪声。

  **QA Scenarios**:
  ```
  Scenario: malformed frame 后 metadata 恢复
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-006
    Expected: exit code 0；stdout 包含 TR-AUDIT-006 PASS；内部断言 title == "after-bad-frame"。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-006/pass.txt

  Scenario: padding 不被误扫描
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-006
    Expected: 包含 padding 的 ID3 样本不会把 padding 后噪声解析为 frame；stdout 包含 padding-stop。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-006/padding.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-006): 恢复ID3坏帧后续解析` | Files: [`src/formats/id3/Id3Frames.cpp`]

- [x] 13. 验证并提交 TR-AUDIT-006

  **What to do**: 在 regression test 中实现 `TR-AUDIT-006`。测试 helper 固定用 `ffmpeg` 生成最小 MP3 音频，再 patch ID3v2.3/2.4 tag：坏 frame header 后接合法 `TIT2`、`USLT`；另生成 padding 样本。断言坏 frame 后合法字段仍进入 `MusicTag`。
  **Must NOT do**: 不改产品代码；不要求用户提供 MP3；不提交二进制样本。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要 ID3 byte builder 和断言。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [14] | Blocked By: [12]

  **References**:
  - Corpus pattern: `test/corpus/generate_corpus.py` - ID3v2 frame 样本。
  - Audit: `ANALYSIS.md:236-242` - fuzz 可达性与修复建议。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-006` exit code `0`，stdout 包含 `TR-AUDIT-006 PASS`。
  - [ ] evidence 保存 malformed-then-title 和 malformed-then-lyrics 样本。
  - [ ] commit 成功且只包含 ID3 frame walker 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: ID3 坏帧后合法标题/歌词不丢失
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-006
    Expected: exit code 0；stdout 包含 recovered-title 和 recovered-lyrics。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-006/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/id3/Id3Frames.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-006): 恢复ID3坏帧后续解析"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-006)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-006/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-006): 恢复ID3坏帧后续解析` | Files: [`src/formats/id3/Id3Frames.cpp`, `test/regression/regression_tests.cpp`]

- [x] 14. 修复 TR-AUDIT-007：ID3 track/disc 数字必须完整消费

  **What to do**: 在 `src/formats/id3/Id3Frames.cpp` 修改 `ParseUInt16()`：调用 `std::stoul()` 后要求 `consumed == value.size()`，否则返回空。`ParseSlashNumber()` 对 slash 两侧分别 trim；左侧为空或含垃圾时拒绝当前字段；右侧存在但为空/含垃圾时拒绝 total。确保 `TRK`/`TPA`/`TRCK`/`TPOS` 分支都使用严格结果。
  **Must NOT do**: 不改变合法 `12/34`、`003/010` 行为；不把非法 total 写成 0；不影响 MP4 track/disk parser。

  **Recommended Agent Profile**:
  - Category: `quick` - 局部解析逻辑修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [15] | Blocked By: [13]

  **References**:
  - Audit: `ANALYSIS.md:244-276` - TR-AUDIT-007。
  - Pattern: `src/formats/id3/Id3Frames.cpp` - `ParseUInt16()`、`ParseSlashNumber()`、`ReadID3v22Frame()`、`ReadID3v2Frame()`。

  **Acceptance Criteria**:
  - [ ] `12abc/7` 不设置 track/disc 数值。
  - [ ] `12/7abc` 整组拒绝，current/total 均不写入。
  - [ ] `003/010` 仍解析为 3/10。

  **QA Scenarios**:
  ```
  Scenario: 合法 track/disc 保持解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-007
    Expected: exit code 0；stdout 包含 TR-AUDIT-007 PASS；合法样本 trackNumber == 3 且 totalTracks == 10。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-007/pass.txt

  Scenario: 数字前缀垃圾被拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-007
    Expected: `12abc/7` 与 `003x/01` 不产生 track/disc 数值；stdout 包含 strict-number-parse。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-007/strict.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-007): 严格解析ID3轨道编号` | Files: [`src/formats/id3/Id3Frames.cpp`]

- [x] 15. 验证并提交 TR-AUDIT-007

  **What to do**: 在 regression test 中实现 `TR-AUDIT-007`。生成 ID3v2.3 `TRCK`/`TPOS` 与 ID3v2.2 `TRK`/`TPA` 样本，覆盖合法值、数字前缀垃圾、slash 右侧垃圾。断言非法字段不会污染 `MusicTag` 数值字段。
  **Must NOT do**: 不改产品代码；不只测 v2.3，必须覆盖 v2.2 和 v2.3/2.4 至少两条路径。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 测试覆盖多 ID3 版本。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [16] | Blocked By: [14]

  **References**:
  - Audit: `ANALYSIS.md:270-276` - fuzz 可达性与修复建议。
  - Test target: `test/regression/regression_tests.cpp`。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-007` exit code `0`，stdout 包含 `TR-AUDIT-007 PASS`。
  - [ ] commit 成功且只包含 ID3 parser 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: v2.2/v2.3 严格数字解析
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-007
    Expected: exit code 0；stdout 包含 v22-strict 和 v23-strict。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-007/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/id3/Id3Frames.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-007): 严格解析ID3轨道编号"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-007)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-007/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-007): 严格解析ID3轨道编号` | Files: [`src/formats/id3/Id3Frames.cpp`, `test/regression/regression_tests.cpp`]

- [x] 16. 修复 TR-AUDIT-008：LRC 数字解析乘加前检查溢出

  **What to do**: 在 `src/text/TextNormalize.cpp` 修改 `ParseDecimalU16Strict()`：每次 `result = result * 10 + digit` 前先检查 `result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10`，溢出则返回空；随后保留 `result > std::numeric_limits<std::uint16_t>::max()` 检查。不新增字段长度启发式限制，避免与现有合法长分钟行为产生额外语义变化。
  **Must NOT do**: 不改变合法 `[01:02.34]`、`[123:45.678]` 的解析；不把超长数字截断为合法时间；不改变 plain lyrics 非 LRC 行保留策略。

  **Recommended Agent Profile**:
  - Category: `quick` - 局部整数解析修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [17] | Blocked By: [15]

  **References**:
  - Audit: `ANALYSIS.md:278-310` - TR-AUDIT-008。
  - Pattern: `src/text/TextNormalize.cpp` - `ParseDecimalU16Strict()`、`ParseLrcTimestamp()`、`ReadLyricsFromPlainText()`。

  **Acceptance Criteria**:
  - [ ] 超长 minute/second/fraction 不发生 uint32 回绕且被拒绝。
  - [ ] 合法 LRC timestamp 仍生成 timed lyric。
  - [ ] 非 timestamp 方括号行仍按现有策略保留为 plain lyric。

  **QA Scenarios**:
  ```
  Scenario: 合法 LRC 时间戳保持解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-008
    Expected: exit code 0；stdout 包含 TR-AUDIT-008 PASS；合法样本 synced lyric timeMs == 62340。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-008/pass.txt

  Scenario: 超长数字时间戳被拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-008
    Expected: 输入 `[999999999999999999999999:01.00]bad` 不产生 synced lyric；stdout 包含 overflow-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-008/overflow.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-008): 修复LRC时间戳溢出` | Files: [`src/text/TextNormalize.cpp`]

- [x] 17. 验证并提交 TR-AUDIT-008

  **What to do**: 在 regression test 中实现 `TR-AUDIT-008`。测试 helper 固定用 `ffmpeg` 生成最小 MP3 音频，再 patch ID3 `USLT` lyrics tag，嵌入 LRC 文本：合法 `[01:02.340]ok`、超长分钟、超长秒、超长毫秒。断言合法行保留，异常 timestamp 不进入 synced lyrics。
  **Must NOT do**: 不改产品代码；不只测试 ASCII 普通歌词，必须触发 LRC timestamp parser。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要构造歌词 tag 或最小音频样本。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [18] | Blocked By: [16]

  **References**:
  - Audit: `ANALYSIS.md:304-310` - fuzz 可达性和修复建议。
  - Pattern: `test/corpus/generate_corpus.py` - encoding/LRC 样本生成。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-008` exit code `0`，stdout 包含 `TR-AUDIT-008 PASS`。
  - [ ] evidence 固定保存 `/tmp/opencode/tagreader_regression/TR-AUDIT-008/lrc_overflow.mp3`。
  - [ ] commit 成功且只包含 TextNormalize 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: LRC overflow regression
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-008
    Expected: exit code 0；stdout 包含 overflow-rejected 和 legal-lrc-preserved。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-008/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/text/TextNormalize.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-008): 修复LRC时间戳溢出"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-008)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-008/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-008): 修复LRC时间戳溢出` | Files: [`src/text/TextNormalize.cpp`, `test/regression/regression_tests.cpp`]

- [x] 18. 修复 TR-AUDIT-009：Ogg Vorbis identification/comment packet 完整性校验

  **What to do**: 在 `src/formats/ogg-vorbis/OggVorbisParser.cpp` 新增 `IsValidVorbisIdentificationPacket()`：要求 packet size 至少 30 bytes、前缀 `0x01 "vorbis"`、version == 0、channels > 0、sampleRate > 0、framing flag bit set。状态机只有 identification packet 验证通过后才从 `LookingForIdentification` 进入 `LookingForComment`。新增 `IsPlausibleVorbisCommentPacket()`：要求前缀 `0x03 "vorbis"` 且至少包含 vendor length 的 4 字节字段，再进入 comment walker。
  **Must NOT do**: 不只检查 7-byte prefix；不拒绝合法 comment packet；不改变 page lacing/continuation 逻辑。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Ogg 状态机规范修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [19] | Blocked By: [17]

  **References**:
  - Audit: `ANALYSIS.md:312-344` - TR-AUDIT-009。
  - Pattern: `src/formats/ogg-vorbis/OggVorbisParser.cpp` - `HasVorbisPrefix()`、`ReadOggVorbisCommentEntries()`。

  **Acceptance Criteria**:
  - [ ] 仅 7-byte identification prefix 的 packet 不推进状态机。
  - [ ] 仅 7-byte comment prefix 的 packet 不进入 Vorbis Comment walker。
  - [ ] 完整合法 identification + comment packet 仍解析 metadata。

  **QA Scenarios**:
  ```
  Scenario: 完整 Ogg Vorbis identification/comment 仍解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-009
    Expected: exit code 0；stdout 包含 TR-AUDIT-009 PASS；合法样本 title == "ogg-valid-ident"。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-009/pass.txt

  Scenario: prefix-only packet 被拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-009
    Expected: 7-byte prefix-only identification/comment 样本不产生 metadata；stdout 包含 prefix-only-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-009/prefix-only.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-009): 校验Ogg Vorbis包结构` | Files: [`src/formats/ogg-vorbis/OggVorbisParser.cpp`]

- [x] 19. 验证并提交 TR-AUDIT-009

  **What to do**: 在 regression test 中实现 `TR-AUDIT-009`。构造三个 Ogg 样本：合法 identification/comment；只有 identification prefix；合法 identification 后只有 comment prefix。断言 prefix-only 样本不解析 metadata，合法样本正常。
  **Must NOT do**: 不改产品代码；不只测试 comment prefix，必须覆盖 identification prefix-only。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Ogg 二进制样本构造。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [20] | Blocked By: [18]

  **References**:
  - Corpus pattern: `test/corpus/generate_corpus.py` - Ogg packet 样本。
  - Audit: `ANALYSIS.md:338-344` - fuzz 可达性和修复建议。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-009` exit code `0`，stdout 包含 `TR-AUDIT-009 PASS`。
  - [ ] commit 成功且只包含 Ogg parser 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: Ogg prefix-only 拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-009
    Expected: exit code 0；stdout 包含 invalid-ident-rejected 和 invalid-comment-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-009/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/ogg-vorbis/OggVorbisParser.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-009): 校验Ogg Vorbis包结构"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-009)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-009/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-009): 校验Ogg Vorbis包结构` | Files: [`src/formats/ogg-vorbis/OggVorbisParser.cpp`, `test/regression/regression_tests.cpp`]

- [x] 20. 修复 TR-AUDIT-010：ID3 UTF-16 with BOM 缺 BOM 时拒绝而非默认小端

  **What to do**: 在 `src/text/TextCodec.cpp` 修改 `ReadUtf16TextWithBom()`：未发现 `FF FE` 或 `FE FF` BOM 时固定返回空字符串，不再调用 `ReadUtf16Text(data, size, false)` 默认 little-endian。确认 `TryReadUtf16Text()` 对显式 UTF-16LE/BE 路径仍正常，并保证 normalize 后不写入字段。
  **Must NOT do**: 不改变 encoding byte `0x02` UTF-16BE、`0x00` Latin-1、`0x03` UTF-8 行为；不新增 public legacy 模式；不把缺 BOM 文本猜测为 UTF-8。

  **Recommended Agent Profile**:
  - Category: `quick` - 局部编码策略修复。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [21] | Blocked By: [19]

  **References**:
  - Audit: `ANALYSIS.md:346-378` - TR-AUDIT-010。
  - Pattern: `src/text/TextCodec.cpp` - `ReadUtf16TextWithBom()`、`TryReadUtf16Text()`、`DecodeTextToUtf8()`。

  **Acceptance Criteria**:
  - [ ] ID3 encoding byte `0x01` 且 payload 无 BOM 时不写入 title/artist/lyrics。
  - [ ] encoding byte `0x01` 且 payload 有 LE/BE BOM 时仍正确解码。
  - [ ] 显式 UTF-16BE 路径不受影响。

  **QA Scenarios**:
  ```
  Scenario: 有 BOM 的 UTF-16 title 正常解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-010
    Expected: exit code 0；stdout 包含 TR-AUDIT-010 PASS；title == "标题"。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-010/pass.txt

  Scenario: 缺 BOM 的 encoding=1 被拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-010
    Expected: encoding=1 且无 BOM 的 TIT2 不写入 title；stdout 包含 bomless-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-010/bomless.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-010): 拒绝缺BOM的UTF16文本` | Files: [`src/text/TextCodec.cpp`]

- [x] 21. 验证并提交 TR-AUDIT-010

  **What to do**: 在 regression test 中实现 `TR-AUDIT-010`。生成 ID3v2.3 `TIT2` 三个样本：encoding=1 + UTF-16LE BOM + 中文；encoding=1 + UTF-16BE BOM + 中文；encoding=1 + 无 BOM 偶数字节。断言前两者解析为 UTF-8 中文，第三者字段为空。
  **Must NOT do**: 不改产品代码；不只测 ASCII；不将缺 BOM 样本视作兼容成功。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要编码字节样本构造和 UTF-8 断言。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [22] | Blocked By: [20]

  **References**:
  - Audit: `ANALYSIS.md:372-378` - fuzz 可达性与修复建议。
  - Pattern: `test/corpus/generate_corpus.py` - encoding 样本。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-010` exit code `0`，stdout 包含 `TR-AUDIT-010 PASS`。
  - [ ] commit 成功且只包含 TextCodec 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: UTF-16 BOM 严格性
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-010
    Expected: exit code 0；stdout 包含 utf16le-bom-ok、utf16be-bom-ok、bomless-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-010/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/text/TextCodec.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-010): 拒绝缺BOM的UTF16文本"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-010)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-010/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-010): 拒绝缺BOM的UTF16文本` | Files: [`src/text/TextCodec.cpp`, `test/regression/regression_tests.cpp`]

- [x] 22. 修复 TR-AUDIT-011：检查 sws_scale 输出行数

  **What to do**: 在 `src/cover/CoverDecoder.cpp` 的 `ConvertImageToPng()` 中保存 `const int scaledRows = sws_scale(...)` 返回值；要求 `scaledRows == decodedFrame->height`，否则释放/清理 sws context 后返回空 vector/空封面。不要新增目标 buffer 清零逻辑，本任务只检查 `sws_scale()` 返回行数，避免混入额外行为变化。
  **Must NOT do**: 不忽略负返回值；不把部分转换结果交给 PNG encoder；不改变尺寸/像素上限。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - FFmpeg 错误路径需谨慎。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [23] | Blocked By: [21]

  **References**:
  - Audit: `ANALYSIS.md:380-412` - TR-AUDIT-011。
  - Pattern: `src/cover/CoverDecoder.cpp` - `ConvertImageToPng()`、`kCoverDecodeLimits`。

  **Acceptance Criteria**:
  - [ ] `sws_scale()` 返回值被检查，负值或少于 height 均返回空封面。
  - [ ] 正常小 PNG/JPEG 封面仍可转 PNG。
  - [ ] malformed 图片不产生不稳定 PNG cache bytes。

  **QA Scenarios**:
  ```
  Scenario: 正常封面仍导出
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-011
    Expected: exit code 0；stdout 包含 TR-AUDIT-011 PASS；正常图片 coverPath 存在。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-011/pass.txt

  Scenario: swscale 异常路径不编码部分帧
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-011
    Expected: 使用畸形图片 payload；`coverPath.empty()` 为 true，cover export 目录不新增 PNG；stdout 包含 malformed-image-skipped。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-011/malformed.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-011): 检查封面缩放结果` | Files: [`src/cover/CoverDecoder.cpp`]

- [x] 23. 验证并提交 TR-AUDIT-011

  **What to do**: 在 regression test 中实现 `TR-AUDIT-011`。生成含正常最小封面的音频样本和含畸形图片 payload 的样本。断言正常封面导出；畸形封面必须同时满足 `coverPath.empty()`、cover export 目录无新增 PNG、cache 文件数量不变；运行 `TagReaderSecuritySmoke` 覆盖正常封面路径。
  **Must NOT do**: 不改产品代码；不依赖人工查看图片；不提交图片 fixture。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 需要 cover decode 测试和 smoke。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [24] | Blocked By: [22]

  **References**:
  - Smoke pattern: `test/security/security_smoke.cpp:77-145`。
  - Audit: `ANALYSIS.md:406-412`。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-011` exit code `0`，stdout 包含 `TR-AUDIT-011 PASS`。
  - [ ] `TagReaderSecuritySmoke` 对正常封面样本 exit code `0`。
  - [ ] commit 成功且只包含 CoverDecoder 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: malformed image 被跳过
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-011
    Expected: exit code 0；stdout 包含 malformed-image-skipped 和 valid-image-exported。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-011/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/cover/CoverDecoder.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-011): 检查封面缩放结果"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-011)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-011/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-011): 检查封面缩放结果` | Files: [`src/cover/CoverDecoder.cpp`, `test/regression/regression_tests.cpp`]

- [x] 24. 修复 TR-AUDIT-012：按 ID3v2.4 extended header 规范计算 frame 起点

  **What to do**: 在 `src/formats/id3/Id3Frames.cpp` 的 `PrepareId3v24FrameRegion()` 重新实现 extended header size 处理：按 ID3v2.4.0 规范，extended header size 字段为 syncsafe 32-bit，表示 extended header 总大小（包含 size 字段本身）。因此 frame cursor 固定前进 `extSize`，不是 `4 + extSize`。增加边界：`extSize >= 6`（size + flag bytes + data），`cursor + extSize <= limit`；不实现非规范 extSize fallback。
  **Must NOT do**: 不破坏无 extended header 的 v2.4；不影响 v2.3 extended header；不盲目双模式扫描导致错误接受垃圾。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 规范兼容修复，需谨慎处理 v2.4。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [25] | Blocked By: [23]

  **References**:
  - Audit: `ANALYSIS.md:414-446` - TR-AUDIT-012。
  - Pattern: `src/formats/id3/Id3Frames.cpp` - `PrepareId3v24FrameRegion()`、`ReadId3TagBytes()`。

  **Acceptance Criteria**:
  - [ ] 标准 v2.4 extended header 后紧跟 `TIT2` 可解析。
  - [ ] 无 extended header 的 v2.4 样本不变。
  - [ ] 越界/过小 extSize 被拒绝，不错位解析。

  **QA Scenarios**:
  ```
  Scenario: 标准 v2.4 extended header 解析
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-012
    Expected: exit code 0；stdout 包含 TR-AUDIT-012 PASS；title == "v24-extended"。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-012/pass.txt

  Scenario: malformed ext header 被拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-012
    Expected: extSize 过小/越界样本不解析错位 frame；stdout 包含 malformed-ext-header-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-012/malformed.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-012): 修正ID3v24扩展头偏移` | Files: [`src/formats/id3/Id3Frames.cpp`]

- [x] 25. 验证并提交 TR-AUDIT-012

  **What to do**: 在 regression test 中实现 `TR-AUDIT-012`。生成 ID3v2.4 tag：标准 extended header（size 包含自身）后紧跟 `TIT2`；无 extended header 对照；extSize 过小/越界异常。断言标准样本 title 可读，异常样本不产生错位字段。
  **Must NOT do**: 不改产品代码；不依赖外部固定样本；不把非规范 fallback 当主要成功路径。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - ID3v2.4 binary sample 构造。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [26] | Blocked By: [24]

  **References**:
  - Corpus pattern: `test/corpus/generate_corpus.py` - ID3 extended header 样本。
  - Audit: `ANALYSIS.md:440-446`。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-012` exit code `0`，stdout 包含 `TR-AUDIT-012 PASS`。
  - [ ] commit 成功且只包含 ID3 frame region 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: v2.4 extended header corpus
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-012
    Expected: exit code 0；stdout 包含 standard-ext-header-ok 和 malformed-ext-header-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-012/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/formats/id3/Id3Frames.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-012): 修正ID3v24扩展头偏移"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-012)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-012/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-012): 修正ID3v24扩展头偏移` | Files: [`src/formats/id3/Id3Frames.cpp`, `test/regression/regression_tests.cpp`]

- [x] 26. 修复 TR-AUDIT-013：封面缓存使用 SHA-256 content address 并强化污染错误策略

  **What to do**: 在 `src/cover/CoverCache.cpp` 替换 `HashEmbeddedImageBytes()` 的双 FNV 风格 hash 为 SHA-256。固定使用 FFmpeg/libavutil 已有依赖中的 `AVSHA`（包含 `libavutil/sha.h`）计算 32-byte digest 并编码 64 hex 字符；保持 `BuildCoverCachePath()` 的 `first2hex/rest.png` shard 格式。保留 `ValidateExistingCoverCacheFile()` 的逐字节校验。对已存在但校验失败的 cache 文件，继续报告 `cover cache` 和路径；不得静默复用错误文件。目录层级 hardening 不在本任务做 `openat` 重写，只在错误信息和 hash 强度上修复本项核心风险。
  **Must NOT do**: 不新增 OpenSSL/BLAKE3 外部依赖；不改变未传 `coverExportDir` 时无副作用；不删除既有污染检测；不把 cache 污染静默降级为成功。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 涉及 hash 格式与缓存路径兼容。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [27] | Blocked By: [25]

  **References**:
  - Audit: `ANALYSIS.md:448-480` - TR-AUDIT-013。
  - Pattern: `src/cover/CoverCache.cpp` - `HashEmbeddedImageBytes()`、`BuildCoverCachePath()`、`ValidateExistingCoverCacheFile()`、`AtomicWriteFileIfAbsent()`。
  - Dependency: `CMakeLists.txt:40-77` - 已链接 libavutil，可用 `AVSHA`。

  **Acceptance Criteria**:
  - [ ] 新 cover cache key 为 64 hex SHA-256 digest，路径仍为 `coverExportDir / first2hex / rest.png`。
  - [ ] 已存在污染 cache 文件仍被拒绝且错误包含 `cover cache` 和路径。
  - [ ] 相同图片重复读取复用同一路径且不重写。

  **QA Scenarios**:
  ```
  Scenario: SHA-256 cache path 稳定
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-013
    Expected: exit code 0；stdout 包含 TR-AUDIT-013 PASS；cover filename hex 长度符合 SHA-256 shard。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-013/pass.txt

  Scenario: 污染 cache 被拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-013
    Expected: 预置错误 PNG/cache 文件后 Read 抛出包含 cover cache 和路径的错误；stdout 包含 polluted-cache-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-013/pollution.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-013): 强化封面缓存哈希` | Files: [`src/cover/CoverCache.cpp`]

- [x] 27. 验证并提交 TR-AUDIT-013

  **What to do**: 在 regression test 中实现 `TR-AUDIT-013`。使用测试代码内固定的 1x1 PNG bytes，并在测试源码中硬编码该 byte 序列的预期 SHA-256 hex；生成含该封面的样本后，断言 coverPath 的 shard 路径严格等于该 SHA-256 的 `first2hex/rest.png`；预置污染文件后断言拒绝。运行 `TagReaderSecuritySmoke`。
  **Must NOT do**: 不改产品代码；不提交封面 fixture；不跳过污染测试。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 文件系统 cache 断言。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [28] | Blocked By: [26]

  **References**:
  - Smoke pattern: `test/security/security_smoke.cpp:36-75` - polluted cache diagnostic。
  - Audit: `ANALYSIS.md:474-480`。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-013` exit code `0`，stdout 包含 `TR-AUDIT-013 PASS`。
  - [ ] `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_covers /tmp/opencode/tagreader_regression/TR-AUDIT-013/cover_sample.mp3` exit code `0`。
  - [ ] commit 成功且只包含 CoverCache 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: SHA-256 path 与污染拒绝
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-013
    Expected: exit code 0；stdout 包含 sha256-cache-path 和 polluted-cache-rejected。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-013/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/cover/CoverCache.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-013): 强化封面缓存哈希"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-013)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-013/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-013): 强化封面缓存哈希` | Files: [`src/cover/CoverCache.cpp`, `test/regression/regression_tests.cpp`]

- [x] 28. 修复 TR-AUDIT-014：减少封面解码重复拷贝并限制 fallback 预算

  **What to do**: 在 `src/cover/CoverDecoder.cpp` 移除 `ReadImageBytes()` 中 `packet -> vector -> packet` 的双重拷贝路径。固定方案：删除中间 vector 返回值，让 decoder 接收原始输入 bytes 并只创建一次 `AVPacket`。新增 fallback decode budget：未知 magic 时最多尝试 2 个 fallback codec；已识别 magic 时只尝试对应 codec；累计失败后返回空封面。
  **Must NOT do**: 不降低正常 PNG/JPEG 支持；不扩大 `kMaxCoverInputBytes`；不引入全局状态；不改变输出 PNG 校验。

  **Recommended Agent Profile**:
  - Category: `deep` - Cover decoder resource path 涉及 FFmpeg packet 生命周期。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [29] | Blocked By: [27]

  **References**:
  - Audit: `ANALYSIS.md:482-514` - TR-AUDIT-014。
  - Pattern: `src/cover/CoverDecoder.cpp` - `ReadImageBytes()`、`ConvertImageToPng()`、fallback codec 列表。

  **Acceptance Criteria**:
  - [ ] 源码不再出现同一输入封面先复制到 packet、再复制到 vector、再复制到 packet 的链路。
  - [ ] 未知/畸形 magic 的 fallback 尝试次数受固定预算限制。
  - [ ] 正常 PNG/JPEG 封面仍导出。

  **QA Scenarios**:
  ```
  Scenario: 正常封面解码保持成功
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-014
    Expected: exit code 0；stdout 包含 TR-AUDIT-014 PASS；正常 PNG/JPEG coverPath 存在。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-014/pass.txt

  Scenario: 畸形大封面 fallback 预算生效
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-014
    Expected: 生成 2MiB 未知 magic payload；返回空 coverPath；stdout 包含 fallback-budget-enforced。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-014/fallback.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-014): 限制封面解码放大` | Files: [`src/cover/CoverDecoder.cpp`]

- [x] 29. 验证并提交 TR-AUDIT-014

  **What to do**: 在 regression test 中实现 `TR-AUDIT-014`。生成正常小 PNG/JPEG 封面样本和 2MiB 未知 magic payload 样本。断言正常成功、畸形 payload 快速失败且不生成 PNG。产品代码必须在 fallback budget 触发时通过测试可见路径返回空封面；测试固定断言 `coverPath.empty()` 且 stdout 包含 `fallback-budget-enforced`。
  **Must NOT do**: 不改产品代码；不依赖人工性能判断；不生成 64MiB 样本，测试 payload 固定为 2MiB。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Cover decode regression 测试。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [30] | Blocked By: [28]

  **References**:
  - Audit: `ANALYSIS.md:508-514` - fuzz 可达性和修复建议。
  - Smoke pattern: `test/security/security_smoke.cpp` - cover path 断言。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-014` exit code `0`，stdout 包含 `TR-AUDIT-014 PASS`。
  - [ ] commit 成功且只包含 CoverDecoder 与 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: fallback decode 预算验证
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-014
    Expected: exit code 0；stdout 包含 fallback-budget-enforced 和 malformed-cover-skipped。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-014/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add src/cover/CoverDecoder.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-014): 限制封面解码放大"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-014)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-014/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-014): 限制封面解码放大` | Files: [`src/cover/CoverDecoder.cpp`, `test/regression/regression_tests.cpp`]

- [x] 30. 修复 TR-AUDIT-015：移除 MusicTag 字段的 boost::flyweight 隐藏共享状态

  **What to do**: 在 `include/Tag.hpp` 将 `MusicTag` 的文本字段类型从 `boost::flyweight<std::string>` 改为普通 `std::string`。删除 `#include <boost/flyweight.hpp>`。所有文本 getter 固定返回 `const std::string&`；setter/构造写入路径保持 UTF-8 字符串内容不变。使用 `lsp_find_references` 和 grep 查找所有 `.get()`/隐式 flyweight 用法并同步修正。
  **Must NOT do**: 不新增 `MusicTag` 字段；不改变字段名、getter 名、lyrics/coverPath 类型；不引入新的全局 intern pool；不扩大 public API。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 公开头文件类型变化，需全库引用检查。
  - Skills: [] - 无额外技能。
  - Omitted: [`webapp-testing`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [31] | Blocked By: [29]

  **References**:
  - Audit: `ANALYSIS.md:516-548` - TR-AUDIT-015。
  - Pattern: `include/Tag.hpp` - `MusicTag` 文本字段。
  - Pattern: `src/core/TagPipeline.cpp` - `BuildMusicTag()` 最终写入路径。

  **Acceptance Criteria**:
  - [ ] `include/Tag.hpp` 不再 include 或使用 `boost::flyweight`。
  - [ ] 公开读取 title/artist/album 等字段的行为保持一致。
  - [ ] 并发读取大量不同字符串样本通过普通 regression；`TAGREADER_ENABLE_SANITIZERS=ON` 的 ASAN/UBSAN 构建也通过同一 case。

  **QA Scenarios**:
  ```
  Scenario: 文本字段行为保持一致
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-015
    Expected: exit code 0；stdout 包含 TR-AUDIT-015 PASS；title/artist/album/composer 字段值与输入一致。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-015/pass.txt

  Scenario: 并发不同字符串读取稳定
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-015
    Expected: 16 个 worker 并发读取不同 metadata 样本；无异常；stdout 包含 concurrent-strings-ok。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-015/concurrency.txt
  ```

  **Commit**: NO | Message: `fix(TR-AUDIT-015): 移除MusicTag共享flyweight` | Files: [`include/Tag.hpp`, `src/core/TagPipeline.cpp`]

- [x] 31. 验证并提交 TR-AUDIT-015

  **What to do**: 在 regression test 中实现 `TR-AUDIT-015`。动态生成多个带不同 title/artist/album/composer 的样本，并发调用 `TagReader::Read()`，断言返回字段准确且互不串扰。固定额外运行 `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize && ./build-sanitize/TagReaderRegressionTests TR-AUDIT-015`。
  **Must NOT do**: 不改产品代码；不把 TSAN 当强制要求（CMake 当前只有 ASAN/UBSAN），但必须执行并发 regression；不跳过引用检查。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - 公开模型回归和并发测试。
  - Skills: [] - 无额外技能。
  - Omitted: [`frontend-design`] - 非 UI。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: [Final Verification Wave] | Blocked By: [30]

  **References**:
  - Audit: `ANALYSIS.md:542-548` - TSan/并发测试建议。
  - Smoke pattern: `test/security/security_smoke.cpp:106-130` - 并发 future 读取模式。
  - CMake sanitizer: `CMakeLists.txt:10-37`。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-015` exit code `0`，stdout 包含 `TR-AUDIT-015 PASS`。
  - [ ] `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON` 和 `cmake --build build-sanitize` 通过，`./build-sanitize/TagReaderRegressionTests TR-AUDIT-015` exit code `0`。
  - [ ] commit 成功且只包含 `Tag.hpp`、必要调用点和 regression test 改动。

  **QA Scenarios**:
  ```
  Scenario: 并发 MusicTag 字段稳定
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests TR-AUDIT-015
    Expected: exit code 0；stdout 包含 concurrent-strings-ok 和 TR-AUDIT-015 PASS。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-015/stdout.txt

  Scenario: commit 边界干净
    Tool: Bash
    Steps: git status --short && git diff --stat && git add include/Tag.hpp src/core/TagPipeline.cpp test/regression/regression_tests.cpp && git commit -m "fix(TR-AUDIT-015): 移除MusicTag共享flyweight"
    Expected: git log --oneline -1 包含 fix(TR-AUDIT-015)。
    Evidence: /tmp/opencode/tagreader_regression/TR-AUDIT-015/commit.txt
  ```

  **Commit**: YES | Message: `fix(TR-AUDIT-015): 移除MusicTag共享flyweight` | Files: [`include/Tag.hpp`, `src/core/TagPipeline.cpp`, `test/regression/regression_tests.cpp`]

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE. Verification is fully agent-executed with no human intervention required for pass/fail.

- [x] F1. Plan Compliance Audit — oracle

  **Tool**: `task(subagent_type="oracle")`
  **Steps**:
  1. 读取 `.omo/plans/analysis-bugfix-plan.md`、`ANALYSIS.md`、`git log --oneline -20`。
  2. 核对是否存在 1 个测试基建 commit 和 15 个 `fix(TR-AUDIT-XXX): ...` commit。
  3. 核对每个 `TR-AUDIT-001` 到 `TR-AUDIT-015` 均完成对应修复任务和验证任务。
  4. 核对每个验证任务都有 evidence 目录 `/tmp/opencode/tagreader_regression/TR-AUDIT-XXX/`。
  **Expected**: Oracle 输出 `APPROVE`；若任一 TR 缺少 commit、测试或 evidence，输出 `REJECT` 并列出缺口。
  **Evidence**: `/tmp/opencode/tagreader_regression/final/F1-plan-compliance.txt`

- [x] F2. Code Quality Review — unspecified-high

  **Tool**: `task(category="unspecified-high")`
  **Steps**:
  1. 审查 `git diff HEAD~16..HEAD -- src include test CMakeLists.txt`。
  2. 检查是否引入 Catch2/GoogleTest/CI/lint/formatter 或大型二进制 fixture。
  3. 检查 parser 修复是否限制在计划指定文件和相邻 helper，不包含无关重构。
  4. 检查新增 `TagReaderRegressionTests` 是否保持轻量 main/dispatch/helper 结构。
  **Expected**: 输出 `APPROVE`；若发现无关重构、测试框架依赖、二进制 fixture、或未计划 public API 扩展，输出 `REJECT`。
  **Evidence**: `/tmp/opencode/tagreader_regression/final/F2-code-quality.txt`

- [x] F3. Agent-Executed Runtime QA — unspecified-high

  **Tool**: `Bash` + `task(category="unspecified-high")`
  **Steps**:
  1. `cmake -S . -B build`
  2. `cmake --build build`
  3. `./build/TagReaderRegressionTests --list`
  4. 依次运行：`./build/TagReaderRegressionTests TR-AUDIT-001`、`TR-AUDIT-002`、`TR-AUDIT-003`、`TR-AUDIT-004`、`TR-AUDIT-005`、`TR-AUDIT-006`、`TR-AUDIT-007`、`TR-AUDIT-008`、`TR-AUDIT-009`、`TR-AUDIT-010`、`TR-AUDIT-011`、`TR-AUDIT-012`、`TR-AUDIT-013`、`TR-AUDIT-014`、`TR-AUDIT-015`。
  5. 运行 `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize && ./build-sanitize/TagReaderRegressionTests TR-AUDIT-015`。
  6. 对 `TR-AUDIT-004`、`TR-AUDIT-005`、`TR-AUDIT-011`、`TR-AUDIT-013` 生成的样本运行对应 `TagReaderSecuritySmoke` 命令。
  **Expected**: 所有命令 exit code `0`；`--list` 包含 15 个 TR 编号；每个单项 stdout 包含 `TR-AUDIT-XXX PASS`；security smoke exit code `0`。
  **Evidence**: `/tmp/opencode/tagreader_regression/final/F3-runtime-qa.txt`

- [x] F4. Scope Fidelity Check — deep

  **Tool**: `task(category="deep")`
  **Steps**:
  1. 对比 `ANALYSIS.md` 的 Bug Report 与最终 commits，确认只修复 `TR-AUDIT-001` 到 `TR-AUDIT-015`。
  2. 检查公开入口仍只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`。
  3. 检查未把元数据来源改成 FFmpeg `AVDictionary`，文本字段仍保证 UTF-8。
  4. 检查封面导出仍仅在传入 `coverExportDir` 时产生文件系统副作用。
  **Expected**: 输出 `APPROVE`；若发现新格式支持、额外 public API、无关架构重写、或副作用边界破坏，输出 `REJECT`。
  **Evidence**: `/tmp/opencode/tagreader_regression/final/F4-scope-fidelity.txt`

## Commit Strategy
- 任务 1 是全局前置测试基建提交，不计入 15 个 bug commit；提交消息为 `test: 添加回归测试入口`。
- 之后每个 `TR-AUDIT-*` 独立提交，格式：`fix(TR-AUDIT-XXX): 修复<中文摘要>`。
- 每个验证任务负责提交；修复任务不提交。
- 每次提交前必须检查 `git status --short`、`git diff --stat`、`git diff`。
- 禁止使用 `git add .`；必须显式列文件。
- 如果验证失败或工作树含无关文件，停止并报告，不 commit。

## Success Criteria
- `TR-AUDIT-001` 到 `TR-AUDIT-015` 均有独立修复、独立 regression case、独立 commit。
- `./build/TagReaderRegressionTests --list` 覆盖 15 项。
- `./build/TagReaderRegressionTests TR-AUDIT-001` ... `TR-AUDIT-015` 全部 exit code `0`。
- `cmake -S . -B build && cmake --build build` 全部通过。
- 封面相关 smoke 通过。
- Final Verification Wave 四项全部 APPROVE。
