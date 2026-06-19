# TagReader 漏洞修复 TDD 执行计划

## TL;DR
> **Summary**: 将 `ANALYSIS.md`“漏洞与缺陷详情”中的 5 个问题转化为可执行修复任务：2 个可并行实现任务、3 个必须串行的文件系统/IO 边界任务。所有任务必须先写红灯回归，再最小修复、绿灯验证、原子提交。
> **Deliverables**:
> - 修复默认封面导出目录 symlink 劫持。
> - 修复显式 `coverExportDir` symlink 边界缺陷，安全默认改为拒绝目录 symlink。
> - 修复输入路径 TOCTOU / split-brain，确保 raw parser 与 FFmpeg 固定同一文件对象。
> - 修复 FLAC metadata 块 fail-fast 放大局部损坏。
> - 修复 `NormalizeLyrics()` 同 timestamp 去重 O(n²)。
> - 为每项修复增加动态资产驱动的 C++ 回归验证。
> **Effort**: Large
> **Parallel**: YES - 2 个实现可并行任务 + 3 个串行任务；同一工作区提交动作必须串行。
> **Critical Path**: S1 默认封面目录加固 → S2 显式封面目录 symlink 策略 → S3 输入 FD/同对象 IO 加固 → Final Verification

## Context

### Original Request
用户要求读取项目根目录 `ANALYSIS.md` 中“漏洞与缺陷详情”，提取所有 Bug 和架构缺陷，生成严谨、拓扑依赖感知、TDD 驱动、自动化执行计划。计划必须包含并发/串行调度、手术刀修改范围、动态测试资产规范、每任务 TDD 闭环、原子 commit、禁止回退 Git 命令。

### Interview Summary
无需进一步访谈。用户已指定输入来源、执行规则、测试资产目录策略、Git 权限边界和输出结构。计划采用安全默认：显式 `coverExportDir` 目录 symlink 也应拒绝；如果产品要保留旧兼容行为，必须先由用户修改本计划，执行 Agent 不得自行改回“只补文档”。

### Metis Review (gaps addressed)
- 已加入“先红灯再修复”的 TDD 要求，避免只写通过后的回归。
- 已处理并行阶段测试文件冲突：P1/P2 源码可并行，但若都要修改 `test/regression/regression_tests.cpp`，测试注册与提交不得并行；优先新增独立测试源文件并修改 `CMakeLists.txt`，否则按 P1 后 P2 串行注册。
- 已明确 cover dir 策略：默认目录拒绝 symlink；显式目录也拒绝 symlink，替代现有 `TR-AUDIT-031 symlink-dir-accepted` 期望。
- 已明确输入 TOCTOU 修复牵动 `ReadContext::input`、`ReadRange()`、`OpenContext()` 与 FFmpeg 打开合约，必须串行。
- 已加入每次 commit 前 `git status --short` 审核，防止提交 `/tmp`、`./tmp` 生成媒体、构建产物或无关文件。

## Work Objectives

### Core Objective
用最小代码改动修复 `ANALYSIS.md` 中 5 个已确认问题，并用动态生成资产和 C++ 回归测试证明每项修复生效且不破坏既有核心行为。

### Deliverables
- 代码修复：`src/core/TagPipeline.cpp`、`src/media/FfmpegSession.cpp`、必要的 IO/context 头文件、`src/formats/flac/FlacParser.cpp`、`src/text/TextNormalize.cpp`。
- 测试修复：回归测试 case 或独立回归测试可执行程序，所有测试资产动态生成。
- 原子提交：每个任务一个 commit，commit message 使用 `fix: <任务描述>`。

### Definition of Done (verifiable conditions with commands)
- `cmake -S . -B build` 成功。
- `cmake --build build` 成功。
- 每个新增/修改 case 可用 `./build/TagReaderRegressionTests <TR-AUDIT-case-id>` 或新增可执行程序单独运行并 PASS。
- 既有相关 case `./build/TagReaderRegressionTests TR-AUDIT-029` 与 `./build/TagReaderRegressionTests TR-AUDIT-031` 按新期望 PASS。
- `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_smoke_covers /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3` PASS；若样本缺失，先运行 `python3 test/security/generate_samples.py`。
- 最终 `git status --short` 不包含未提交代码改动；不得包含生成媒体、`/tmp` 资产、`./tmp` 抛弃资产或构建产物。

### Must Have
- 每个任务必须先产生红灯：新增或修改测试先失败，确认能复现对应漏洞/缺陷，再改代码。
- 每个任务必须执行“修改代码 → 动态生成测试资产与 C++ 测试 → 编译运行 → 失败则当前任务内自旋修复 → 绿灯后 `git add`/`git commit`”。
- 测试资产必须由脚本、C++ 测试代码或 FFmpeg 动态生成；严禁寻找或依赖仓库中不存在的现成音频。
- 一次性脚本/中间音频/抛弃型产物放 `/tmp`；需要跨自旋复用的生成脚本、复用资源或状态文件放项目根 `./tmp`，不存在则创建。
- 仅允许提交代码、测试源码、必要的构建配置；生成音频和临时脚本不得提交。
- 每个任务提交前必须运行 `git status --short` 并人工/Agent 审核 staged 文件列表。

### Must NOT Have
- 禁止执行 `git reset`、`git revert`、`git checkout --` 或任何回退式命令；错误只能前进式修复并新增 commit。
- 禁止重构无关 parser、引入 CI、formatter、lint 框架、长时间 fuzz、全局架构重写。
- 禁止把实现降到 C++17/C++20；项目是 C++23。
- 禁止依赖 `ctest`，仓库没有 `enable_testing()`/`add_test()`。
- 禁止提交生成媒体、构建目录、`/tmp` 内容、`./tmp` 抛弃资产。
- 禁止在并行任务中同时修改同一个文件；若测试注册必须修改同一个文件，则该部分串行化。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: TDD + existing executable-style regression tests (`TagReaderRegressionTests`) and targeted temporary C++ harness only when needed.
- QA policy: Every task has agent-executed happy path + failure/edge scenarios.
- Evidence: `.omo/evidence/task-{N}-{slug}.txt`，记录命令、退出码、关键输出；`.omo/` 已被 `.gitignore` 忽略。
- Asset policy: `/tmp` for disposable generated files; project `./tmp` for reusable generators/state; generated media/scripts are not committed unless explicitly promoted to test source code.

## Execution Strategy

### 阶段 1：前置环境与依赖分析

#### 提取的 Bug
1. `ANALYSIS.md:17` 默认封面导出目录使用共享临时目录固定路径，默认 root symlink 可被劫持。
2. `ANALYSIS.md:48` 输入路径预检与 `OpenContext()` 双重路径打开导致 TOCTOU / split-brain。
3. `ANALYSIS.md:70` 显式 `coverExportDir` 接受 symlink，目录边界依赖外部调用方约定。
4. `ANALYSIS.md:79` FLAC metadata block 局部损坏导致整段 metadata fail-fast。
5. `ANALYSIS.md:87` `NormalizeLyrics()` 同 timestamp 去重 O(n²)。

#### 文件级与逻辑级依赖
- `src/core/TagPipeline.cpp` 同时包含 `DefaultCoverExportDir()`、`ValidateCoverExportDir()`、`ValidatePath()` 与主流程，默认封面目录和显式封面目录必须串行；输入路径修复也会触达同文件，必须排在封面策略稳定之后。
- `src/media/FfmpegSession.cpp`、`src/media/FfmpegSession.hpp`、`src/core/ReadContext.hpp`、`src/io/ByteReader.hpp`、`src/io/ByteReader.cpp` 构成 IO 合约；S3 修改会影响所有 parser 的 absolute offset 读取，必须单独串行。
- `src/formats/flac/FlacParser.cpp` 的局部错误策略与 `src/text/TextNormalize.cpp` 的歌词算法互不影响，可并行实现。
- `test/regression/regression_tests.cpp` 是现有回归入口，多个任务若都修改该文件会冲突；并行任务优先新增独立测试源文件和 CMake target，或把测试注册拆为串行小步骤。

### Parallel Execution Waves
> Target: 5-8 tasks per wave. 本计划只有 5 个修复项，且 3 个共享核心边界文件，不能强行并行。

Wave P: P1 FLAC fail-fast、P2 Lyrics O(n²) 可并行实现；若共享 `test/regression/regression_tests.cpp`，测试注册/提交必须串行。
Wave S: S1 默认封面目录、S2 显式封面目录、S3 输入 TOCTOU 必须严格串行。
Wave F: 四路最终审查并行。

### Dependency Matrix (full, all tasks)
- P1 FLAC fail-fast: Blocks none; Blocked by none; conflicts only if modifying shared regression file.
- P2 Lyrics O(n²): Blocks none; Blocked by none; conflicts only if modifying shared regression file.
- S1 默认封面目录: Blocks S2; Blocked by none; modifies `src/core/TagPipeline.cpp`.
- S2 显式封面目录: Blocks S3; Blocked by S1; modifies `src/core/TagPipeline.cpp` and `TR-AUDIT-031` expectations.
- S3 输入 TOCTOU: Blocks Final; Blocked by S2; modifies IO context contract and broad parser read path.

### Agent Dispatch Summary (wave → task count → categories)
- Wave P → 2 tasks → `quick` 或 `unspecified-high`；P2 如做性能基准用 `unspecified-high`。
- Wave S → 3 tasks → S1/S2 `unspecified-high`，S3 `deep` 或 `unspecified-high` 因 IO 合约复杂。
- Wave F → 4 review tasks → oracle / unspecified-high / unspecified-high / deep。

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST follow: red test first, minimal fix, green verification, atomic commit.
> EVERY task MUST warn: only modify strongly related code; no unrelated refactor; if function signatures force coupled edits, make only minimal compile/logical edits.

### 阶段 2：并行修复组 (Parallel Execution Stage)

- [x] P1. FLAC metadata 块局部损坏降级为跳过当前块

  **What to do**: 先新增红灯回归，动态构造一个 FLAC：早期 Vorbis comment metadata block 被截断/非法，后续仍有合法 PICTURE 或合法字段块；当前行为应导致后续合法 metadata 缺失。然后修改 `ReadFlacMetadataBlocks()`，把单个 Vorbis comment block 的读取失败/非法格式/截断块降级为跳过当前块并继续扫描后续 block；`cover cache` / `cover export` 错误不得吞掉，仍需保留上抛信号。
  **Must NOT do**: 不得重写 FLAC parser 状态机；不得改变 ID3/Ogg/MP4/APE 行为；不得吞掉 `WriteCoverAsPng()` 相关错误；不得改 public API。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 需要构造二进制 FLAC metadata 并区分局部错误与 cover cache 错误。
  - Skills: [] - 无需专用技能。
  - Omitted: `security-research` - 已有漏洞报告，不需要重新审计。

  **Parallelization**: Can Parallel: YES with P2 only if files do not overlap | Wave P | Blocks: none | Blocked By: none

  **涉及代码文件**:
  - `src/formats/flac/FlacParser.cpp`
  - 测试文件优先新增独立源：`test/regression/flac_malformed_metadata_tests.cpp` 并在 `CMakeLists.txt` 新增 target；若必须改 `test/regression/regression_tests.cpp`，该测试注册/commit 必须与 P2 串行。

  **References**:
  - Pattern: `src/formats/flac/FlacParser.cpp:164` - `ReadFlacMetadataBlocks()` 当前遇到坏块会 throw。
  - Pattern: `src/formats/flac/FlacParser.cpp:209` - Vorbis comment block 读取失败位置。
  - Pattern: `src/formats/flac/FlacParser.cpp:234` - PICTURE block 读取与 cover 写入位置，cover 错误不得吞。
  - Test: `test/regression/regression_tests.cpp:3841` - 现有 TR-AUDIT case 风格和动态样本生成模式。
  - Build: `CMakeLists.txt:122` - 现有 `TagReaderRegressionTests` target。

  **执行指令**:
  1. 【红灯测试】在 C++ 测试中动态写入最小 FLAC 样本；一次性音频放 `/tmp/tagreader-p1-*`，若编写生成脚本且需多次复用则放 `./tmp/flac_malformed_generator.py`，但默认优先 C++ 内构造字节，避免提交脚本。
  2. 【确认失败】构建并运行新增 case，确认当前代码不能读取坏块后的合法 metadata/cover，记录 `.omo/evidence/task-P1-red.txt`。
  3. 【修改代码】只改 `ReadFlacMetadataBlocks()` 附近逻辑，把局部 malformed block 转成 `cursor = blockEnd; continue`；cover 写入失败保持异常。
  4. 【运行验证】`cmake --build build`，运行新增 P1 case；失败则在 P1 内 debug 并前进式修复，直到绿灯。
  5. 【回归验证】运行至少 `./build/TagReaderRegressionTests TR-AUDIT-031` 或新增 target 的等价命令，确保封面路径仍正常。
  6. 【原子提交】执行 `git status --short`，只 stage `src/formats/flac/FlacParser.cpp`、测试源码、必要 `CMakeLists.txt`；执行 `git add <涉及文件>` 和 `git commit -m "fix: tolerate malformed flac metadata blocks"`。

  **Acceptance Criteria**:
  - [ ] 红灯阶段能证明坏 FLAC metadata block 会阻断后续合法 metadata/cover。
  - [ ] 修复后同一样本不顶层失败，合法后续 metadata/cover 可读取或至少坏块不清空后续扫描机会。
  - [ ] cover cache/export 错误仍会暴露，不被 P1 的局部容错吞掉。
  - [ ] 生成音频与临时脚本未被 git stage。

  **QA Scenarios**:
  ```
  Scenario: Malformed FLAC comment block followed by valid metadata
    Tool: Bash
    Steps: Build, run P1 regression case that writes a generated FLAC under /tmp/tagreader-p1-* and calls TagReader::Read().
    Expected: Before fix red; after fix PASS with later valid field/cover observed.
    Evidence: .omo/evidence/task-P1-flac-malformed.txt

  Scenario: Cover export failure still propagates
    Tool: Bash
    Steps: Run P1 case with non-writable/invalid cover export dir for a FLAC PICTURE block.
    Expected: Failure signal still contains cover/cache/export context; no silent success.
    Evidence: .omo/evidence/task-P1-cover-error.txt
  ```

  **Commit**: YES | Message: `fix: tolerate malformed flac metadata blocks` | Files: `src/formats/flac/FlacParser.cpp`, selected test source, maybe `CMakeLists.txt`

- [x] P2. `NormalizeLyrics()` 同 timestamp 去重改为非平方复杂度

  **What to do**: 先新增红灯性能/复杂度测试，直接构造 `RawLyrics` 中 5k/10k/20k 条相同 timestamp 且文本不同的 timed lines，调用 `NormalizeLyrics()`，证明旧实现耗时随规模近似平方增长或超过保守阈值。然后将去重改为排序键 `(timestamp, text)` 后线性 unique，或按 timestamp 分组使用 `std::unordered_set<std::string_view>`/`std::unordered_set<std::string>`；必须保持输出按 timestamp 稳定且去掉同 timestamp 重复文本。
  **Must NOT do**: 不得改变普通文本歌词清洗规则；不得改变 `kMaxLyricLines` 上限；不得改 metadata normalization；不得添加全局性能框架。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 需要性能回归设计，避免脆弱 wall-clock 测试。
  - Skills: [] - 无需专用技能。
  - Omitted: `security-research` - 已有漏洞报告，不需要重新审计。

  **Parallelization**: Can Parallel: YES with P1 only if test file/CMake edits do not overlap | Wave P | Blocks: none | Blocked By: none

  **涉及代码文件**:
  - `src/text/TextNormalize.cpp`
  - 测试文件优先新增独立源：`test/regression/lyrics_normalize_complexity_tests.cpp` 并在 `CMakeLists.txt` 新增 target；若必须改 `test/regression/regression_tests.cpp`，该测试注册/commit 必须与 P1 串行。

  **References**:
  - API/Type: `src/text/TextNormalize.hpp:11` - `NormalizeLyrics(RawLyrics &lyrics)` 可直接测试。
  - Pattern: `src/text/TextNormalize.cpp:224` - 当前只按 timestamp 排序。
  - Pattern: `src/text/TextNormalize.cpp:226` - 当前分组后 `any_of` 去重形成 O(n²)。
  - Test: `test/regression/regression_tests.cpp:4485` - 现有 case dispatch 风格，如选择接入主回归。

  **执行指令**:
  1. 【红灯测试】编写 C++ 测试直接构造 `RawLyrics`，不需要真实音频；如果需要保留性能测量状态，放 `./tmp/lyrics-complexity-baseline.json`，否则所有中间输出放 `/tmp/tagreader-p2-*`。
  2. 【确认失败】运行测试确认旧实现超过阈值或 20k/10k 耗时比明显超出线性门限；红灯证据写 `.omo/evidence/task-P2-red.txt`。
  3. 【修改代码】只改 `NormalizeLyrics()` 中 timed lines 排序/去重段；推荐排序 comparator 为 `(timestamp, text)` 后 `std::unique` 删除完全重复 pair，或者每 timestamp 分组 set 去重。
  4. 【运行验证】`cmake --build build`，运行 P2 测试；失败则在 P2 内前进式修复。
  5. 【行为回归】测试必须覆盖重复文本同 timestamp 只保留一条、不同 timestamp 相同文本都保留、空/非法 UTF-8 行仍清理。
  6. 【原子提交】执行 `git status --short`，只 stage `src/text/TextNormalize.cpp`、测试源码、必要 `CMakeLists.txt`；执行 `git add <涉及文件>` 和 `git commit -m "fix: make lyric deduplication linearithmic"`。

  **Acceptance Criteria**:
  - [ ] 红灯阶段证明旧算法在同 timestamp 不同文本大量输入下不可接受或呈平方增长。
  - [ ] 修复后 5k/10k/20k 数据不出现平方级增长，测试阈值保守且不依赖极端硬件假设。
  - [ ] 去重语义保持：同 timestamp + 同 text 去重；不同 timestamp 不互相去重。
  - [ ] `kMaxLyricLines` 仍生效。

  **QA Scenarios**:
  ```
  Scenario: Large same-timestamp lyric set
    Tool: Bash
    Steps: Build, run P2 complexity test constructing RawLyrics in memory with 20000 unique texts at one timestamp.
    Expected: PASS under conservative time/ratio threshold; output size remains 20000 or capped by kMaxLyricLines as expected.
    Evidence: .omo/evidence/task-P2-complexity.txt

  Scenario: Duplicate semantics preserved
    Tool: Bash
    Steps: Run P2 behavior case with duplicate text at same timestamp, same text at different timestamps, empty invalid lines.
    Expected: Only exact same timestamp/text duplicates removed; invalid/empty lines removed.
    Evidence: .omo/evidence/task-P2-behavior.txt
  ```

  **Commit**: YES | Message: `fix: make lyric deduplication linearithmic` | Files: `src/text/TextNormalize.cpp`, selected test source, maybe `CMakeLists.txt`

### 阶段 3：串行修复组 (Sequential Execution Stage)

- [x] S1. 默认封面导出目录改为私有化并拒绝默认 root symlink

  **What to do**: 先新增/修改回归，使 `TMPDIR=/tmp/tagreader-s1-base` 且预置 `tagreader-covers` symlink 指向 attacker dir 时，单参数 `Read(path)` 不会写入 symlink 目标。然后修改默认目录策略：优先 `XDG_RUNTIME_DIR/tagreader-covers`；fallback 到系统临时目录下按 UID 隔离路径，如 `tagreader-covers-$UID`；默认目录创建后必须校验不是 symlink、owner 为当前用户、权限不宽于 `0700` 或主动设置为 `0700`。
  **Must NOT do**: 不得改变显式 `Read(path, coverExportDir)` 行为，本任务只处理单参数默认目录；不得把默认目录改到物理持久 cache 目录；不得改 cover hash 路径算法。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 文件系统权限/symlink 策略需要精确测试。
  - Skills: [] - 无需专用技能。
  - Omitted: `security-research` - 已有 PoC，不需要重新审计。

  **Parallelization**: Can Parallel: NO | Wave S | Blocks: S2 | Blocked By: none

  **涉及代码文件**:
  - `src/core/TagPipeline.cpp`
  - `test/regression/regression_tests.cpp` 或独立 cover export 回归测试源
  - 可能 `CMakeLists.txt`，仅当新增独立测试 target

  **References**:
  - Pattern: `ANALYSIS.md:17` - 漏洞描述与修复建议。
  - Pattern: `src/core/TagPipeline.cpp:37` - 当前 `DefaultCoverExportDir()` 固定返回系统临时目录子目录。
  - Pattern: `src/core/TagPipeline.cpp:96` - 当前 `ValidateCoverExportDir()` 跟随目录 symlink。
  - Test: `test/regression/regression_tests.cpp:3995` - `TR-AUDIT-031` 当前覆盖 default temp cover export。

  **执行指令**:
  1. 【红灯测试】动态生成带封面的 MP3：优先复用 `python3 test/security/generate_samples.py` 生成 `/tmp/opencode/tagreader_security_samples/cover_export_base.mp3`；如脚本不可用，在测试内调用 FFmpeg 生成一次性样本到 `/tmp/tagreader-s1-*`。不得提交生成音频。
  2. 【攻击资产】在 `/tmp/tagreader-s1-tmp` 下预置 `tagreader-covers` symlink 指向 `/tmp/tagreader-s1-hijack`，用环境变量 `TMPDIR=/tmp/tagreader-s1-tmp` 运行单参数 `Read()`。
  3. 【确认失败】旧代码应把 PNG 写入 hijack 目标；记录 `.omo/evidence/task-S1-red.txt`。
  4. 【修改代码】只改默认目录选择与默认目录校验；推荐新增内部 helper，例如 `DefaultCoverExportDir()` 和 `ValidateDefaultCoverExportDir()`，最小连带修改 `ReadTag()` 调用顺序。
  5. 【运行验证】`cmake --build build`，运行 S1 case；断言 hijack 目标无 PNG，返回 cover path 位于 `XDG_RUNTIME_DIR/tagreader-covers` 或 `/tmp/tagreader-covers-$UID`，目录不是 symlink。
  6. 【原子提交】执行 `git status --short`，只 stage S1 相关文件；执行 `git add <涉及文件>` 和 `git commit -m "fix: harden default cover export directory"`。

  **Acceptance Criteria**:
  - [ ] 默认目录仍优先使用 tmpfs/运行时临时区域，不改为持久物理 cache 默认。
  - [ ] 预置 `/tmp/.../tagreader-covers` symlink 不会被单参数 `Read()` 跟随写入。
  - [ ] 默认 fallback 路径包含 UID 或等效 per-user 隔离。
  - [ ] 显式 `coverExportDir` 行为在 S1 后暂不改变；S2 单独处理。

  **QA Scenarios**:
  ```
  Scenario: Default TMPDIR symlink hijack blocked
    Tool: Bash
    Steps: Generate cover MP3 under /tmp, create TMPDIR/tagreader-covers symlink to hijack dir, run default Read() regression.
    Expected: PASS; hijack dir remains without generated PNG; coverPath not under symlink target.
    Evidence: .omo/evidence/task-S1-default-symlink.txt

  Scenario: Normal default export still works
    Tool: Bash
    Steps: Run default Read() with clean TMPDIR or XDG_RUNTIME_DIR and generated cover MP3.
    Expected: PASS; coverPath exists under hardened default directory.
    Evidence: .omo/evidence/task-S1-normal-default.txt
  ```

  **Commit**: YES | Message: `fix: harden default cover export directory` | Files: `src/core/TagPipeline.cpp`, selected test source, maybe `CMakeLists.txt`

- [x] S2. 显式 `coverExportDir` 目录 symlink 改为拒绝

  **What to do**: 必须基于 S1 最新磁盘状态执行。先修改/新增红灯测试，使 `Read(path, symlinkExportDir)` 期望失败，而不是当前 `TR-AUDIT-031 symlink-dir-accepted`。然后修改 `ValidateCoverExportDir()`，对显式目录也使用 `symlink_status()` 拒绝目录 symlink，并保持普通显式目录、非可写目录错误、默认目录正常。
  **Must NOT do**: 不得重新设计 cover cache；不得改变 leaf cache 文件污染防护；不得改 `Read(path)` 默认目录策略，除非 S1 编译失败需要最小修正。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 需要更新既有测试期望并维护 API 安全语义。
  - Skills: [] - 无需专用技能。
  - Omitted: `security-research` - 已有审计结论。

  **Parallelization**: Can Parallel: NO | Wave S | Blocks: S3 | Blocked By: S1

  **涉及代码文件**:
  - `src/core/TagPipeline.cpp`
  - `test/regression/regression_tests.cpp` 中 `TR-AUDIT-031` 或独立 cover export 测试源
  - 可能 `docs/DESIGN.md`，仅当执行者选择同步 public 行为文档；若修改文档也必须同 commit 且仅描述新 symlink 拒绝语义

  **References**:
  - Pattern: `ANALYSIS.md:70` - 显式目录 symlink 边界缺陷。
  - Pattern: `test/regression/regression_tests.cpp:4097` - 当前创建目录 symlink。
  - Pattern: `test/regression/regression_tests.cpp:4116` - 当前期望 symlink accepted，需要改为 rejected。
  - Pattern: `src/core/TagPipeline.cpp:96` - 当前目录校验跟随 symlink。

  **执行指令**:
  1. 【红灯测试】修改 `TR-AUDIT-031` 或新增 case：显式 symlink 目录必须失败，错误文本需包含 `symlink` 或 `symbolic link` 或 `cover export directory`。
  2. 【确认失败】旧代码应输出/表现为 accepted；记录 `.omo/evidence/task-S2-red.txt`。
  3. 【修改代码】只改 `ValidateCoverExportDir()` 及必要 helper，使用 `std::filesystem::symlink_status()` 先拒绝 symlink，再创建/检查普通目录；保持探针文件写入检测。
  4. 【运行验证】`cmake --build build`；运行 `./build/TagReaderRegressionTests TR-AUDIT-031`，确认 default/explicit/non-writable/symlink 子场景按新期望 PASS。
  5. 【文档同步可选但受限】若 `docs/DESIGN.md` 明确说显式目录 symlink 接受，必须最小改为“显式目录 symlink 被拒绝”；不得扩写无关架构。
  6. 【原子提交】执行 `git status --short`，只 stage S2 相关文件；执行 `git add <涉及文件>` 和 `git commit -m "fix: reject symlink cover export directories"`。

  **Acceptance Criteria**:
  - [ ] `Read(path, symlinkExportDir)` 失败且不写入 symlink 目标。
  - [ ] `Read(path, normalExplicitDir)` 仍成功导出封面。
  - [ ] 非可写显式目录仍失败或保持现有 platform-skip 逻辑。
  - [ ] `TR-AUDIT-031` 不再报告 `symlink-dir-accepted` 作为 PASS 条件。

  **QA Scenarios**:
  ```
  Scenario: Explicit symlink export dir rejected
    Tool: Bash
    Steps: Run TR-AUDIT-031 or S2 case with generated cover MP3 and symlink export directory.
    Expected: PASS; symlink dir rejected; symlink target has no generated PNG.
    Evidence: .omo/evidence/task-S2-explicit-symlink.txt

  Scenario: Explicit normal export dir still accepted
    Tool: Bash
    Steps: Run same case with a real directory under /tmp/tagreader-s2-export.
    Expected: PASS; coverPath exists under explicit real directory.
    Evidence: .omo/evidence/task-S2-explicit-normal.txt
  ```

  **Commit**: YES | Message: `fix: reject symlink cover export directories` | Files: `src/core/TagPipeline.cpp`, selected test/doc files

- [x] S3. 输入路径打开改为固定同一文件对象，消除 TOCTOU / split-brain

  **What to do**: 必须基于 S2 最新磁盘状态执行。先新增红灯 race/split-brain 回归：构造同一路径在检查和打开之间被替换的场景，或构造能证明 raw parser 与 FFmpeg 可能读取不同对象的可控 harness。然后用描述符语义固定文件对象：POSIX 下 `open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)`，`fstat()` 确认 regular file，file size/mtime 来自同一 fd；raw-byte 读取和 FFmpeg probe 必须基于同一已打开对象。
  **Must NOT do**: 不得新增独立 `DetectContainer()`；不得把 metadata/title/lyrics 改为依赖 FFmpeg `AVDictionary`；不得重写所有 parser；不得改变 public `TagReader::Read()` 签名。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 牵动底层 IO 合约、FFmpeg 打开方式和所有 parser 的 `ReadRange()` 使用。
  - Skills: [] - 无需专用技能。
  - Omitted: `security-research` - 已有漏洞报告，不需要重新审计。

  **Parallelization**: Can Parallel: NO | Wave S | Blocks: Final | Blocked By: S2

  **涉及代码文件**:
  - `src/core/TagPipeline.cpp`
  - `src/media/FfmpegSession.cpp`
  - `src/media/FfmpegSession.hpp`
  - `src/core/ReadContext.hpp`
  - `src/io/ByteReader.hpp`
  - `src/io/ByteReader.cpp`
  - 必要测试源；如新增 POSIX helper 源/头，必须最小化

  **References**:
  - Pattern: `ANALYSIS.md:48` - TOCTOU / split-brain 描述。
  - API/Type: `src/core/ReadContext.hpp:34` - 当前保存 `std::ifstream input`。
  - Pattern: `src/media/FfmpegSession.cpp:43` - 当前 `OpenContext(path)` 按路径打开。
  - Pattern: `src/io/ByteReader.cpp:106` - 当前 `ReadRange(std::ifstream &...)` 依赖 stream seek/read。
  - Test: `test/regression/regression_tests.cpp:3841` - `TR-AUDIT-029` 当前只覆盖直接 symlink 拒绝，需要扩展 race/split-brain。

  **执行指令**:
  1. 【红灯测试】扩展 `TR-AUDIT-029` 或新增 case，使用动态生成 MP3 A/B：A 与 B 有不同可观测 metadata 或容器特征；在可控 hook/临时 helper 中制造检查后替换路径，证明旧实现可能让 raw parser/FFmpeg 读取不同对象。若难以稳定 race，允许在测试 target 中增加测试专用 hook 宏，但不得暴露到 public API。
  2. 【确认失败】旧代码 race case 失败或能够观测 split-brain/绕过 symlink 策略；记录 `.omo/evidence/task-S3-red.txt`。
  3. 【修改 IO 合约】新增 RAII fd 持有对象或让 `ReadContext` 持有 fd/FILE*；`fileSize` 与 `lastModified` 来自 `fstat()`；raw-byte `ReadRange()` 改为基于 fd 的 `pread()` 或等效不污染 offset 的读取。
  4. 【修改 FFmpeg 打开】优先使用自定义 `AVIOContext` 基于同一 fd/pread 读取；若选择 `/proc/self/fd/<fd>`，必须证明不会重新跟随用户路径且生命周期内 fd 不被关闭，并清楚处理 seek。不得让 FFmpeg 再打开原始 path。
  5. 【最小连带修改】只更新受 `ReadContext::input` 类型变化影响的编译点；parser 仍通过 `ReadRange()` absolute offset 读取，不做无关 parser 重构。
  6. 【运行验证】`cmake --build build`；运行 S3 case、`./build/TagReaderRegressionTests TR-AUDIT-029`、`./build/TagReaderRegressionTests TR-AUDIT-031`。
  7. 【原子提交】执行 `git status --short`，只 stage S3 相关文件；执行 `git add <涉及文件>` 和 `git commit -m "fix: bind tag parsing to a single input file"`。

  **Acceptance Criteria**:
  - [ ] 直接 symlink 输入仍被拒绝。
  - [ ] 检查后替换路径不能让 FFmpeg 或 raw parser 打开替换后的对象。
  - [ ] `ReadContext` 的 raw parser 与 FFmpeg probe 使用同一文件对象或同一 fd 派生对象。
  - [ ] `ReadRange()` 仍按绝对 offset 读取且不依赖/污染共享 stream 状态。
  - [ ] 常规 MP3/FLAC/Ogg/MP4/APE 回归样本仍可读取。

  **QA Scenarios**:
  ```
  Scenario: Race-after-check cannot swap input
    Tool: Bash
    Steps: Generate A/B audio under /tmp, run S3 race regression that swaps path after validation opportunity.
    Expected: PASS; read result corresponds to the originally opened file object or rejects safely, never split-brain.
    Evidence: .omo/evidence/task-S3-race.txt

  Scenario: Direct symlink input remains rejected
    Tool: Bash
    Steps: Run ./build/TagReaderRegressionTests TR-AUDIT-029.
    Expected: PASS; symlink rejected before open/use.
    Evidence: .omo/evidence/task-S3-symlink.txt
  ```

  **Commit**: YES | Message: `fix: bind tag parsing to a single input file` | Files: `src/core/TagPipeline.cpp`, `src/media/FfmpegSession.cpp`, `src/media/FfmpegSession.hpp`, `src/core/ReadContext.hpp`, `src/io/ByteReader.hpp`, `src/io/ByteReader.cpp`, selected test files

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE before execution is considered complete.
> F1-F4 are read-only verification tasks, not implementation nodes. They must not edit source, generate committed assets, or create commits.
> If any verifier rejects, execution is blocked. The implementer must create a new fixing-forward implementation commit under the relevant P/S task scope, then rerun all F1-F4.
> Completion requires final `git status --short` to be clean except for explicitly approved plan/evidence artifacts under `.omo/`, which are ignored and must not be committed.

Common constraints for F1-F4:
- Surgical scope: inspect only files and commits touched by this plan unless a dependency must be checked to verify behavior.
- Asset policy: if a verifier needs disposable generated samples, use `/tmp`; if a reusable verifier script/state is needed, use `./tmp`; never commit generated media or temporary scripts.
- Git policy: verification tasks may run `git status --short`, `git diff`, `git log`, and read-only inspection commands; they must not run `git add`, `git commit`, `git reset`, `git revert`, `git checkout --`, or `git clean`.
- TDD evidence policy: verify that each P/S task has red evidence before fix and green evidence after fix; F1-F4 themselves do not create red tests or commits.

- [x] F1. Plan Compliance Audit — oracle
  - Verify every `ANALYSIS.md` vulnerability has a corresponding commit and test.
  - Verify no forbidden Git commands appear in shell history/log notes for this workflow.
  - Verify each task produced red and green evidence in `.omo/evidence/`.
  - Verify final `git status --short` is clean except ignored `.omo/` evidence.

- [x] F2. Code Quality Review — unspecified-high
  - Inspect surgical scope: no unrelated refactors, no public API changes, no parser rewrites beyond required areas.
  - Inspect C++23 style, RAII for fd/AVIO resources, error propagation, and platform guards.

- [x] F3. Real Manual QA — unspecified-high
  - Run `cmake -S . -B build` and `cmake --build build`.
  - Run all task-specific regression commands.
  - Run `python3 test/security/generate_samples.py` then `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_smoke_covers /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3`.
  - Run `./build/TagReaderRegressionTests TR-AUDIT-029` and `./build/TagReaderRegressionTests TR-AUDIT-031`.

- [x] F4. Scope Fidelity Check — deep
  - Compare final diff against `.omo/plans/tagreader-security-fixes.md` and `ANALYSIS.md`.
  - Confirm generated media/scripts are absent from staged/committed files.
  - Confirm final `git status --short` is clean except ignored `.omo/` evidence.

## Commit Strategy
- 每个 P/S 任务一个 commit，格式固定：`fix: <任务描述>`。
- 每个 commit 前必须执行 `git status --short` 和 `git diff --cached --stat`，确认只 stage 当前任务文件。
- 只允许 `git add <涉及文件>` 与 `git commit -m "fix: ..."`。
- 禁止 `git reset`、`git revert`、`git checkout --`、`git clean`、force push、amend、rebase。
- 若 commit 后发现错误：新增修复代码、新增验证、再新增一个 fixing-forward commit；不得回退历史。
- 并行实现任务若共用同一工作区，commit 必须按 P1、P2、S1、S2、S3 的顺序串行执行，避免 index 混杂。

## Success Criteria
- 5 个漏洞/缺陷均有测试先失败、修复后通过的证据。
- 默认封面目录保留 tmpfs/临时目录便利性，同时不再使用共享固定未加固 root。
- 显式 `coverExportDir` symlink 被拒绝，调用方目录边界不再依赖外部约定。
- 输入文件只绑定一个已打开对象，raw parser 与 FFmpeg 不再 split-brain。
- FLAC 坏块只影响当前 block；cover cache/export 错误仍上抛。
- 歌词去重复杂度不再 O(n²)，语义保持。
- 最终构建、相关回归、安全 smoke 全部 PASS。
- 最终 `git status --short` 干净，所有代码改动均已原子化提交，生成资产未提交。
