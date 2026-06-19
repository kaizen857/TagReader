# TagReader `topPlan.md` 阶段 0-5 代码编写计划

## TL;DR
> **Summary**: 按 `topPlan.md` 完成阶段 0-5：先锁定现有 parser 与验证基线，再扩展检测/复用分发、Ogg/Opus 图片与 OpusTags、RIFF/IFF/DSF/DFF 容器提取、ASF/Matroska 元数据模型，最后收束裸流与文档边界。阶段 6 CUE 不实现，只通过内部边界避免未来 `ReadCue`/`ReadAlbum` 被阻塞。
> **Deliverables**:
> - 现有 ID3/Vorbis/FLAC/Ogg/MP4/APE 基线回归固定
> - 新增/明确复用型检测与分发规则
> - Ogg `METADATA_BLOCK_PICTURE` 与 OpusTags 读取路径
> - WAV/AIFF/DSF/DFF/DXD 容器内嵌标签提取
> - ASF/WMA 与 Matroska/MKA/WebM 元数据/封面读取
> - 裸 `dts`/`ac3`/`truehd` 明确不支持边界与文档矩阵
> - 每项实现对应 tests-after 回归、security/fuzz 触点和文档更新
> **Effort**: XL
> **Parallel**: YES - 6 waves
> **Critical Path**: Task 1 → Tasks 2/3 → Tasks 4-11 → Task 12 → Task 13 → Task 14 → Final Verification

## Context

### Original Request
用户要求读取 `topPlan.md`，确认文档内容，并结合网络搜索结果，综合给出一套详细代码编写计划书，完成文档中的阶段 0 到阶段 5；暂时不完成阶段 6，但要做好编写阶段 6 的代码准备。

### Interview Summary
- 范围：覆盖 `topPlan.md` 阶段 0-5，阶段 6 CUE 只做准备约束，不实现。
- 验证策略：tests-after 推荐；扩展现有回归、安全 smoke、专项测试、fuzz 资产，不新增单元测试框架，不使用 `ctest`。
- 默认能力边界：新容器最低读取常规 metadata；lyrics/cover 只在该格式模型有对应字段时实现；duration/codec/container 继续由 FFmpeg 媒体信息路径负责。
- 失败语义：FFmpeg 可 probe 但原始 parser 找不到支持 metadata 时，`Read()` 返回媒体信息与空/部分 tag 字段，不作为顶层失败。
- CUE 准备：只避免架构阻塞；阶段 0-5 不新增 `ReadCue`、`ReadAlbum`、目录输入、`std::vector<MusicTag>` public API 或批量读取抽象。

### Metis Review (gaps addressed)
- 增加强 guardrails：不改 public API，不引入 TagLib/MediaInfo/libsndfile，不用 `AVDictionary` 取 metadata，不新增 `DetectContainer()`。
- 限制 scope creep：ASF/Matroska 只做必要 metadata/tag/cover 字段，不实现完整容器语义；bare stream 只做明确不支持边界。
- 每个新格式任务都覆盖 detection、dispatch、parser、resource limits、tests、docs。
- 所有验收均为 agent 可执行命令和二元断言，不要求人工确认。

### External Specification References
- Opus/Ogg: RFC 7845 `https://datatracker.ietf.org/doc/html/rfc7845`; Xiph VorbisComment `https://wiki.xiph.org/VorbisComment`; Opus picture tag `https://www.opus-codec.org/docs/opusfile_api-0.7/structOpusPictureTag.html`。
- RIFF/WAV: Microsoft RIFF `https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-`; WAV INFO reference `https://wavref.til.cafe/chunk/info/`; RIFF MCI PDF `https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/riffmci.pdf`。
- AIFF/AIFC: McGill AIFF `https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/AIFF/AIFF.html`; Apple Sound Manager extract `https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/AIFF/Docs/MacOS_Sound-extract.pdf`。
- DSF/DFF: DSF spec `https://dsd-guide.com/sites/default/files/white-papers/DSFFileFormatSpec_E.pdf`; DSDIFF 1.5 `https://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf`。
- ASF/WMA: ASF structure `https://learn.microsoft.com/en-us/windows/win32/medfound/asf-file-structure`; ASF overview `https://learn.microsoft.com/en-us/windows/win32/wmformat/overview-of-the-asf-format`; metadata `https://learn.microsoft.com/en-us/windows/win32/wmformat/metadata`。
- Matroska/WebM: RFC 9559 `https://www.rfc-editor.org/rfc/rfc9559.html`; Matroska elements `https://www.matroska.org/technical/elements.html`; tags draft `https://www.ietf.org/archive/id/draft-ietf-cellar-tags-24.html`; WebM container `https://www.webmproject.org/docs/container/`。

## Work Objectives

### Core Objective
在不改变当前 public API 和 `ReadTag()` 主流程的前提下，把 TagReader 从当前六类完整 parser 扩展到 `topPlan.md` 阶段 0-5 的目标容器/标签来源，并以现有可执行程序验证回归、安全和资源边界。

### Deliverables
- 新增或调整 `TagFormat` / `DetectedContainer` / `ContainerFromTagFormat()` / `DetectTagFormat()` 的可复用分发，不新增独立 `DetectContainer()`。
- 新增 parser 文件只放在 `src/formats/` 对应子目录附近；容器提取器只负责定位 raw payload、边界校验和映射到 `RawMetadata` / `RawLyrics` / cover data。
- 新增/扩展 `TagReaderRegressionTests` TR-AUDIT case、security sample/fuzz corpus 覆盖点和文档矩阵。
- 为阶段 6 保留内部命名与单文件约束边界，但不实现 CUE。

### Definition of Done (verifiable conditions with commands)
- `cmake -S . -B build && cmake --build build` exit code 为 0。
- `./build/TagReaderRegressionTests --list` 输出包含新增 `TR-AUDIT-032` 到 `TR-AUDIT-056`，且旧 `TR-AUDIT-001` 到 `TR-AUDIT-031` 仍存在。
- 逐项运行新增阶段 0-5 TR-AUDIT case exit code 为 0。
- `./build/TagReaderFlacMalformedMetadataTests`、`./build/TagReaderDefaultCoverExportDirectoryTests`、`./build/TagReaderLyricsNormalizeComplexityTests` exit code 均为 0。
- `python3 test/security/generate_samples.py --out-dir /tmp/opencode/tagreader_security_samples && ./build/TagReaderSecuritySmoke /tmp/opencode/tagreader-cover-smoke /tmp/opencode/tagreader_security_samples/*` 在存在可用样本时 exit code 为 0；脚本跳过缺失 ffmpeg 样本时记录为非阻塞。
- 可选 fuzz：`cmake -S . -B build-fuzz -DTAGREADER_ENABLE_FUZZING=ON && cmake --build build-fuzz`；若 `TagReaderFuzz` 生成，则 `python3 test/corpus/generate_corpus.py --out-dir /tmp/opencode/tagreader_fuzz_corpus && ./build-fuzz/TagReaderFuzz -runs=100 /tmp/opencode/tagreader_fuzz_corpus` exit code 为 0。
- `! rg -n "ReadCue|ReadAlbum|std::vector<MusicTag>|DetectContainer\(" include src` exit code 为 0；若有任何输出即失败。

### Must Have
- 所有新增 parser 用 C++23。
- 所有文本写入 `MusicTag` 前为 UTF-8；非法局部字段跳过，不污染其它字段。
- 所有二进制读取使用绝对 offset + bounded range helper，调用前后恢复/避免污染 `ReadContext::input` 状态。
- 新增格式必须定义资源上限：总扫描量、对象/chunk/element 数量、单项 payload、图片输入输出大小。
- 封面导出沿用 content-addressed PNG cache、显式目录 symlink 拒绝、默认目录硬化、`cover cache` 错误上抛。

### Must NOT Have
- 不修改 `TagReader::Read()` public API，不新增 `ReadCue` / `ReadAlbum`，不支持目录输入。
- 不引入 TagLib、MediaInfo、libsndfile 或其它 metadata 库。
- 不用 FFmpeg `AVDictionary` 填充标题、歌手、专辑、歌词、封面块。
- 不新增 `DetectContainer()` 步骤，不重排 `ReadTag()` pipeline。
- 不把 `dts`、`ac3`、`truehd` 裸流写成已支持标签格式。
- 不把 `docs/DESIGN.md` 的最终目标写成当前能力。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after + existing executable framework。
- QA policy: 每个任务包含 agent-executed happy path 与 failure/edge scenario。
- Per-task gate: 每个任务完成代码编写后，必须立刻运行该任务 `Acceptance Criteria` 和 `QA Scenarios` 中列出的测试命令；任一命令失败则该任务状态保持未完成，必须修复 bug 并重跑同一组测试，直到全部通过后才能进入后续依赖任务或提交。
- Release gate: 一个完整功能任务只有在代码、测试、文档和 evidence 都完成且测试全绿后才算完成；失败任务不得被后续任务依赖或合并验证“带过”。
- Evidence: `.omo/evidence/task-{N}-{slug}.{ext}`，优先保存命令 stdout/stderr、回归 case 输出、样本生成日志。

## Execution Strategy

### Parallel Execution Waves
> Target: 5-8 tasks per wave. <3 per wave (except final) = under-splitting.
> Extract shared dependencies as Wave-1 tasks for max parallelism.
> 并行规则：只有同一 wave 内且 `Blocked By` 全部已完成的任务可以并行执行；任何存在前后依赖的任务必须严格按 `Dependency Matrix` 顺序执行，不能并行乱序开始。
> 放行规则：一个任务的测试未通过时，该任务阻塞所有后续依赖任务；不得为了并行度跳过失败任务。

Wave 1: Task 1 baseline inventory。
Wave 2: Task 2 dispatch model, Task 3 shared bounded helpers。
Wave 3: Task 4 reusable suffix/tag detection, Task 5 Ogg picture, Task 6 OpusTags, Task 7 RIFF/WAV, Task 8 AIFF/AIFC, Task 9 DSF/DFF/DXD, Task 10 ASF/WMA, Task 11 Matroska/MKA/WebM。
Wave 4: Task 12 bare stream/documentation boundaries。
Wave 5: Task 13 regression/security/fuzz consolidation。
Wave 6: Task 14 CUE readiness audit and docs guardrails, then Final Verification。

### Dependency Matrix (full, all tasks)
- Task 1: blocks all tasks by locking current semantics.
- Task 2: blocked by Task 1; blocks Tasks 4-12 and 14.
- Task 3: blocked by Task 1; blocks Tasks 5-11.
- Task 4: blocked by Tasks 1-2; can run parallel with Tasks 5-6 after shared decisions.
- Tasks 5-6: blocked by Tasks 2-3; independent of RIFF/ASF/Matroska.
- Tasks 7-9: blocked by Tasks 2-3; independent of Tasks 10-11.
- Tasks 10-11: blocked by Tasks 2-3; independent from each other.
- Task 12: blocked by Tasks 4-11.
- Task 13: blocked by Tasks 4-12 for final case list but can prepare harness conventions after Task 1.
- Task 14: blocked by Tasks 2, 12, and 13; must run before final verification.

### Agent Dispatch Summary
- Wave 1: 1 task → `deep` baseline lock。
- Wave 2: 2 tasks → `unspecified-high` core dispatch/helper work。
- Wave 3: 8 tasks → `deep` per independent format family。
- Wave 4: 1 task → `writing` docs boundary consolidation。
- Wave 5: 1 task → `unspecified-high` validation consolidation。
- Wave 6: 1 task → `unspecified-high` readiness audit。

### Regression Case ID Allocation
- Task 1: `TR-AUDIT-032` APE-over-ID3 baseline, `TR-AUDIT-033` ID3v2-over-ID3v1 fallback, `TR-AUDIT-034` cover cache reuse, `TR-AUDIT-035` malformed local field skip。
- Task 3: `TR-AUDIT-036` bounded reader overflow/padding/parent-range behavior。
- Task 4: `TR-AUDIT-037` reusable raw tag source dispatch and bare AAC no-tag boundary。
- Task 5: `TR-AUDIT-038` Ogg `METADATA_BLOCK_PICTURE` happy/malformed/URL/oversized behavior。
- Task 6: `TR-AUDIT-039` OpusTags happy/truncated/wrong-packet/oversized behavior。
- Task 7: `TR-AUDIT-040` WAV INFO-only, `TR-AUDIT-041` WAV INFO+ID3 merge, `TR-AUDIT-042` malformed RIFF chunk isolation。
- Task 8: `TR-AUDIT-043` AIFF native-only, `TR-AUDIT-044` AIFF native+ID3 merge, `TR-AUDIT-045` malformed AIFF chunk isolation。
- Task 9: `TR-AUDIT-046` DSF metadata pointer ID3, `TR-AUDIT-047` DSF/DFF no-tag or invalid-pointer boundary, `TR-AUDIT-048` DFF `ID3 `/`DI3v` compatibility。
- Task 10: `TR-AUDIT-049` ASF/WMA text metadata, `TR-AUDIT-050` ASF UTF-16/malformed descriptor, `TR-AUDIT-051` ASF oversized object/image boundary。
- Task 11: `TR-AUDIT-052` Matroska/MKA/WebM `SimpleTag`, `TR-AUDIT-053` Matroska/MKA/WebM attachment cover cache, `TR-AUDIT-054` EBML unknown/oversized/deep nesting boundary。
- Task 14: `TR-AUDIT-055` directory input remains rejected by `Read()`, `TR-AUDIT-056` CUE/API absence guardrail。

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.

- [x] 1. 锁定现有 parser 与回归基线

  **What to do**: 阅读并记录当前 `ID3`、`Vorbis`、`FLAC`、`Ogg Vorbis`、`MP4`、`APE` 的字段、歌词、封面、fallback 优先级与资源上限；在 `test/regression/regression_tests.cpp` 中新增阶段 0 基线 case，覆盖 APE-over-ID3、ID3v2-over-ID3v1 fallback、封面 cache reuse、malformed 局部跳过。
  **Must NOT do**: 不重构 parser；不改变已有字段优先级；不删除 `TR-AUDIT-001` 到 `TR-AUDIT-031`。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 需要跨多个 parser 和回归资产确认现有语义。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`security-research`] - 不是漏洞审计，只做回归基线。

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: 2,3,4,5,6,7,8,9,10,11,12,13,14 | Blocked By: none

  **References**:
  - Pattern: `src/core/TagPipeline.cpp` - `ReadMetadata()` / `ReadLyrics()` fallback 与流状态恢复。
  - Pattern: `src/media/ContainerDetector.cpp` - `DetectTagFormat()` APE footer 优先级。
  - Pattern: `src/formats/id3/Id3Parser.cpp` - ID3v1/v2 字段、歌词、封面入口。
  - Pattern: `src/formats/ape/ApeParser.cpp` - APE item、cover、lyrics 入口。
  - Test: `test/regression/regression_tests.cpp` - TR-AUDIT 注册和样本模式。

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build && cmake --build build` exit code 为 0。
  - [ ] `./build/TagReaderRegressionTests --list` 输出仍包含 `TR-AUDIT-001` 到 `TR-AUDIT-031`，并包含 `TR-AUDIT-032`、`TR-AUDIT-033`、`TR-AUDIT-034`、`TR-AUDIT-035`。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-032` 到 `TR-AUDIT-035` exit code 均为 0，stdout 包含对应 case id 与 pass 结果。

  **QA Scenarios**:
  ```
  Scenario: Existing parser baseline stays stable
    Tool: Bash
    Steps: cmake -S . -B build && cmake --build build && ./build/TagReaderRegressionTests --list && ./build/TagReaderRegressionTests TR-AUDIT-032 && ./build/TagReaderRegressionTests TR-AUDIT-033 && ./build/TagReaderRegressionTests TR-AUDIT-034 && ./build/TagReaderRegressionTests TR-AUDIT-035
    Expected: exit code 0; list contains old and new case ids; phase 0 case passes
    Evidence: .omo/evidence/task-1-baseline.txt

  Scenario: Malformed local field does not fail whole read
    Tool: Bash
    Steps: Run the new malformed-baseline TR-AUDIT case created in this task
    Expected: exit code 0; output shows partial tag returned and malformed field skipped
    Evidence: .omo/evidence/task-1-baseline-error.txt
  ```

  **Commit**: `锁定现有解析器回归基线` | Files: [`test/regression/regression_tests.cpp`, sample generator files if needed]

- [x] 2. 扩展 `TagFormat` / `DetectedContainer` 分发模型

  **What to do**: 在 `src/core/TagFormat.hpp`、`src/core/ReadContext.hpp`、`src/media/ContainerDetector.cpp`、`src/core/TagPipeline.cpp` 中规划并实现新格式枚举/容器枚举与分发占位：OggOpus、RiffWav、Aiff、Dsf、Dff、Asf、Matroska，以及可复用 raw tag 来源。保持 `DetectTagFormat()` 是唯一标签检测入口，`ContainerFromTagFormat()` 只映射 `TagFormat` 到 `DetectedContainer`。
  **Must NOT do**: 不新增 `DetectContainer()`；不修改 `TagReader.hpp` public API；不把 CUE 放进任何 public enum/API。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 核心枚举/分发表改动影响全局。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`qt-cpp-review`] - 本项目非 Qt。

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 4,5,6,7,8,9,10,11,12,14 | Blocked By: 1

  **References**:
  - API/Type: `src/core/TagFormat.hpp` - 新增 tag/source 类型。
  - API/Type: `src/core/ReadContext.hpp` - 新增 detected container 类型。
  - Pattern: `src/media/ContainerDetector.cpp` - detect 和 map 现有模式。
  - Pattern: `src/core/TagPipeline.cpp` - metadata/lyrics 分发表。

  **Acceptance Criteria**:
  - [ ] `! rg -n "DetectContainer\(" src include` exit code 为 0；若有输出即失败。
  - [ ] `! rg -n "ReadCue|ReadAlbum|std::vector<MusicTag>" include src` exit code 为 0；若有输出即失败。
  - [ ] `cmake --build build` exit code 为 0。

  **QA Scenarios**:
  ```
  Scenario: Dispatch compiles without API drift
    Tool: Bash
    Steps: cmake --build build && ! rg -n "DetectContainer\(" src include && ! rg -n "ReadCue|ReadAlbum|std::vector<MusicTag>" include src
    Expected: build exit code 0; no independent DetectContainer; no CUE public API
    Evidence: .omo/evidence/task-2-dispatch.txt

  Scenario: Unsupported new enum path returns partial/empty metadata not crash
    Tool: Bash
    Steps: Run a regression case for a probeable sample with no supported raw metadata
    Expected: exit code 0; MusicTag has media info and empty metadata fields
    Evidence: .omo/evidence/task-2-dispatch-error.txt
  ```

  **Commit**: `扩展标签分发模型` | Files: [`src/core/TagFormat.hpp`, `src/core/ReadContext.hpp`, `src/media/ContainerDetector.cpp`, `src/core/TagPipeline.cpp`, tests]

- [x] 3. 新增共享 bounded binary reader 约定

  **What to do**: 新增 `src/formats/common/BoundedReader.hpp` 与 `src/formats/common/BoundedReader.cpp`，命名空间使用 `tagreader_core::formats`，提供 `ReadRangeAt(ReadContext&, std::uint64_t offset, std::uint64_t size, std::uint64_t parentEnd)`、little/big-endian integer helpers、bounded cursor 和 padding helper，供 RIFF/IFF/ASF/Matroska 等容器 parser 使用；确保所有读取检查溢出、父范围、子 payload 上限和 padding，不污染 `ReadContext::input` 状态。
  **Must NOT do**: 不把所有旧 parser 全量迁移；只提供新格式使用的最小共享 helper。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 共享基础设施影响后续 parser 安全性。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`security-research`] - 不是完整安全审计。

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 5,6,7,8,9,10,11 | Blocked By: 1

  **References**:
  - Pattern: `src/formats/flac/FlacParser.cpp` - 现有 bounded `ByteCursor` 风格。
  - Pattern: `src/formats/mp4/Mp4AtomReader.hpp` - atom payload/数量上限风格。
  - Pattern: `src/core/TagPipeline.cpp` - parser 间 `context.input.clear()` 语义。
  - New File: `src/formats/common/BoundedReader.hpp` - 共享 helper 声明。
  - New File: `src/formats/common/BoundedReader.cpp` - 共享 helper 实现。

  **Acceptance Criteria**:
  - [ ] 新 helper 有覆盖父范围截断、整数溢出、padding 越界的 regression case。
  - [ ] `cmake --build build && ./build/TagReaderRegressionTests TR-AUDIT-036` exit code 为 0。

  **QA Scenarios**:
  ```
  Scenario: Bounded reader accepts valid nested ranges
    Tool: Bash
    Steps: Run bounded-reader regression case on synthetic valid chunk tree
    Expected: exit code 0; parser reads expected fields
    Evidence: .omo/evidence/task-3-bounded-reader.txt

  Scenario: Bounded reader rejects overflow range locally
    Tool: Bash
    Steps: Run bounded-reader regression case with oversized child length
    Expected: exit code 0; bad child skipped or parser returns local malformed without top-level crash
    Evidence: .omo/evidence/task-3-bounded-reader-error.txt
  ```

  **Commit**: `新增有界二进制读取工具` | Files: [`src/formats/common/BoundedReader.hpp`, `src/formats/common/BoundedReader.cpp`, `CMakeLists.txt`, regression tests]

- [x] 4. 明确可复用后缀与 raw tag 检测规则

  **What to do**: 实现/验证 `m4a`、`alac`、MP4-contained `aac` 继续走 MP4 `ilst`；`mpc`、`mp+`、`mpp`、`wv`、`tak`、`tta`、`shn` 在实际携带 APEv2/ID3 时复用现有 parser；裸 `aac` 仅当有 ID3v2 或 MP4 容器标签时读取，不能声明无标签裸流支持。
  **Must NOT do**: 不按扩展名直接宣称完整支持；不新增无标准来源的专属 parser。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 需要覆盖多后缀 detection/fallback 组合。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`librarian`] - 外部规范已收集。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 1,2

  **References**:
  - Pattern: `src/media/ContainerDetector.cpp` - APE/ID3/MP4 检测顺序。
  - Pattern: `src/formats/mp4/Mp4Parser.cpp` - MP4 `ilst` 路径。
  - Pattern: `src/formats/ape/ApeParser.cpp` - footer detection 和 fallback。
  - Test: `test/regression/regression_tests.cpp` - 新增后缀/标签组合 case。

  **Acceptance Criteria**:
  - [ ] 新增 TR-AUDIT case 覆盖 MP4 family、APEv2/ID3 fallback family、bare AAC no-tag boundary。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-037` exit code 为 0。
  - [ ] bare `aac` no-tag case 成功返回媒体信息且 metadata 为空，不失败也不伪造字段。

  **QA Scenarios**:
  ```
  Scenario: Existing parser reused by raw tag source
    Tool: Bash
    Steps: Run reuse-dispatch TR-AUDIT case with synthetic APEv2/ID3-tagged target suffix samples
    Expected: exit code 0; expected fields read through existing parser
    Evidence: .omo/evidence/task-4-reuse-dispatch.txt

  Scenario: Bare stream without metadata stays empty
    Tool: Bash
    Steps: Run bare AAC no-tag regression case
    Expected: exit code 0; no title/artist/album values are fabricated
    Evidence: .omo/evidence/task-4-reuse-dispatch-error.txt
  ```

  **Commit**: `复用原始标签检测分发` | Files: [`src/media/ContainerDetector.cpp`, parser dispatch, regression samples]

- [x] 5. 为 Ogg Vorbis 增加 `METADATA_BLOCK_PICTURE` 封面读取

  **What to do**: 在 `src/formats/ogg-vorbis/` 的 comment 解析中识别 `METADATA_BLOCK_PICTURE`，Base64 decode 后复用 FLAC picture block 解析思路；支持图片导出到现有 cover cache；URL 图片 MIME `-->` 跳过；坏 Base64/坏 picture block 只清空封面。
  **Must NOT do**: 不改变 Ogg Vorbis 文本字段和歌词映射；不把 URL 图片下载成本地封面。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 需要处理 Ogg packet、Vorbis comment、FLAC picture block 与 cover cache。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`webapp-testing`] - 非 Web UI。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - Pattern: `src/formats/ogg-vorbis/OggVorbisParser.cpp` - Ogg page/comment 提取。
  - Pattern: `src/formats/flac/FlacParser.cpp` - FLAC Picture Block 解析。
  - Pattern: `src/cover/CoverCache.cpp` - `WriteCoverAsPng()` 与 cache reuse。
  - External: `https://wiki.xiph.org/VorbisComment` - `METADATA_BLOCK_PICTURE` 字段。

  **Acceptance Criteria**:
  - [ ] 新增 Ogg picture happy/malformed/URL/oversized case。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-038` exit code 为 0。
  - [ ] 重复读取同一 Ogg picture 样本返回同一 cache path 且不重复写损坏 cache。

  **QA Scenarios**:
  ```
  Scenario: Ogg embedded picture exports through cover cache
    Tool: Bash
    Steps: Run Ogg picture TR-AUDIT case with explicit cover directory
    Expected: exit code 0; coverPath is non-empty PNG under content-addressed cache
    Evidence: .omo/evidence/task-5-ogg-picture.txt

  Scenario: Bad METADATA_BLOCK_PICTURE is skipped locally
    Tool: Bash
    Steps: Run malformed Ogg picture TR-AUDIT case
    Expected: exit code 0; metadata/lyrics remain readable; coverPath empty
    Evidence: .omo/evidence/task-5-ogg-picture-error.txt
  ```

  **Commit**: `读取 Ogg 内嵌封面` | Files: [`src/formats/ogg-vorbis/*`, `src/formats/flac/*` shared helper if needed, tests]

- [x] 6. 新增 OpusTags comment parser 路径

  **What to do**: 新增 `src/formats/opus/` 或在 Ogg family 中明确 Opus branch，解析 Ogg Opus `OpusHead` 与第二 packet `OpusTags`；复用 Vorbis Comment 字段映射、UTF-8 策略、comment 数量上限和 `METADATA_BLOCK_PICTURE` 封面逻辑。
  **Must NOT do**: 不把 Opus 当 Ogg Vorbis 小改；不要求解析音频 granule duration；不改变 FFmpeg 媒体信息职责。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 需要按 RFC 7845 处理 Opus header/comment packet 差异。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`librarian`] - RFC 已确认。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - Pattern: `src/formats/ogg-vorbis/OggVorbisParser.cpp` - Ogg page/packet 扫描。
  - Pattern: `src/formats/vorbis/VorbisCommentParser.cpp` - field/lyrics 映射。
  - API/Type: `src/formats/vorbis/VorbisCommentLimits.hpp` - comment 数量上限。
  - External: `https://datatracker.ietf.org/doc/html/rfc7845` - `OpusHead` / `OpusTags`。

  **Acceptance Criteria**:
  - [ ] OpusTags happy path 读取 title/artist/album/lyrics 与 cover。
  - [ ] 截断 OpusTags、错误 packet 顺序、oversized comment 均为局部失败或空 metadata，不崩溃。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-039` exit code 为 0。

  **QA Scenarios**:
  ```
  Scenario: OpusTags maps through Vorbis comment fields
    Tool: Bash
    Steps: Run OpusTags happy TR-AUDIT case
    Expected: exit code 0; title/artist/album/lyrics match expected UTF-8 values
    Evidence: .omo/evidence/task-6-opus-tags.txt

  Scenario: Truncated OpusTags does not fail media read
    Tool: Bash
    Steps: Run truncated OpusTags regression case
    Expected: exit code 0; metadata empty or partial; no uncaught exception
    Evidence: .omo/evidence/task-6-opus-tags-error.txt
  ```

  **Commit**: `解析 OpusTags 元数据` | Files: [`src/formats/opus/*` or Ogg family files, CMakeLists, tests]

- [x] 7. 新增 RIFF/WAV `LIST/INFO` 与内嵌 ID3v2 提取

  **What to do**: 新增 `src/formats/riff/` 或 `src/formats/wav/` parser，解析 `RIFF`/`WAVE` 小端 chunk tree；读取 `LIST`/`INFO` 中 `INAM`、`IART`、`IPRD`、`ICRD`、`IGNR`、`ICMT` 等原生字段；定位 `id3 ` / `ID3 ` chunk 并复用 ID3v2 parser；字段冲突规则：ID3v2 优先，RIFF INFO 补缺。
  **Must NOT do**: 不把 RIFF `id3 ` 当官方必有标准；不依赖文件扩展名跳过 magic 检查。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 需要容器 chunk parser、ID3 raw payload 复用和字段冲突策略。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`qt-cpp-review`] - 非 Qt。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - Pattern: `src/formats/id3/Id3Parser.cpp` - ID3v2 payload 复用目标。
  - Pattern: `src/core/RawTagData.hpp` - `RawMetadata` 字段。
  - External: Microsoft RIFF URL and WAV INFO references above。

  **Acceptance Criteria**:
  - [ ] WAV INFO-only case 读取 title/artist/album/year/genre/comment 映射字段。
  - [ ] WAV INFO + ID3 case 使用 ID3 主字段，INFO 只补缺。
  - [ ] malformed chunk size、odd padding、oversized LIST 不导致顶层 crash。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-040 && ./build/TagReaderRegressionTests TR-AUDIT-041 && ./build/TagReaderRegressionTests TR-AUDIT-042` exit code 均为 0。

  **QA Scenarios**:
  ```
  Scenario: WAV INFO and embedded ID3 merge correctly
    Tool: Bash
    Steps: Run WAV TR-AUDIT case containing both INFO and ID3 chunks
    Expected: exit code 0; ID3 fields win conflicts; INFO fills missing fields
    Evidence: .omo/evidence/task-7-wav-riff.txt

  Scenario: Malformed RIFF child chunk is isolated
    Tool: Bash
    Steps: Run malformed WAV chunk regression case
    Expected: exit code 0; invalid chunk skipped; no out-of-bounds read
    Evidence: .omo/evidence/task-7-wav-riff-error.txt
  ```

  **Commit**: `解析 WAV 元数据` | Files: [`src/formats/riff/*` or `src/formats/wav/*`, CMakeLists, dispatch, tests]

- [x] 8. 新增 AIFF/AIFC 原生 chunk 与 `ID3 ` 兼容提取

  **What to do**: 新增 `src/formats/aiff/` parser，解析 `FORM`/`AIFF`/`AIFC` 大端 chunk；读取原生 `NAME`、`AUTH`、`ANNO`、`(c) `、`COMT` 基础字段；定位非标准 `ID3 ` chunk 并复用 ID3v2 parser；字段冲突规则：ID3v2 优先，AIFF native 补缺。
  **Must NOT do**: 不把 AIFF `ID3 ` 宣称为官方标准；不解析/重写音频样本。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: 大端 IFF parser 与非标准 ID3 兼容路径需要边界控制。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`librarian`] - AIFF specs 已收集。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - Pattern: `src/formats/id3/Id3Parser.cpp` - ID3v2 reuse。
  - Pattern: `src/core/RawTagData.hpp` - native fields mapping。
  - External: AIFF/AIFC spec URLs above。

  **Acceptance Criteria**:
  - [ ] AIFF native-only case 读取 title/artist/comment/copyright。
  - [ ] AIFF native + ID3 case 使用 ID3 主字段、native 补缺。
  - [ ] big-endian length、odd chunk padding、truncated `COMT` 覆盖 regression。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-043 && ./build/TagReaderRegressionTests TR-AUDIT-044 && ./build/TagReaderRegressionTests TR-AUDIT-045` exit code 均为 0。

  **QA Scenarios**:
  ```
  Scenario: AIFF native fields map to RawMetadata
    Tool: Bash
    Steps: Run AIFF native TR-AUDIT case
    Expected: exit code 0; NAME/AUTH/ANNO fields appear in expected MusicTag fields
    Evidence: .omo/evidence/task-8-aiff.txt

  Scenario: Truncated AIFF comment chunk is local failure
    Tool: Bash
    Steps: Run truncated AIFF COMT regression case
    Expected: exit code 0; malformed comment skipped; other fields remain
    Evidence: .omo/evidence/task-8-aiff-error.txt
  ```

  **Commit**: `解析 AIFF 元数据` | Files: [`src/formats/aiff/*`, CMakeLists, dispatch, tests]

- [x] 9. 新增 DSF metadata pointer、DFF ID3 兼容与 DXD 边界

  **What to do**: 新增 `src/formats/dsd/` parser：DSF 按 `DSD ` header 的 metadata pointer 定位 ID3v2；DFF/DSDIFF 在 `FRM8` chunk tree 中兼容查找非标准 `ID3 ` 或 `DI3v` payload；DXD 不作为独立标签容器，只按实际 magic 走 WAV/FLAC/DSF 路径或返回无 metadata。
  **Must NOT do**: 不宣称 DFF 有官方标准标签；不新增 DXD 专属 metadata parser。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: DSD family 需要明确规范/非规范边界和 64-bit offset 检查。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`security-research`] - 资源边界由 task 内回归覆盖。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - Pattern: `src/formats/id3/Id3Parser.cpp` - ID3v2 reuse。
  - External: DSF and DSDIFF spec URLs above。
  - Guardrail: `topPlan.md` 阶段 3 - DXD 不是独立标签容器。

  **Acceptance Criteria**:
  - [ ] DSF valid metadata pointer case 复用 ID3v2 读取字段。
  - [ ] DSF pointer 0、越界 pointer、DFF no ID3 case 均成功返回空 metadata 或局部失败。
  - [ ] DFF `ID3 `/`DI3v` case 读取 ID3 字段但文档标为兼容非标准。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-046 && ./build/TagReaderRegressionTests TR-AUDIT-047 && ./build/TagReaderRegressionTests TR-AUDIT-048` exit code 均为 0。

  **QA Scenarios**:
  ```
  Scenario: DSF metadata pointer locates ID3v2
    Tool: Bash
    Steps: Run DSF ID3 regression case
    Expected: exit code 0; ID3 title/artist fields are read
    Evidence: .omo/evidence/task-9-dsd.txt

  Scenario: DFF without nonstandard ID3 remains successful empty metadata
    Tool: Bash
    Steps: Run DFF no-tag regression case
    Expected: exit code 0; media info present if probeable; metadata empty
    Evidence: .omo/evidence/task-9-dsd-error.txt
  ```

  **Commit**: `提取 DSD 元数据` | Files: [`src/formats/dsd/*`, CMakeLists, dispatch, tests]

- [ ] 10. 新增 ASF/WMA metadata object parser

  **What to do**: 新增 `src/formats/asf/` parser，读取 ASF object header `GUID(16)+size(8)`，只解析 Header Object 内的 `Content Description Object`、`Extended Content Description Object`、必要时 `Metadata Library Object`；映射 `Title`、`Author`、`WM/AlbumTitle`、`WM/AlbumArtist`、`WM/TrackNumber`、`WM/Year`、lyrics/description 和 picture-like binary 属性；定义 ASF object count、header size、single descriptor、image payload 上限。
  **Must NOT do**: 不实现 ASF packet/data 解复用；不从 Windows Media APIs 或 FFmpeg dictionary 读取 tag。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: GUID object tree、UTF-16LE 字符串和二进制封面映射复杂。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`mcp-builder`] - 非 MCP。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - API/Type: `src/core/RawTagData.hpp` - metadata/lyrics 输出。
  - Pattern: `src/cover/CoverCache.cpp` - picture binary 到 PNG cache。
  - External: Microsoft ASF docs and metadata URL above。
  - Constants: ASF Header `75B22630-668E-11CF-A6D9-00AA0062CE6C`, Content Description `75B22633-668E-11CF-A6D9-00AA0062CE6C`, Extended Content Description `D2D0A440-E307-11D2-97F0-00A0C95EA850`, Header Extension `5FBF03B5-A92E-11CF-8EE3-00C00C205365`。

  **Acceptance Criteria**:
  - [ ] ASF/WMA text metadata happy case 读取 title/artist/album/albumArtist/year/track。
  - [ ] UTF-16LE descriptor 正确转 UTF-8；非法 descriptor 局部跳过。
  - [ ] oversized object/descriptor/image 覆盖 regression，不能越界或顶层 crash。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-049 && ./build/TagReaderRegressionTests TR-AUDIT-050 && ./build/TagReaderRegressionTests TR-AUDIT-051` exit code 均为 0。

  **QA Scenarios**:
  ```
  Scenario: ASF content descriptors map to MusicTag
    Tool: Bash
    Steps: Run ASF metadata TR-AUDIT case
    Expected: exit code 0; Title/Author/WM Album fields match expected UTF-8 values
    Evidence: .omo/evidence/task-10-asf.txt

  Scenario: Oversized ASF object is bounded
    Tool: Bash
    Steps: Run malformed oversized ASF object regression case
    Expected: exit code 0 or expected local unsupported result; no crash; no unbounded allocation
    Evidence: .omo/evidence/task-10-asf-error.txt
  ```

  **Commit**: `解析 ASF 元数据` | Files: [`src/formats/asf/*`, CMakeLists, dispatch, tests]

- [ ] 11. 新增 Matroska/MKA/WebM tags 与附件封面 parser

  **What to do**: 新增 `src/formats/matroska/` EBML reader，读取 Segment 内 `Tags/SimpleTag` 的 `TagName`/`TagString` 文本字段，按 Matroska tag target 选择 album/track 常规字段；对 `mka`、`webm`、Matroska 都按 `Attachments/AttachedFile` 提取封面图片数据（仅支持 image MIME 且受 64 MiB 级资源上限约束）；定义 element depth、element count、payload、attachment image 上限。
  **Must NOT do**: 不实现完整 Matroska demux；不解析 `TagBinary` 为任意 metadata；不下载外部附件或引用。

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: EBML VINT、任意顺序 element 和 tag target 语义复杂。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`webapp-testing`] - 非 Web UI。

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 12,13 | Blocked By: 2,3

  **References**:
  - API/Type: `src/core/RawTagData.hpp` - fields/lyrics output。
  - Pattern: `src/cover/CoverCache.cpp` - attachment image cache。
  - External: RFC 9559, Matroska element table, tags draft, WebM docs。
  - Element IDs: `Tags 0x1254C367`, `Tag 0x7373`, `SimpleTag 0x67C8`, `TagName 0x45A3`, `TagString 0x4487`, `Attachments 0x1941A469`, `AttachedFile 0x61A7`, `FileName 0x466E`, `FileMediaType 0x4660`, `FileData 0x465C`。

  **Acceptance Criteria**:
  - [ ] MKA/Matroska tags happy case 读取 title/artist/album/year/genre。
  - [ ] Matroska/MKA/WebM attachment cover case 均能在 image MIME 且未超限时导出 PNG cache。
  - [ ] unknown element、unknown-size element、oversized attachment、deep nesting 覆盖 regression。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-052 && ./build/TagReaderRegressionTests TR-AUDIT-053 && ./build/TagReaderRegressionTests TR-AUDIT-054` exit code 均为 0。

  **QA Scenarios**:
  ```
  Scenario: Matroska SimpleTag maps to metadata fields
    Tool: Bash
    Steps: Run Matroska tags TR-AUDIT case
    Expected: exit code 0; SimpleTag fields map to expected MusicTag fields
    Evidence: .omo/evidence/task-11-matroska.txt

  Scenario: Oversized Matroska attachment is skipped locally
    Tool: Bash
    Steps: Run oversized attachment regression case
    Expected: exit code 0; metadata remains readable; coverPath empty; no unbounded allocation
    Evidence: .omo/evidence/task-11-matroska-error.txt
  ```

  **Commit**: `解析 Matroska 元数据` | Files: [`src/formats/matroska/*`, CMakeLists, dispatch, tests]

- [ ] 12. 收束裸流边界与能力矩阵文档

  **What to do**: 更新 `docs/DESIGN.md` 和必要的 `docs/` 新文档，明确“当前可读”“检测可达”“目标能力”“明确不支持”；将 `dts`、`ac3`、`truehd` 裸流标为不规划独立 tag parser，只通过 Matroska/MP4 等外层容器读取标签；记录 TagLib 仅为未来架构决策，不混用。
  **Must NOT do**: 不修改 `README.md` 作为架构事实来源；不把阶段 6 CUE 写成已支持。

  **Recommended Agent Profile**:
  - Category: `writing` - Reason: 主要是中文开发者文档与能力矩阵。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`doc-coauthoring`] - 不需要多轮文档协作流程。

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: 13,14 | Blocked By: 4,5,6,7,8,9,10,11

  **References**:
  - Doc: `topPlan.md` - 阶段 5 与总体优先级。
  - Doc: `docs/DESIGN.md` - 当前定位、最终目标、映射表。
  - Guardrail: `AGENTS.md` - 中文用户文档、README 不是架构事实来源。

  **Acceptance Criteria**:
  - [ ] `rg -n "当前完整支持|最终目标|明确不支持|检测可达" docs` 能定位能力分层说明。
  - [ ] `rg -n "dts|ac3|truehd|CUE|TagLib" docs/DESIGN.md` 显示裸流不支持、CUE 未实现、TagLib 未引入边界。
  - [ ] 文档不声称 `ReadCue`、目录输入或 TagLib 当前可用。

  **QA Scenarios**:
  ```
  Scenario: Documentation capability matrix is explicit
    Tool: Bash
    Steps: rg -n "当前完整支持|最终目标|明确不支持|检测可达" docs && rg -n "dts|ac3|truehd|CUE|TagLib" docs/DESIGN.md
    Expected: exit code 0; output distinguishes support levels and unsupported bare streams
    Evidence: .omo/evidence/task-12-doc-boundaries.txt

  Scenario: Documentation does not overclaim CUE
    Tool: Bash
    Steps: ! rg -n "已支持 CUE|当前.*CUE|Read\(.*目录|目录输入.*Read\(" docs topPlan.md
    Expected: no line claims CUE is currently implemented or part of Read()
    Evidence: .omo/evidence/task-12-doc-boundaries-error.txt
  ```

  **Commit**: `明确元数据支持边界` | Files: [`docs/DESIGN.md`, optional `docs/*.md`]

- [ ] 13. 统一阶段 0-5 回归、安全 smoke 与 fuzz 资产

  **What to do**: 为阶段 0-5 新增 TR-AUDIT case id、样本生成 helper、security smoke 样本和 fuzz corpus seed 生成逻辑；确保每类新增 parser 至少有 happy path、malformed、oversized、unknown field、cover cache reuse 或 unsupported boundary case。
  **Must NOT do**: 不提交二进制 seed；不新增 CI/ctest；不把缺失 ffmpeg CLI 的样本跳过误判为失败。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 跨所有实现任务整合验证资产。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`debugging`] - 不是定位单个运行时 bug。

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: 14 | Blocked By: 4,5,6,7,8,9,10,11,12

  **References**:
  - Test: `test/regression/regression_tests.cpp` - TR-AUDIT harness。
  - Test: `test/security/security_smoke.cpp` - cover cache/security smoke。
  - Test: `test/corpus/generate_corpus.py` - fuzz corpus generator。
  - Test: `test/security/generate_samples.py` - security sample generator。

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests --list` 包含阶段 0-5 新增 case id。
  - [ ] 阶段 0-5 新增 case 全部单独运行 exit code 为 0。
  - [ ] `python3 test/corpus/generate_corpus.py --out-dir /tmp/opencode/tagreader_fuzz_corpus` exit code 为 0 且不写入 repo 二进制 seed。
  - [ ] security sample generator 和 smoke 对新增 cover formats 有覆盖或明确跳过说明。

  **QA Scenarios**:
  ```
  Scenario: All phase 0-5 regression cases are listed and runnable
    Tool: Bash
    Steps: ./build/TagReaderRegressionTests --list && run each new phase 0-5 case id
    Expected: exit code 0 for list and each case; no missing registration
    Evidence: .omo/evidence/task-13-validation.txt

  Scenario: Fuzz/security generators stay repo-clean
    Tool: Bash
    Steps: python3 test/corpus/generate_corpus.py --out-dir /tmp/opencode/tagreader_fuzz_corpus && python3 test/security/generate_samples.py --out-dir /tmp/opencode/tagreader_security_samples && git status --short
    Expected: generators exit 0 or documented ffmpeg skip; no binary sample files added to repo
    Evidence: .omo/evidence/task-13-validation-error.txt
  ```

  **Commit**: `补齐元数据解析验证矩阵` | Files: [`test/regression/*`, `test/corpus/generate_corpus.py`, `test/security/generate_samples.py`, optional smoke code]

- [ ] 14. 阶段 6 CUE 准备边界审计

  **What to do**: 审计阶段 0-5 代码，确保新增 parser 和 dispatch 没有硬编码“所有输入只能是最终单曲 filePath 语义”之外的不可扩展假设；记录未来 CUE 可复用点（metadata normalization、cover cache、media info、single-file `Read()` 保持不变）到 `docs/DESIGN.md` 或 `topPlan.md`，但不新增 CUE API/类型实现。
  **Must NOT do**: 不新增 `RawCueSheet`、`RawCueTrack`、`ReadCue`、`ReadAlbum` 或目录 traversal。

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: 需要跨代码与文档确认未来扩展边界。
  - Skills: [] - 不需要专用技能。
  - Omitted: [`qt-cpp-docs`] - 非 Qt API 文档。

  **Parallelization**: Can Parallel: NO | Wave 6 | Blocks: Final Verification | Blocked By: 2,12,13

  **References**:
  - API/Type: `include/TagReader.hpp` - public API 不变。
  - Pattern: `src/core/TagPipeline.cpp` - `ValidatePath()` 单文件约束保持。
  - Doc: `topPlan.md` - 阶段 6 的未来目标。

  **Acceptance Criteria**:
  - [ ] `! rg -n "ReadCue|ReadAlbum|RawCue|CueSheet|std::vector<MusicTag>" include src` exit code 为 0；若有输出即失败。
  - [ ] `! (git diff -- src include | rg -n "recursive_directory_iterator|Read\(.*directory|ValidatePath.*is_directory|is_directory.*ValidatePath")` exit code 为 0；若 diff 中出现新增目录遍历或 `Read()` 目录输入路径即失败。
  - [ ] 文档明确阶段 6 未实现但未来应复用 normalization/media info/cover cache。
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-055 && ./build/TagReaderRegressionTests TR-AUDIT-056` exit code 均为 0。

  **QA Scenarios**:
  ```
  Scenario: CUE public API is not introduced
    Tool: Bash
    Steps: ! rg -n "ReadCue|ReadAlbum|RawCue|CueSheet|std::vector<MusicTag>" include src
    Expected: no matches for implemented CUE API/types in include/src
    Evidence: .omo/evidence/task-14-cue-readiness.txt

  Scenario: Directory input remains rejected by Read()
    Tool: Bash
    Steps: Run `./build/TagReaderRegressionTests TR-AUDIT-055` and `! (git diff -- src include | rg -n "recursive_directory_iterator|Read\(.*directory|ValidatePath.*is_directory|is_directory.*ValidatePath")`
    Expected: regression exit code 0; diff grep has no output; case observes expected "path is not a regular file" failure
    Evidence: .omo/evidence/task-14-cue-readiness-error.txt
  ```

  **Commit**: `保留 CUE 扩展边界` | Files: [`docs/DESIGN.md`, optional `topPlan.md`, tests if directory case absent]

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must PASS by agent-executable evidence. No human/manual approval gate.
> If any verification fails, fix the cited issue, rerun the failed verifier and affected commands, and update `.omo/evidence/final-*`.
- [ ] F1. Plan Compliance Audit — oracle
- [ ] F2. Code Quality Review — unspecified-high
- [ ] F3. Agent-Run Executable QA — unspecified-high
- [ ] F4. Scope Fidelity Check — deep

### Final Verification Commands and Evidence
- F1 evidence: `.omo/evidence/final-plan-compliance.txt` must include oracle PASS that every TODO was completed or explicitly deferred with reason.
- F2 evidence: `.omo/evidence/final-code-quality.txt` must include reviewer PASS for resource limits, UTF-8 handling, stream-state safety, and no forbidden dependencies.
- F3 evidence: `.omo/evidence/final-agent-qa.txt` must include `cmake -S . -B build && cmake --build build`, all new TR-AUDIT case runs, existing specialty executable runs, and available security/fuzz smoke commands with exit codes.
- F4 evidence: `.omo/evidence/final-scope-fidelity.txt` must include `rg` checks proving no `ReadCue`/`ReadAlbum`, no `DetectContainer()`, no TagLib dependency, no directory input path, and no `AVDictionary` metadata field usage.

## Commit Strategy
- 每个完整功能任务完成后必须提交一次；“完成”定义为该任务代码、测试、文档/evidence 均完成，且该任务全部 Acceptance Criteria 与 QA Scenarios 测试通过。
- Git 读取操作无限制，包括 `git status`、`git diff`、`git log`、`git show`、`git blame` 等。
- Git 写入操作仅允许 `git add` 和 `git commit`；禁止其它写入操作，包括但不限于 `git reset`、`git restore`、`git checkout`、`git switch`、`git revert`、`git clean`、`git stash`、`git rebase`、`git merge`、`git cherry-pick`、`git commit --amend`、`git push`。
- 每次提交前必须运行该任务对应测试并确认全绿；测试失败时不得 `git add` 或 `git commit`，必须先修复 bug 并重跑测试。
- 提交只允许包含当前任务相关文件和 evidence；不得把生成样本、fuzz corpus、cover cache、build 目录提交到仓库。
- 若某个任务因样本生成依赖 ffmpeg CLI 缺失而跳过，应在 evidence 中记录 skip 原因，并用手写最小二进制 fixture 或 parser-level synthetic bytes 补齐对应 regression。

## Success Criteria
- 阶段 0-5 所有任务完成，且 `TagReader::Read()` API、主流程顺序、raw-byte metadata 原则保持不变。
- 每个新增格式具备明确 detection、dispatch、parser、resource limits、tests、docs。
- 新增格式 failure semantics 统一：局部 malformed/oversized 跳过或清空局部结果，输入不可用/无音频流/容器无法建立才顶层失败。
- 文档明确区分当前支持、检测可达、目标能力和明确不支持，不把 CUE 或 TagLib 写成当前能力。
