# ANALYSIS.md Bug Report 修复计划

## TL;DR
> **Summary**: 修复 `ANALYSIS.md` Bug Report 中 8 个仍成立的问题，范围限定为 TagReader 当前音乐/音频元数据读取库，不扩展到通用文件解析器。所有修复保持公共 API、依赖策略和主流程不变，并使用现有构建、security smoke、fuzz 与确定性样本脚本验证。
> **Deliverables**:
> - `src/TagReader.cpp` 中封面缓存、ID3、MP4、Ogg、LRC 分支的定向修复
> - `test/fuzz/tagreader_fuzz.cpp` fuzz 隔离与覆盖增强
> - `test/security/generate_samples.py` 与 `test/corpus/generate_corpus.py` 的确定性回归样本扩展
> - 现有 `TagReaderTest`、`TagReaderSecuritySmoke`、ASAN/UBSAN、可选 libFuzzer 验证通过
> **Effort**: Large
> **Parallel**: YES - 4 waves
> **Critical Path**: Task 1/2 封面失败语义 → Task 3 fuzz 基建 → Task 4/5/6/7/8 parser 修复 → Final Verification Wave

## Context
### Original Request
用户要求：“请根据 `ANALYSIS.md` 文档中的 `Bug Report`，规划详细的 bug 修复计划。对于第六个问题，当前库的定位是处理音乐文件，不考虑非音乐/音频文件。且解析器部分只需要可以将 `include/Tag.hpp` 中 `MusicTag` 类中的所有字段全部解析出来就行。”

### Interview Summary
- 计划只覆盖 `ANALYSIS.md` 的 8 个 Bug Report 项。
- Bug #6 已收敛：仅处理音乐/音频 Ogg Vorbis 文件中 `MusicTag` 字段的提取，不做非音频/非音乐文件支持。
- 不修改 `TagReader::Read()` 签名，不新增 `MusicTag` 字段，不引入 Result 类型。
- 显式传入 `coverExportDir` 时，封面导出失败或既有缓存文件不可信应抛出可诊断异常；未传 `coverExportDir` 时不得产生封面文件副作用。
- 测试策略采用 tests-after：每个修复任务包含实现与回归验证，不引入单元测试框架。
- 本轮 Prometheus 只产出 `.omo/drafts/bug-report-fix-plan.md` 与 `.omo/plans/bug-report-fixes.md`；工作区中既有 `AGENTS.md`、`ANALYSIS.md` 修改来自上一轮文档审计，不属于本计划执行或代码实现。

### Metis Review (gaps addressed)
- 已显式固定封面导出失败 API 行为，避免实现者自行决定抛异常/空字段/错误状态。
- 已显式固定缓存命中校验失败策略：不覆盖、不跟随 symlink、不静默返回旧路径，直接失败并报告不可信缓存。
- 已限制 Bug #6 范围，避免膨胀成完整 Ogg muxer、非 Vorbis、Opus 或视频混流支持。
- 已固定 LRC 方括号行规则、MP4 UTF-16 BOM 策略、MP4 `size=0` 容错语义。
- 已明确不得要求 GTest/Catch2、CI、lint、formatter。

## Work Objectives
### Core Objective
在不改变公共 API、依赖策略和主解析流程的前提下，修复 `ANALYSIS.md` Bug Report 的 8 个问题，并用现有仓库验证工具给出 agent 可执行证据。

### Deliverables
- 8 个实现任务，每个任务对应一个 Bug Report 项。
- 每个任务内联样本生成/扩展和回归验证。
- 最终验证波次包含计划合规、代码质量、真实 QA、范围一致性 4 路复核。

### Definition of Done (verifiable conditions with commands)
- `cmake -S . -B build && cmake --build build` 退出码 0。
- `python3 test/security/generate_samples.py` 退出码 0，并生成本计划指定的确定性安全样本。
- `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_samples/covers /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3 /tmp/opencode/tagreader_security_samples/cover_export_base.mp3 /tmp/opencode/tagreader_security_samples/mp4_lyrics_utf16_bom.m4a /tmp/opencode/tagreader_security_samples/ogg_vorbis_music_multistream_comments.ogg` 退出码 0。
- `cmake -S . -B build-asan -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-asan` 退出码 0；运行 smoke 时无 ASAN/UBSAN 报错。
- 若 `clang++` 可用：`cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON && cmake --build build-fuzz-clang` 退出码 0；`./build-fuzz-clang/TagReaderFuzz /tmp/opencode/tagreader_fuzz_corpus -runs=1000` 无崩溃。

### Must Have
- 保持主流程：`ValidatePath()` → `OpenContext()` → `DetectStream()` → `DetectContainer()` → `ReadMediaInfo()` → `ReadMetadata()` → `ReadLyrics()` → `BuildMusicTag()`。
- 标签、歌词、封面块继续直接读文件原始字节解析；FFmpeg 只负责 probe、容器识别、主音频流、基础媒体信息、封面图像解码/PNG 编码。
- 修改优先放在现有格式专用小函数或相邻新增小函数。
- 所有文本字段最终保持 UTF-8。
- 所有验收条件可由 agent 执行，不依赖人工查看。

### Must NOT Have (guardrails, AI slop patterns, scope boundaries)
- 不引入 TagLib、Mutagen、外部标签库或新运行时依赖。
- 不把 `AVDictionary` 当元数据来源。
- 不新增公共 API、不新增 `MusicTag` 字段、不改变 `TagReader::Read()` 返回类型。
- 不新增 GTest/Catch2、CI、lint、formatter。
- 不做全文件重构、架构迁移、入口大 lambda 塞逻辑。
- 不把 Bug #6 扩展成通用 Ogg muxer、非 Vorbis、Opus、视频混流或非音乐文件支持。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after + existing CMake/manual harness/security smoke/fuzz framework
- QA policy: Every task has agent-executed scenarios
- Evidence: `.omo/evidence/task-{N}-{slug}.{ext}`

## Execution Strategy
### Parallel Execution Waves
> Target: 5-8 tasks per wave. <3 per wave (except final) = under-splitting.
> Extract shared dependencies as Wave-1 tasks for max parallelism.

Wave 1: Task 1, Task 2, Task 3（封面语义与 fuzz 基建，互相独立）
Wave 2: Task 4, Task 5, Task 7, Task 8（ID3/MP4/LRC/MP4 walker 分支修复，局部独立）
Wave 3: Task 6（Ogg Vorbis comment 兼容，需利用前两波样本/验证模式）
Wave 4: Final Verification Wave

### Dependency Matrix (full, all tasks)
- Task 1: blocks Task 2 的封面失败一致性验收；otherwise independent.
- Task 2: blocked by Task 1 的缓存失败语义；blocks final smoke.
- Task 3: independent; improves final fuzz coverage.
- Task 4: independent; affects ID3 lyrics only.
- Task 5: independent; affects MP4 lyrics only.
- Task 6: independent from code changes, but should reuse final `MusicTag` field assertion style from Task 5/7 samples.
- Task 7: independent; affects plain/timed lyrics normalization.
- Task 8: independent; affects MP4 atom traversal and may improve Task 5 corpus coverage.
- F1-F4: blocked by all tasks.

### Agent Dispatch Summary (wave → task count → categories)
- Wave 1 → 3 tasks → `unspecified-high` for filesystem/fuzz safety work
- Wave 2 → 4 tasks → `unspecified-high` for parser fixes
- Wave 3 → 1 task → `deep` for Ogg Vorbis constrained compatibility
- Wave 4 → 4 tasks → oracle / unspecified-high / unspecified-high / deep

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.

- [x] 1. 校验既有 cover cache 文件，拒绝不可信缓存命中

  **What to do**: 在 `src/TagReader.cpp` 的封面缓存路径中增加“命中既有路径”校验。修改 `WriteCoverAsPng()`：先按当前嵌入封面解码得到本次应写入的 `pngBytes`，再处理 `coverPath` 是否已存在；若已存在，调用相邻新增 helper（建议命名 `ValidateExistingCoverCacheFile()`）使用 `std::filesystem::symlink_status()` 拒绝 symlink 和非普通文件，并在大小上限内读取既有文件，与本次 `pngBytes` 做 byte-for-byte 比较；完全一致才返回既有 `coverPath`。验证失败时抛出 `std::runtime_error` 或 `std::filesystem::filesystem_error`，错误消息必须包含 `cover cache` 和目标路径。扩展 `test/security/generate_samples.py` 生成 `cover_cache_base.mp3` 样本，并在 `TagReaderSecuritySmoke` 中加入“缓存路径被 symlink/文本文件污染后第二次读取失败”的自动断言。
  **Must NOT do**: 不覆盖既有缓存文件；不跟随 symlink；不静默返回旧路径；不在未传 `coverExportDir` 时创建任何文件；不改变 `MusicTag::coverPath` 类型或 `TagReader::Read()` 签名。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 文件系统安全与异常语义需要谨慎处理
  - Skills: [] - 不需要额外技能
  - Omitted: [`frontend-design`, `pdf`, `xlsx`] - 与 C++ 库修复无关

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: [2] | Blocked By: []

  **References**:
  - Pattern: `src/TagReader.cpp:331-360` - `HashEmbeddedImageBytes()` / `BuildCoverCachePath()` 提供内容派生路径
  - Pattern: `src/TagReader.cpp:486-554` - `AtomicWriteFileIfAbsent()`、`PublishFileIfAbsent()`、`FsyncDirectory()` 原子发布模式
  - Pattern: `src/TagReader.cpp:876-916` - `WriteCoverAsPng()` 当前直接返回既有路径的位置
  - Pattern: `src/TagReader.cpp:977-1010` - `ValidateCoverExportDir()` 目录校验模式
  - Test: `test/security/security_smoke.cpp` - cover cache 重复读取/并发读取断言位置
  - Test: `test/security/generate_samples.py` - 增加确定性封面样本

  **Acceptance Criteria** (agent-executable only):
  - [ ] `cmake -S . -B build && cmake --build build` 退出码 0。
  - [ ] `python3 test/security/generate_samples.py` 生成 `/tmp/opencode/tagreader_security_samples/cover_cache_base.mp3`。
  - [ ] 第一次运行 `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3 /tmp/opencode/tagreader_security_samples/covers` 输出包含非空 `coverPath`。
  - [ ] 将该 `coverPath` 替换为 symlink 或文本文件后，再运行同一命令退出非 0，stderr 包含 `cover cache`。
  - [ ] `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_samples/covers /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3` 对未污染缓存退出码 0。

  **QA Scenarios** (MANDATORY - task incomplete without these):
  ```
  Scenario: Happy path - 已有合法缓存可复用
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && cmake -S . -B build && cmake --build build && rm -rf /tmp/opencode/tagreader_security_samples/covers && mkdir -p /tmp/opencode/tagreader_security_samples/covers && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3 /tmp/opencode/tagreader_security_samples/covers > /tmp/opencode/task1-first.txt && cover_path=$(awk -F': ' '/^coverPath: /{print $2}' /tmp/opencode/task1-first.txt) && test -n "$cover_path" && mtime1=$(stat -c %Y "$cover_path") && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3 /tmp/opencode/tagreader_security_samples/covers > /tmp/opencode/task1-second.txt && cover_path2=$(awk -F': ' '/^coverPath: /{print $2}' /tmp/opencode/task1-second.txt) && mtime2=$(stat -c %Y "$cover_path2") && test "$cover_path" = "$cover_path2" && test "$mtime1" = "$mtime2"
    Expected: 命令退出码 0；`/tmp/opencode/task1-first.txt` 与 `task1-second.txt` 中 coverPath 相同；mtime 相同
    Evidence: .omo/evidence/task-1-cover-cache-valid.txt

  Scenario: Failure - 既有缓存路径被污染
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && cmake -S . -B build && cmake --build build && rm -rf /tmp/opencode/tagreader_security_samples/covers && mkdir -p /tmp/opencode/tagreader_security_samples/covers && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3 /tmp/opencode/tagreader_security_samples/covers > /tmp/opencode/task1-poison-first.txt && cover_path=$(awk -F': ' '/^coverPath: /{print $2}' /tmp/opencode/task1-poison-first.txt) && test -n "$cover_path" && printf 'not a png cache entry\n' > "$cover_path" && if ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3 /tmp/opencode/tagreader_security_samples/covers > /tmp/opencode/task1-poison-out.txt 2> /tmp/opencode/task1-poison-err.txt; then exit 1; else grep -q 'cover cache' /tmp/opencode/task1-poison-err.txt; fi
    Expected: 命令退出码 0；内部第二次 TagReaderTest 失败；`/tmp/opencode/task1-poison-err.txt` 包含 cover cache
    Evidence: .omo/evidence/task-1-cover-cache-poisoned.txt
  ```

  **Commit**: YES | Message: `fix(cover): validate existing cache entries` | Files: [`src/TagReader.cpp`, `test/security/security_smoke.cpp`, `test/security/generate_samples.py`]

- [x] 2. 区分封面导出失败与 malformed metadata 容错

  **What to do**: 调整 `ReadMetadata()` 中 `ignoreMalformedMetadata` 的容错边界。保留普通 malformed tag 字段吞异常策略，但当 `coverExportDir` 有值且异常来源于 `WriteCoverAsPng()`、`AtomicWriteFileIfAbsent()`、目录不可写、`fsync/link/open` 失败或 Task 1 的缓存不可信时，异常必须向上传播到 `TagReader::Read()` 调用方。实现方式应优先在封面导出调用点局部捕获/重抛，不要引入全局错误状态。扩展 security samples 生成 `cover_export_base.mp3`，并在 smoke 或独立命令中验证不可写目录失败。
  **Must NOT do**: 不让 malformed 标题/专辑等字段导致整次读取失败；不新增日志系统；不改变 public API；不吞掉显式导出失败。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 异常边界影响调用方可观测性
  - Skills: [] - 不需要额外技能
  - Omitted: [`webapp-testing`] - 无浏览器/UI

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: [] | Blocked By: [1 for final cache semantics]

  **References**:
  - Pattern: `src/TagReader.cpp:2802-2855` - `ReadMetadata()` 的 `ignoreMalformedMetadata`
  - Pattern: `src/TagReader.cpp:876-916` - `WriteCoverAsPng()` 导出异常来源
  - Pattern: `src/TagReader.cpp:925-943` - `TagReader::Read()` 调用链
  - Pattern: `src/TagReader.cpp:4584-4623` - `ReadLyrics()` 容错风格参考
  - Test: `test/security/generate_samples.py` - 添加封面导出失败样本

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_export_base.mp3 /tmp/opencode/tagreader_security_samples/covers` 退出码 0 且输出非空 `coverPath`。
  - [ ] 对不可写目录运行同一命令退出非 0，stderr 包含 `cover export` 或 `cover cache` 和目录路径。
  - [ ] 对 `/tmp/opencode/tagreader_security_samples/malformed_noncover_metadata.mp3` 运行 `TagReaderTest` 退出码 0，字段缺失允许为空。
  - [ ] ASAN/UBSAN 构建通过且 smoke 无 sanitizer 报错。

  **QA Scenarios**:
  ```
  Scenario: Happy path - 显式导出成功
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && cmake -S . -B build && cmake --build build && rm -rf /tmp/opencode/tagreader_security_samples/covers && mkdir -p /tmp/opencode/tagreader_security_samples/covers && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_export_base.mp3 /tmp/opencode/tagreader_security_samples/covers > /tmp/opencode/task2-cover-export-ok.txt && awk -F': ' '/^coverPath: /{ if ($2 ~ /^\/tmp\/opencode\/tagreader_security_samples\/covers\//) found=1 } END { exit(found ? 0 : 1) }' /tmp/opencode/task2-cover-export-ok.txt
    Expected: 退出码 0；stdout 包含 coverPath 且路径位于 covers 目录
    Evidence: .omo/evidence/task-2-cover-export-ok.txt

  Scenario: Failure - 显式导出目录不可写
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && cmake -S . -B build && cmake --build build && rm -rf /tmp/opencode/tagreader_readonly_covers && mkdir -p /tmp/opencode/tagreader_readonly_covers && chmod 500 /tmp/opencode/tagreader_readonly_covers && if ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_export_base.mp3 /tmp/opencode/tagreader_readonly_covers > /tmp/opencode/task2-readonly-out.txt 2> /tmp/opencode/task2-readonly-err.txt; then chmod 700 /tmp/opencode/tagreader_readonly_covers; exit 1; else chmod 700 /tmp/opencode/tagreader_readonly_covers; grep -Eq 'cover export|cover cache|Permission denied|filesystem' /tmp/opencode/task2-readonly-err.txt; fi
    Expected: 命令退出码 0；内部 TagReaderTest 失败；stderr 匹配 cover export/cover cache/Permission denied/filesystem
    Evidence: .omo/evidence/task-2-cover-export-readonly.txt
  ```

  **Commit**: YES | Message: `fix(cover): propagate explicit export failures` | Files: [`src/TagReader.cpp`, `test/security/generate_samples.py`, `test/security/security_smoke.cpp`]

- [x] 3. 隔离 fuzz 临时目录并提升 public API 覆盖

  **What to do**: 修改 `test/fuzz/tagreader_fuzz.cpp`，避免所有 fuzz worker 固定共享 `/tmp/tagreader_fuzz`。将 `FuzzRoot()` 改为优先使用环境变量 `TAGREADER_FUZZ_ROOT`，否则使用 `std::filesystem::temp_directory_path()` 下包含进程 ID 或 worker 标识的唯一子目录；`CleanupFuzzFiles()` 只清理本 worker 子目录。扩展 `test/corpus/generate_corpus.py` 增加能通过 FFmpeg probe/audio gate 的最小 MP3/M4A/Ogg 样本，以及直接针对 ID3/MP4/Ogg/LRC 边界的 seed 文件。保持 public API fuzz 为主；如新增 parser-level harness，必须仍构建为可选目标且不暴露私有 API 给库用户。
  **Must NOT do**: 不清理共享 `/tmp`；不依赖绝对固定目录；不要求 corpus 提交二进制；不重写 fuzz 基建为复杂框架。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: fuzz harness 与临时文件隔离影响安全验证
  - Skills: [] - 不需要额外技能
  - Omitted: [`mcp-builder`] - 无 MCP

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: [] | Blocked By: []

  **References**:
  - Pattern: `test/fuzz/tagreader_fuzz.cpp:11-74` - `FuzzRoot()`、`CleanupFuzzFiles()`、`LLVMFuzzerTestOneInput()`
  - Test: `test/corpus/generate_corpus.py` - 确定性 corpus 生成脚本
  - Test: `test/corpus/README.md` - corpus 不提交二进制 seed 的说明
  - Build: `CMakeLists.txt` - `TAGREADER_ENABLE_FUZZING` 仅 Clang 下生成 `TagReaderFuzz`

  **Acceptance Criteria**:
  - [ ] `python3 test/corpus/generate_corpus.py` 退出码 0，生成 `/tmp/opencode/tagreader_fuzz_corpus`。
  - [ ] `TAGREADER_FUZZ_ROOT=/tmp/opencode/tagreader_fuzz_a ./build-fuzz-clang/TagReaderFuzz /tmp/opencode/tagreader_fuzz_corpus -runs=100` 不写入 `/tmp/tagreader_fuzz`。
  - [ ] 并行运行两个不同 `TAGREADER_FUZZ_ROOT` 的 fuzz 进程时互不删除对方目录。
  - [ ] 若 `clang++` 不可用，记录 `.omo/evidence/task-3-fuzz-skipped.txt`，普通构建和 corpus 生成仍必须通过。

  **QA Scenarios**:
  ```
  Scenario: Happy path - 自定义 fuzz root 隔离
    Tool: Bash
    Steps: python3 test/corpus/generate_corpus.py && cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON && cmake --build build-fuzz-clang && rm -rf /tmp/opencode/tagreader_fuzz_a /tmp/tagreader_fuzz && TAGREADER_FUZZ_ROOT=/tmp/opencode/tagreader_fuzz_a ./build-fuzz-clang/TagReaderFuzz /tmp/opencode/tagreader_fuzz_corpus -runs=100 && test -d /tmp/opencode/tagreader_fuzz_a && test ! -e /tmp/tagreader_fuzz
    Expected: 退出码 0；/tmp/opencode/tagreader_fuzz_a 存在；/tmp/tagreader_fuzz 不存在或未被本进程创建
    Evidence: .omo/evidence/task-3-fuzz-root.txt

  Scenario: Failure/edge - 并行 worker 不互删
    Tool: Bash
    Steps: python3 test/corpus/generate_corpus.py && cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON && cmake --build build-fuzz-clang && rm -rf /tmp/opencode/tagreader_fuzz_a /tmp/opencode/tagreader_fuzz_b && (TAGREADER_FUZZ_ROOT=/tmp/opencode/tagreader_fuzz_a ./build-fuzz-clang/TagReaderFuzz /tmp/opencode/tagreader_fuzz_corpus -runs=100 > /tmp/opencode/task3-fuzz-a.txt 2>&1 & pid_a=$!; TAGREADER_FUZZ_ROOT=/tmp/opencode/tagreader_fuzz_b ./build-fuzz-clang/TagReaderFuzz /tmp/opencode/tagreader_fuzz_corpus -runs=100 > /tmp/opencode/task3-fuzz-b.txt 2>&1 & pid_b=$!; wait $pid_a; wait $pid_b) && test -d /tmp/opencode/tagreader_fuzz_a && test -d /tmp/opencode/tagreader_fuzz_b
    Expected: 命令退出码 0；两个 fuzz root 目录均存在；两个 fuzz 进程日志无崩溃
    Evidence: .omo/evidence/task-3-fuzz-parallel.txt
  ```

  **Commit**: YES | Message: `test(fuzz): isolate temporary roots` | Files: [`test/fuzz/tagreader_fuzz.cpp`, `test/corpus/generate_corpus.py`, `test/corpus/README.md`]

- [x] 4. 放宽 ID3v2.2 lyrics tag flags 判定

  **What to do**: 修改 `ReadID3Lyrics()` 中 `tagView.versionMajor == 2 && tagView.flags != 0` 直接返回的逻辑。复用 `ReadId3TagBytes()` 已完成的 tag 级读取/unsync 处理结果；仅拒绝确实无法支持或未定义的 v2.2 flag，不因合法/可忽略 flag 丢弃 `ULT`/`SLT` 歌词帧。`ReadID3v22LyricsFrames()` 继续负责 v2.2 frame 解析，禁止把 v2.3/v2.4 frame flag 规则原样套给 v2.2。扩展 `generate_samples.py` 或 `generate_corpus.py` 生成 `id3v22_lyrics_flagged.mp3` 和 `id3v22_lyrics_unsupported_flag.mp3`。
  **Must NOT do**: 不改变 metadata 路径行为；不让压缩/加密/未知无法安全解析的数据进入歌词；不扩大到完整 ID3 重写。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: ID3 flags 与歌词帧兼容性需遵循现有解析边界
  - Skills: [] - 不需要额外技能
  - Omitted: [`frontend-design`] - 无 UI

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [] | Blocked By: []

  **References**:
  - Pattern: `src/TagReader.cpp:3013-3097` - `ReadId3TagBytes()` tag 读取与边界处理
  - Pattern: `src/TagReader.cpp:4039-4058` - `ReadID3Lyrics()` 当前 v2.2 flags 早退
  - Pattern: `src/TagReader.cpp:4061-4166` - `ReadID3v22LyricsFrames()` v2.2 歌词帧解析
  - Pattern: `src/TagReader.cpp` - `PrepareId3v23Or24FrameData()` / `Id3v24TagUnsyncAppliesToPayload()` 精确 flag 判断风格

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/id3v22_lyrics_flagged.mp3` 退出码 0，stdout 的 lyrics 包含 `ID3v22 flagged lyric line`。
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/id3v22_lyrics_unsupported_flag.mp3` 退出码 0 或按现有容错返回空 lyrics，但不得崩溃。
  - [ ] `python3 test/corpus/generate_corpus.py && test -f /tmp/opencode/tagreader_fuzz_corpus/id3v22_lyrics_flagged.mp3` 退出码 0。

  **QA Scenarios**:
  ```
  Scenario: Happy path - v2.2 合法 flag 不丢歌词
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/id3v22_lyrics_flagged.mp3
    Expected: 退出码 0；stdout 包含 lyrics 和 ID3v22 flagged lyric line
    Evidence: .omo/evidence/task-4-id3v22-lyrics-flagged.txt

  Scenario: Failure/edge - 不支持 flag 容错
    Tool: Bash
    Steps: ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/id3v22_lyrics_unsupported_flag.mp3
    Expected: 退出码 0；无崩溃；lyrics 为空或不包含损坏 payload
    Evidence: .omo/evidence/task-4-id3v22-unsupported-flag.txt
  ```

  **Commit**: YES | Message: `fix(id3): parse v22 lyrics with supported tag flags` | Files: [`src/TagReader.cpp`, `test/security/generate_samples.py`, `test/corpus/generate_corpus.py`]

- [x] 5. 统一 MP4 lyrics 文本解码并支持 UTF-16 data atom

  **What to do**: 修改 `ReadMP4LyricsItem()` 和 `ReadMP4FreeformLyricsItem()`，移除只接受 data type 0/1 的窄条件，统一调用 `DecodeMp4TextData()` 处理 type 0/1/2/3。保留 `kMaxLyricsBytes` 上限和现有 `RawLyrics` UTF-8 输出约束。针对 UTF-16 BOM 场景生成 `mp4_lyrics_utf16_bom.m4a`；若无 BOM，按现有 `DecodeTextToUtf8()`/`DecodeRawText()` 策略处理，不新增猜编码大系统。同步检查 `ReadMP4DataAtom()` 不被破坏。
  **Must NOT do**: 不把 MP4 metadata/lyrics 改为 AVDictionary；不做完整 MP4 metadata 引擎；不取消歌词大小上限；不改变非 lyrics 文本字段既有行为。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: MP4 atom 文本解码共享 helper 影响多字段
  - Skills: [] - 不需要额外技能
  - Omitted: [`pdf`] - 无 PDF

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [] | Blocked By: []

  **References**:
  - Pattern: `src/TagReader.cpp:3909-3943` - `DecodeMp4TextData()` 已支持 type 0/1/2/3
  - Pattern: `src/TagReader.cpp:3962-4037` - `ReadMP4DataAtom()` metadata 文本路径
  - Pattern: `src/TagReader.cpp:4848-4943` - `ReadMP4LyricsItem()` / `ReadMP4FreeformLyricsItem()` 当前 lyrics 路径
  - API/Type: `include/Tag.hpp` - `MusicTag::lyrics` 最终字段

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_lyrics_utf16_bom.m4a` 退出码 0，stdout 的 lyrics 包含 `MP4 UTF16 lyric line`。
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_lyrics_utf8.m4a` 退出码 0，stdout 的 lyrics 包含 `MP4 UTF8 lyric line`。
  - [ ] `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_samples/covers /tmp/opencode/tagreader_security_samples/mp4_lyrics_utf16_bom.m4a` 退出码 0。

  **QA Scenarios**:
  ```
  Scenario: Happy path - MP4 UTF-16 BOM lyrics
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_lyrics_utf16_bom.m4a
    Expected: 退出码 0；stdout lyrics 包含 MP4 UTF16 lyric line
    Evidence: .omo/evidence/task-5-mp4-lyrics-utf16.txt

  Scenario: Failure/edge - 超大 lyrics 仍受限
    Tool: Bash
    Steps: ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_lyrics_oversized.m4a
    Expected: 退出码 0；无崩溃；lyrics 为空或被安全拒绝；stderr 无 sanitizer 报错
    Evidence: .omo/evidence/task-5-mp4-lyrics-oversized.txt
  ```

  **Commit**: YES | Message: `fix(mp4): decode utf16 lyrics data atoms` | Files: [`src/TagReader.cpp`, `test/security/generate_samples.py`, `test/corpus/generate_corpus.py`]

- [x] 6. 限定范围增强 Ogg Vorbis MusicTag 字段提取

  **What to do**: 在 `ReadOggVorbisCommentEntries()` 中增强合法 Ogg Vorbis 音乐文件的 comment packet 定位。保留 `kMaxOggPages`、`kMaxOggPacketBytes`、`kMaxOggScannedBytes` 等资源限制；建立最小 per-serial 状态，只选择包含 Vorbis identification packet 与 comment packet 的音频 logical stream，不支持非 Vorbis、Opus、Theora/视频混流、通用 chained muxer。确保 `ForEachVorbisCommentEntry()` 仍用于 entry 迭代，并验证 `MusicTag` 字段：`title/genre/artist/album/albumArtist/composer/year/trackNumber/discNumber/lyrics` 可从 Vorbis comments 映射；`filePath/duration/offset/lastModified/sampleRate/bitDepth/bitRate/channels/format/coverPath/playCount/rating/lastPlayed` 不从 Ogg comment parser 新增业务语义，继续由现有文件/媒体/默认值路径填充。扩展样本 `ogg_vorbis_music_multistream_comments.ogg` 只包含音乐/音频范围内的合法结构。
  **Must NOT do**: 不支持非音乐/非音频文件；不支持视频混流；不新增 Opus；不实现完整 Ogg muxer；不移除资源上限；不把非 `MusicTag` 字段作为目标。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: Ogg page/packet 状态机需要谨慎限制范围
  - Skills: [] - 不需要额外技能
  - Omitted: [`frontend-design`, `webapp-testing`] - 无 UI

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: [] | Blocked By: []

  **References**:
  - Pattern: `src/TagReader.cpp:1354` - `ForEachVorbisCommentEntry()` comment entry 迭代模式
  - Pattern: `src/TagReader.cpp:3560-3700` - `ReadOggVorbisCommentEntries()` 当前单 serial/sequence 限制
  - API/Type: `include/Tag.hpp` - `MusicTag` 字段全集，验收只覆盖这些字段
  - Test: `test/security/generate_samples.py` - 添加 Ogg Vorbis 音乐样本

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_vorbis_music_multistream_comments.ogg` 退出码 0。
  - [ ] stdout 包含：`title: Ogg Title`、`artist: Ogg Artist`、`album: Ogg Album`、`albumArtist: Ogg Album Artist`、`composer: Ogg Composer`、`genre: Ogg Genre`、`year: 2026`、`trackNumber: 3`、`discNumber: 1`、`[0]:Ogg lyric line`。
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_non_vorbis_or_video_mixed.ogg > /tmp/opencode/task6-nontarget-out.txt 2> /tmp/opencode/task6-nontarget-err.txt; code=$?; test "$code" = 0 -o "$code" = 2` 成立，且 stderr 不包含 `AddressSanitizer`、`runtime error`、`heap-buffer-overflow`。
  - [ ] `timeout 10 ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_comment_resource_limit.ogg > /tmp/opencode/task6-limit-out.txt 2> /tmp/opencode/task6-limit-err.txt; code=$?; test "$code" = 0 -o "$code" = 2` 成立，且 stderr 不包含 `AddressSanitizer`、`runtime error`、`out of memory`。

  **QA Scenarios**:
  ```
  Scenario: Happy path - 音乐 Ogg Vorbis MusicTag 字段完整提取
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_vorbis_music_multistream_comments.ogg
    Expected: 退出码 0；stdout 以 `字段名: 值` 格式包含所有指定 MusicTag 文本字段值，并包含 `[0]:Ogg lyric line`
    Evidence: .omo/evidence/task-6-ogg-vorbis-musictag.txt

  Scenario: Failure/edge - 非目标 Ogg 结构安全跳过
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_non_vorbis_or_video_mixed.ogg > /tmp/opencode/task6-nontarget-out.txt 2> /tmp/opencode/task6-nontarget-err.txt; code=$?; test "$code" = 0 -o "$code" = 2; ! grep -Eq 'AddressSanitizer|runtime error|heap-buffer-overflow' /tmp/opencode/task6-nontarget-err.txt
    Expected: 命令退出码 0；内部 TagReaderTest 只允许退出 0 或 2；stderr 无 sanitizer/UB/OOB 关键词；不要求填充 metadata
    Evidence: .omo/evidence/task-6-ogg-nontarget-safe.txt

  Scenario: Failure/edge - Ogg comment 资源限制样本不超时
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && timeout 10 ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_comment_resource_limit.ogg > /tmp/opencode/task6-limit-out.txt 2> /tmp/opencode/task6-limit-err.txt; code=$?; test "$code" = 0 -o "$code" = 2; ! grep -Eq 'AddressSanitizer|runtime error|out of memory' /tmp/opencode/task6-limit-err.txt
    Expected: 命令退出码 0；内部 TagReaderTest 只允许退出 0 或 2；10 秒内返回；stderr 无 sanitizer/UB/OOM 关键词
    Evidence: .omo/evidence/task-6-ogg-resource-limit.txt
  ```

  **Commit**: YES | Message: `fix(ogg): target vorbis comments for music tags` | Files: [`src/TagReader.cpp`, `test/security/generate_samples.py`, `test/corpus/generate_corpus.py`]

- [x] 7. 保留非时间戳方括号 LRC 行为普通歌词

  **What to do**: 修改 `ReadLyricsFromPlainText()` 的 LRC 行解析。仅当一行至少成功解析出一个 `ParseLrcTimestamp()` 时间戳时，才作为 timed lyric；若行首/行内方括号 token 不是有效时间戳且不是 LRC metadata 行（`[ar:]`、`[ti:]`、`[al:]`、`[by:]`、`[offset:]`），则整行作为 plain lyric 保留。`[00:01.00]text` 和 `[00:01.00][00:02.00]text` 仍进入 timed lyrics；`[Verse]`、`[hello]`、`[Chorus] sing` 进入 plain lyrics。扩展样本 `lyrics_bracket_plain.mp3`。
  **Must NOT do**: 不实现完整 karaoke/翻译 LRC；不把 `[ar:]` 等 metadata 行写入正文歌词；不破坏 timed lyrics 排序、去重、行数限制。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 歌词解析行为需精确保留兼容性
  - Skills: [] - 不需要额外技能
  - Omitted: [`xlsx`] - 无表格

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [] | Blocked By: []

  **References**:
  - Pattern: `src/TagReader.cpp:4477-4582` - `ReadLyricsFromPlainText()` 当前跳过 invalid timestamp 行
  - Pattern: `src/TagReader.cpp:4945-5000` - `ParseLrcTimestamp()` token 解析
  - Pattern: `src/TagReader.cpp:4828-4845` - `AppendPlainLyrics()` / `AppendTimedLyrics()` 输出收敛

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/lyrics_bracket_plain.mp3` stdout lyrics 包含 `[Verse]` 和 `[Chorus] sing`。
  - [ ] 同一输出不包含 `[ar:Unit Test Artist]` 作为正文歌词。
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/lyrics_timed_multi.mp3` 退出码 0，stdout 包含 `[1000000]:first timed line` 和 `[2000000]:second timed line`。

  **QA Scenarios**:
  ```
  Scenario: Happy path - 非时间戳方括号保留
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/lyrics_bracket_plain.mp3
    Expected: 退出码 0；lyrics 包含 [Verse] 与 [Chorus] sing；不包含 [ar:Unit Test Artist]
    Evidence: .omo/evidence/task-7-lrc-bracket-plain.txt

  Scenario: Failure/edge - 有效时间戳仍按 timed lyrics
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/lyrics_timed_multi.mp3 > /tmp/opencode/task7-timed.txt && grep -q '\[1000000\]:first timed line' /tmp/opencode/task7-timed.txt && grep -q '\[2000000\]:second timed line' /tmp/opencode/task7-timed.txt
    Expected: 命令退出码 0；stdout 精确包含 `[1000000]:first timed line` 和 `[2000000]:second timed line`
    Evidence: .omo/evidence/task-7-lrc-timed.txt
  ```

  **Commit**: YES | Message: `fix(lyrics): preserve bracketed plain lrc lines` | Files: [`src/TagReader.cpp`, `test/security/generate_samples.py`, `test/corpus/generate_corpus.py`]

- [x] 8. 明确 MP4 size=0 atom 的 sibling 扫描与恢复策略

  **What to do**: 调整 `ForEachMp4ChildAtom()` 对 `atom.atomSize == 0` 的处理。规则固定为：`size=0` 仅作为“当前 parent 范围剩余部分由该 atom 占用”的 EOF 终止语义接受；若该 atom 位于当前扫描范围中间且会隐藏后续 `ilst` metadata sibling，则返回 `ParseStatus::Malformed` 或可恢复状态，并允许上层 walker 在安全范围内继续扫描可定位 sibling，绝不越过 parent end。`ReadMp4AtomHeader()` 保持边界校验；`WalkMp4IlstItems()` 必须区分规范 EOF、malformed、resource limit。扩展 `mp4_size0_tail_ok.m4a` 和 `mp4_size0_hides_metadata.m4a` corpus/security 样本。
  **Must NOT do**: 不进行越界恢复扫描；不递归重写 walker；不把 malformed MP4 当完全可信；不破坏正常 MP4 ilst metadata 读取。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: MP4 atom walker 影响多处 metadata/lyrics 解析
  - Skills: [] - 不需要额外技能
  - Omitted: [`webapp-testing`] - 无 UI

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [] | Blocked By: []

  **References**:
  - Pattern: `src/TagReader.cpp:2161-2236` - `ReadMp4AtomHeader()` size/header 边界
  - Pattern: `src/TagReader.cpp:2289-2333` - `ForEachMp4ChildAtom()` 当前 `atomSize == 0` 早退
  - Pattern: `src/TagReader.cpp:2366-2450` - `WalkMp4IlstItems()` 显式栈 walker
  - Pattern: `src/TagReader.cpp` - `ParseStatus::Ok/NotFound/Malformed/ResourceLimit` 状态架构

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_size0_tail_ok.m4a` 退出码 0，stdout 包含 `title: Size0 Tail OK` 和 `artist: Size0 Artist`。
  - [ ] `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_size0_hides_metadata.m4a > /tmp/opencode/task8-hidden.txt 2> /tmp/opencode/task8-hidden-err.txt && grep -q 'title: After Size0' /tmp/opencode/task8-hidden.txt` 退出码 0。
  - [ ] `cmake -S . -B build-asan -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-asan && ./build-asan/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_size0_tail_ok.m4a > /tmp/opencode/task8-asan-tail.txt 2> /tmp/opencode/task8-asan-tail-err.txt && ./build-asan/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_size0_hides_metadata.m4a > /tmp/opencode/task8-asan-hidden.txt 2> /tmp/opencode/task8-asan-hidden-err.txt && ! grep -Eq 'AddressSanitizer|runtime error|heap-buffer-overflow|undefined' /tmp/opencode/task8-asan-tail-err.txt /tmp/opencode/task8-asan-hidden-err.txt` 退出码 0。

  **QA Scenarios**:
  ```
  Scenario: Happy path - size=0 位于 parent 末尾
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_size0_tail_ok.m4a
    Expected: 退出码 0；stdout 包含 `title: Size0 Tail OK` 和 `artist: Size0 Artist`；stderr 不包含 malformed
    Evidence: .omo/evidence/task-8-mp4-size0-tail.txt

  Scenario: Failure/edge - size=0 隐藏 sibling metadata
    Tool: Bash
    Steps: python3 test/security/generate_samples.py && cmake -S . -B build && cmake --build build && ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_size0_hides_metadata.m4a > /tmp/opencode/task8-hidden.txt 2> /tmp/opencode/task8-hidden-err.txt && grep -q 'title: After Size0' /tmp/opencode/task8-hidden.txt && ! grep -Eq 'AddressSanitizer|runtime error|heap-buffer-overflow' /tmp/opencode/task8-hidden-err.txt
    Expected: 命令退出码 0；stdout 包含 `title: After Size0`；stderr 无 sanitizer/UB/OOB 关键词
    Evidence: .omo/evidence/task-8-mp4-size0-hidden-sibling.txt
  ```

  **Commit**: YES | Message: `fix(mp4): handle size-zero atoms safely` | Files: [`src/TagReader.cpp`, `test/security/generate_samples.py`, `test/corpus/generate_corpus.py`]

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback -> fix -> re-run -> present again -> wait for okay.
- [x] F1. Plan Compliance Audit — oracle
  - Verify every Bug Report item has exactly one implementation task and no scope expansion.
  - Verify public API, dependency, raw-byte parsing, main-flow guardrails remain satisfied.
- [x] F2. Code Quality Review — unspecified-high
  - Inspect changed C++ for focused helpers, no giant lambdas in entry flow, no resource-limit regressions.
  - Run `lsp_diagnostics` where available and inspect compiler output.
- [x] F3. Real Manual QA — unspecified-high
  - Execute: normal build, ASAN build, `generate_samples.py`, `TagReaderSecuritySmoke`, `TagReaderTest` commands from tasks.
  - If `clang++` exists, execute fuzz build and `TagReaderFuzz -runs=1000`.
- [x] F4. Scope Fidelity Check — deep
  - Confirm Bug #6 stayed limited to music/audio Ogg Vorbis and `MusicTag` fields.
  - Confirm no TagLib/AVDictionary metadata dependency, no CI/lint/unit framework invention.

## Commit Strategy
- Use one commit per task where practical, following messages listed in each task.
- If implementation batches combine tightly coupled files, still keep commits scoped by Bug Report item.
- Do not commit `.omo/evidence/` unless repo policy explicitly asks; evidence paths are runtime artifacts for review.
- Before every commit: inspect `git status --short` and `git diff --stat`, stage only intended source/test files.

## Success Criteria
- All 8 implementation TODOs completed with task evidence files.
- F1-F4 all approve, and user explicitly says “okay” before work is marked complete.
- `ANALYSIS.md` Bug Report no longer describes reproducible current behavior for the 8 fixed issues.
- The library remains a lightweight C++23 music metadata reader with no new external tag parsing dependency.
