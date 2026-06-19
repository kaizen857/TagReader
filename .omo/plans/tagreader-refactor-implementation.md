# TagReader Refactor Implementation Plan

## TL;DR
> **Summary**: 将 `src/TagReader.cpp` 从 God Translation Unit 安全拆分为 `core/media/io/text/cover/formats/*` 内部模块，保持公共 API 与所有解析结果不变。执行主线是先冻结行为基线，再抽公共契约/工具，最后按 `TagFormat` 子目录逐个迁移 parser，并由 `TagPipeline` 使用 `DetectTagFormat -> switch` 分发。
> **Deliverables**:
> - `src/core/`、`src/media/`、`src/io/`、`src/text/`、`src/cover/`、`src/formats/{id3,vorbis,flac,ogg-vorbis,mp4}/` 内部模块
> - `CMakeLists.txt` 更新，所有新 `.cpp` 纳入 `TagReaderCore`
> - 行为基线与回归证据保存在 `.omo/evidence/`
> - `TagReader.cpp` 收敛为 public facade，内部调度移至 `TagPipeline`
> **Effort**: XL
> **Parallel**: YES - 5 waves
> **Critical Path**: Baseline evidence -> Core contracts/utilities -> Format parser migration -> TagPipeline dispatch -> final security/fuzz verification

## Context

### Original Request
用户要求：根据 `ANALYSIS.md` 制定详细代码修改/编写计划。

### Interview Summary
- 公共 API 保持 `TagReader::Read(path)` 与 `TagReader::Read(path, coverExportDir)` 不变。
- 内部执行顺序使用：FFmpeg/media handling -> `DetectTagFormat()` -> `switch` / `if` by `TagFormat` -> parser -> `BuildMusicTag()`。
- `src/formats/` 必须按标签格式使用子目录：`id3`、`vorbis`、`flac`、`ogg-vorbis`、`mp4`。
- 同一个标签格式的 metadata、lyrics、cover block、内部 helper 必须留在对应同一子目录。
- 不引入 `dynamic_cast`、RTTI、visitor、深继承、plugin registry。
- 新 parser 代码不新增 `reinterpret_cast`；现有解析算法先原样移动，不顺手改偏移、位运算、容错、字段优先级、文本规范化或封面缓存语义。

### Metis Review (gaps addressed)
- 加入行为冻结基线，任何 parser 移动前先保存 `TagReaderTest` 输出。
- 每个格式目录独立迁移并验证，避免一次性拆散导致回归难定位。
- 明确 `DetectTagFormat()` 的输入优先级：优先原始字节标记，可结合容器信息，但不得依赖 FFmpeg `AVDictionary` 作为元数据来源。
- 明确 `RawMediaInfo`、`RawMetadata`、`RawLyrics`、`DecodedField` 归属 `src/core/RawTagData.hpp`。
- 明确 `ReadContext::input` 的 `clear()` / `seekg()` 语义为 parser 入口护栏。
- 明确无 CI/lint/unit framework；不得把引入测试框架/CI/formatter 纳入本次计划。

## Work Objectives

### Core Objective
把当前集中在 `src/TagReader.cpp` 和 `include/TagReader.hpp` 私有声明里的格式解析、文本/字节工具、封面缓存、FFmpeg 媒体处理拆分为内部模块，同时保持现有解析结果、错误策略、副作用边界和公共 API 不变。

### Deliverables
- `src/core/ReadContext.hpp`
- `src/core/RawTagData.hpp`
- `src/core/TagFormat.hpp`
- `src/core/TagPipeline.hpp`
- `src/core/TagPipeline.cpp`
- `src/media/FfmpegSession.hpp/.cpp`
- `src/media/MediaInfoReader.hpp/.cpp`
- `src/media/ContainerDetector.hpp/.cpp`
- `src/io/ByteReader.hpp/.cpp`
- `src/text/TextCodec.hpp/.cpp`
- `src/text/TextNormalize.hpp/.cpp`
- `src/cover/CoverCache.hpp/.cpp`
- `src/cover/CoverDecoder.hpp/.cpp`
- `src/formats/id3/Id3Parser.hpp/.cpp`
- `src/formats/id3/Id3Frames.hpp/.cpp`
- `src/formats/vorbis/VorbisCommentParser.hpp/.cpp`
- `src/formats/flac/FlacParser.hpp/.cpp`
- `src/formats/ogg-vorbis/OggVorbisParser.hpp/.cpp`
- `src/formats/mp4/Mp4Parser.hpp/.cpp`
- `src/formats/mp4/Mp4AtomReader.hpp/.cpp`
- `CMakeLists.txt` 更新

### Definition of Done
- `cmake -S . -B build` 成功。
- `cmake --build build` 成功生成 `TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke`。
- `include/TagReader.hpp` 对外入口仍只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`。
- `rg -n "dynamic_cast|typeid\(|visitor|Plugin|Registry|ITagParser|virtual .*Parser|reinterpret_cast" src include` 不显示新增禁用抽象或新增 parser 级 `reinterpret_cast`；旧有 `reinterpret_cast` 若仍存在，必须限于原样迁移代码或 C API/legacy 边界，并记录在 evidence 中。
- 对实际可用样本运行 `./build/TagReaderTest <audio-file-path> [cover-export-dir]`，重构前后 stdout 完全一致；没有样本的格式必须在 `.omo/evidence/task-1-baseline-gaps.md` 记录为未验证。
- `./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [audio-file-path ...]` 在可用样本上通过。
- 可选：Clang 可用时 `cmake -S . -B build-fuzz-clang -DTAGREADER_ENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-fuzz-clang --target TagReaderFuzz` 通过；非 Clang 环境记录 skip。

### Must Have
- 行为基线先行。
- 每次迁移一个格式族后立即构建和回归。
- `ReadContext::input` 复用打开的文件句柄；每个 parser 入口显式管理 `clear()` / `seekg()`。
- `TagPipeline` 控制调用顺序：media/FFmpeg -> `DetectTagFormat()` -> metadata/lyrics dispatch -> normalize -> build。
- `CMakeLists.txt` 显式列出新增 `.cpp`，`TagReaderCore` 增加 `PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src` include 路径。

### Must NOT Have
- 不新增 public API。
- 不新增 parser 虚基类、继承树、factory hierarchy、plugin registry、visitor、RTTI。
- 不新增格式支持、字段支持、编码策略升级或历史 bug 修复。
- 不把 `AVDictionary` 当标签元数据来源。
- 不在 `Read(path)` 无 cover dir 时产生文件系统写入。
- 不引入 CI/lint/formatter/unit test framework。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after + existing CMake executables. 仓库没有单元测试框架，计划不引入新框架。
- QA policy: 每个任务都有 agent-executed 构建/grep/运行场景。
- Evidence path: `.omo/evidence/task-{N}-{slug}.{ext}`。
- 样本策略：优先使用真实样本路径；若仓库没有真实音频样本，执行者必须运行 `python3 test/corpus/generate_corpus.py` 并记录 corpus 路径 `/tmp/opencode/tagreader_fuzz_corpus`，但不得声称 corpus 等同真实音频全覆盖。

## Execution Strategy

### Parallel Execution Waves
> **Hard Gate**: `.omo/evidence/baseline/index.md` and comparable baseline stdout files MUST exist before any task modifies `src/`, `include/`, or `CMakeLists.txt`.

Wave 1: Task 1 baseline evidence only
Wave 2: Task 2 architecture skeleton/CMake scaffolding after baseline evidence exists
Wave 3: Task 3 core contracts -> Task 4 IO/text utilities -> Task 5 cover split
Wave 4A: Task 6 ID3 parser, Task 7 Vorbis parser, Task 10 MP4 parser
Wave 4B: Task 8 FLAC parser, Task 9 Ogg Vorbis parser after Task 7 Vorbis is complete
Wave 5: Task 11 TagPipeline/DetectTagFormat dispatch, Task 12 CMake/include cleanup, Task 13 docs/evidence update
Final: Final verification wave F1-F4

### Dependency Matrix
| Task | Depends On | Blocks |
|---|---|---|
| 1 Baseline | none | 3,4,5,6,7,8,9,10,11 |
| 2 Skeleton/CMake prep | 1 | 3,4,5,6,7,8,9,10,11 |
| 3 Core contracts | 1,2 | 4,5,6,7,8,9,10,11 |
| 4 IO/Text utilities | 3 | 6,7,8,9,10 |
| 5 Cover modules | 3,4 | 6,8,10 |
| 6 ID3 | 3,4,5 | 11 |
| 7 Vorbis | 3,4 | 8,9,11 |
| 8 FLAC | 5,7 | 11 |
| 9 Ogg Vorbis | 7 | 11 |
| 10 MP4 | 3,4,5 | 11 |
| 11 TagPipeline dispatch | 6,8,9,10 | 12,13 |
| 12 CMake/include cleanup | 11 | 13 |
| 13 Final local verification | 12 | F1-F4 |

### Agent Dispatch Summary
| Wave | Task Count | Categories |
|---|---:|---|
| 1 | 1 | unspecified-high |
| 2 | 1 | quick |
| 3 | 3 | unspecified-high |
| 4A | 3 | unspecified-high, deep |
| 4B | 2 | unspecified-high |
| 5 | 3 | unspecified-high, quick, writing |
| Final | 4 | oracle, unspecified-high, deep |

## TODOs

- [x] 1. Establish characterization baseline

  **What to do**: Build current code before refactor, collect actual sample paths, run `TagReaderTest` for each available MP3/FLAC/Ogg/MP4/sample-with-cover, save stdout to `.omo/evidence/baseline/`. If no real samples exist, run `python3 test/corpus/generate_corpus.py` and record generated corpus path plus limitation that generated seeds are not full real-audio coverage.
  **Must NOT do**: Do not edit source code. Do not invent sample paths. Do not treat missing samples as success; record gaps.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Requires careful repository/runbook handling and evidence capture.
  - Skills: [] - No special skill applies.
  - Omitted: [`webapp-testing`] - No browser/UI.

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: 2,3,4,5,6,7,8,9,10,11 | Blocked By: none

  **References**:
  - Pattern: `test/main.cpp` - `TagReaderTest` prints all public fields for baseline comparison.
  - Pattern: `test/security/security_smoke.cpp` - cover cache behavior expectations.
  - Pattern: `test/corpus/generate_corpus.py` - deterministic corpus generator.
  - Pattern: `AGENTS.md` - build commands and no CI/lint/unit framework constraint.

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build` exits 0.
  - [ ] `cmake --build build` exits 0.
  - [ ] `.omo/evidence/baseline/index.md` lists every sample path attempted and whether it covers MP3, FLAC, Ogg, MP4, lyrics, cover, malformed input.
  - [ ] For each available sample, `.omo/evidence/baseline/<safe-name>.stdout` contains `TagReaderTest` output.

  **QA Scenarios**:
  ```
  Scenario: Baseline build and field output
    Tool: Bash
    Steps: Run cmake configure/build, then run ./build/TagReaderTest for every sample listed in .omo/evidence/baseline/index.md.
    Expected: Build exits 0; every successful sample has captured stdout; missing formats are explicitly recorded.
    Evidence: .omo/evidence/task-1-baseline.md

  Scenario: Missing sample gap is explicit
    Tool: Bash
    Steps: Check .omo/evidence/baseline/index.md for MP3, FLAC, Ogg, MP4, cover, lyrics coverage rows.
    Expected: Each row is marked covered or missing; no blank/unknown coverage entries.
    Evidence: .omo/evidence/task-1-baseline-gaps.md
  ```

  **Commit**: NO | Message: `n/a` | Files: [.omo/evidence/** only]

- [x] 2. Add directory skeleton and CMake source plan

  **What to do**: After Task 1 baseline evidence exists, create empty/near-empty internal directories and header/source stubs only where needed for staged migration: `src/core`, `src/media`, `src/io`, `src/text`, `src/cover`, `src/formats/id3`, `src/formats/vorbis`, `src/formats/flac`, `src/formats/ogg-vorbis`, `src/formats/mp4`. Update `CMakeLists.txt` to include new `.cpp` stubs in `TagReaderCore` and add `PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src` include path. Stubs must not change behavior.
  **Must NOT do**: Do not move parser algorithms yet. Do not expose internal headers under `include/`. Do not rename public targets.

  **Recommended Agent Profile**:
  - Category: `quick` - Mechanical file/CMake scaffolding.
  - Skills: [] - No special skill applies.
  - Omitted: [`frontend-design`] - Not UI.

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: 3,4,5,6,7,8,9,10,11 | Blocked By: 1

  **References**:
  - Pattern: `CMakeLists.txt:45-64` - current `TagReaderCore` source/include/link structure.
  - Pattern: `ANALYSIS.md` - target directory tree.

  **Acceptance Criteria**:
  - [ ] `.omo/evidence/baseline/index.md` exists before any `src/`, `include/`, or `CMakeLists.txt` change in this task.
  - [ ] `CMakeLists.txt` still defines `TagReaderCore`, `TagReaderTest`, `TagReaderSecuritySmoke`.
  - [ ] `target_include_directories(TagReaderCore ...)` includes public `include` and private `src`.
  - [ ] `cmake --build build` exits 0 with stubs.

  **QA Scenarios**:
  ```
  Scenario: Skeleton compiles without behavior change
    Tool: Bash
    Steps: Run cmake -S . -B build && cmake --build build; run baseline sample command from task 1 on one available sample.
    Expected: Build exits 0; sample output matches baseline exactly.
    Evidence: .omo/evidence/task-2-skeleton-build.txt

  Scenario: Public include surface unchanged
    Tool: Bash
    Steps: Run rg -n "src/(core|media|io|text|cover|formats)" include CMakeLists.txt.
    Expected: No internal src path appears in public headers under include/.
    Evidence: .omo/evidence/task-2-public-surface.txt
  ```

  **Commit**: YES | Message: `refactor(build): add internal module skeleton` | Files: [CMakeLists.txt, src/core/**, src/media/**, src/io/**, src/text/**, src/cover/**, src/formats/**]

- [x] 3. Extract core contracts and TagFormat

  **What to do**: Move internal contracts from `include/TagReader.hpp` private section into `src/core/ReadContext.hpp`, `src/core/RawTagData.hpp`, and `src/core/TagFormat.hpp`: `DetectedContainer` or `ContainerFormat`, `ReadContext`, `RawMediaInfo`, `RawMetadata`, `RawLyrics`, `DecodedField`, `Id3TagView` if needed by ID3. Add `TagFormat` enum with `Unknown`, `Id3v1`, `Id3v2`, `VorbisComment`, `Flac`, `OggVorbis`, `Mp4`. Adjust `src/TagReader.cpp` includes to use internal headers.
  **Must NOT do**: Do not change public `TagReader` method signatures or `MusicTag`/`Lyrics` types. Do not change field defaults.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Header dependency surgery with compile risk.
  - Skills: [] - No special skill applies.
  - Omitted: [`doc-coauthoring`] - Code execution task, not prose.

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: 4,5,6,7,8,9,10,11 | Blocked By: 1,2

  **References**:
  - API/Type: `include/TagReader.hpp:19-159` - current private types and declarations.
  - API/Type: `include/Tag.hpp` - public `MusicTag`, must not change.
  - API/Type: `include/Lyrics.hpp` - public `Lyrics`, must not change.

  **Acceptance Criteria**:
  - [ ] `include/TagReader.hpp` still exposes only the two public `Read` overloads.
  - [ ] Internal structs compile from `src/core/*`.
  - [ ] `cmake --build build` exits 0.

  **QA Scenarios**:
  ```
  Scenario: Core contract extraction compiles
    Tool: Bash
    Steps: Run cmake --build build; run ./build/TagReaderTest on one baseline sample.
    Expected: Build exits 0; output matches baseline exactly.
    Evidence: .omo/evidence/task-3-core-contracts.txt

  Scenario: Public API unchanged
    Tool: Bash
    Steps: Run rg -n "static MusicTag Read" include/TagReader.hpp and rg -n "RawMetadata|RawLyrics|ReadContext|TagFormat" include/TagReader.hpp.
    Expected: Two public Read overloads remain; internal raw types absent from public header unless still private and justified.
    Evidence: .omo/evidence/task-3-public-api.txt
  ```

  **Commit**: YES | Message: `refactor(core): move internal tag contracts` | Files: [include/TagReader.hpp, src/core/**, src/TagReader.cpp]

- [x] 4. Extract ByteReader and TextCodec/TextNormalize

  **What to do**: Move byte helpers and text helpers from `src/TagReader.cpp` into `src/io/ByteReader.*`, `src/text/TextCodec.*`, `src/text/TextNormalize.*`. Include `ReadRange`, endian readers, syncsafe readers, safe add/limit helpers, `NormalizeText`, `DetectTextEncoding`, `DecodeTextToUtf8`, `DecodeRawText`, `NormalizeMetadata`, `NormalizeLyrics`, `ReadLyricsFromPlainText` only if shared outside a single format.
  **Must NOT do**: Do not rewrite encoding algorithms. Do not add new `reinterpret_cast`; if an existing one is moved unchanged, record it in `.omo/evidence/task-4-cast-inventory.txt`.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Shared utility extraction touches many call sites.
  - Skills: [] - No special skill applies.
  - Omitted: [`mcp-builder`] - Not MCP.

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: 6,7,8,9,10 | Blocked By: 3

  **References**:
  - Pattern: `src/TagReader.cpp:1237-2262` - numeric/text/byte helpers area.
  - Pattern: `src/TagReader.cpp:4903-5104` - normalize/decode final helpers.
  - Pattern: `ANALYSIS.md:325-336` - utility extraction requirements.

  **Acceptance Criteria**:
  - [ ] `cmake --build build` exits 0.
  - [ ] `rg -n "dynamic_cast|typeid\(|virtual .*Parser|ITagParser" src include` shows no forbidden abstraction.
  - [ ] `.omo/evidence/task-4-cast-inventory.txt` records any remaining `reinterpret_cast` locations and whether they are moved legacy code.

  **QA Scenarios**:
  ```
  Scenario: Utility extraction preserves text output
    Tool: Bash
    Steps: Run ./build/TagReaderTest on all baseline samples with text/lyrics coverage.
    Expected: stdout matches baseline exactly.
    Evidence: .omo/evidence/task-4-text-baseline-diff.txt

  Scenario: Forbidden abstraction check
    Tool: Bash
    Steps: Run rg -n "dynamic_cast|typeid\(|visitor|Plugin|Registry|ITagParser|virtual .*Parser" src include.
    Expected: No matches for newly introduced parser abstraction.
    Evidence: .omo/evidence/task-4-forbidden-abstractions.txt
  ```

  **Commit**: YES | Message: `refactor(common): extract byte and text utilities` | Files: [src/io/**, src/text/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 5. Extract CoverCache and CoverDecoder

  **What to do**: Move cover cache and cover image decode/PNG encode helpers into `src/cover/CoverCache.*` and `src/cover/CoverDecoder.*`. Preserve content-addressed path generation, existing cache validation, atomic write/link/fsync semantics, polluted cache diagnostics, and FFmpeg image decode/PNG encode behavior.
  **Must NOT do**: Do not write cover files when `coverExportDir` is empty. Do not change error message substrings required by `TagReaderSecuritySmoke` (`cover cache` and path).

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Side-effect and security-sensitive extraction.
  - Skills: [] - No special skill applies.
  - Omitted: [`webapp-testing`] - No browser.

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: 6,8,10 | Blocked By: 3,4

  **References**:
  - Pattern: `src/TagReader.cpp:319-1027` - cover cache and FFmpeg image conversion cluster.
  - Test: `test/security/security_smoke.cpp` - cover cache repeated/concurrent/polluted assertions.

  **Acceptance Criteria**:
  - [ ] `cmake --build build` exits 0.
  - [ ] `./build/TagReaderSecuritySmoke <cover-dir> <sample...>` exits 0 for available cover samples.
  - [ ] `Read(path)` without cover dir produces no PNG or cache directory writes in evidence temp dir.

  **QA Scenarios**:
  ```
  Scenario: Cover cache safety still passes
    Tool: Bash
    Steps: Run ./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_out <available-cover-samples>.
    Expected: Exit 0; repeated path stable; mtime unchanged; polluted cache rejected.
    Evidence: .omo/evidence/task-5-cover-security.txt

  Scenario: No-cover path has no side effects
    Tool: Bash
    Steps: Run ./build/TagReaderTest <sample-with-cover> without cover dir while monitoring /tmp/opencode/tagreader_no_cover_out remains absent/empty.
    Expected: No PNG/cache files created.
    Evidence: .omo/evidence/task-5-no-cover-side-effect.txt
  ```

  **Commit**: YES | Message: `refactor(cover): extract cover cache and decoder` | Files: [src/cover/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 6. Migrate ID3 parser into src/formats/id3

  **What to do**: Move ID3 metadata, frame, picture, APIC, and lyrics functions into `src/formats/id3/Id3Parser.*` and `src/formats/id3/Id3Frames.*`. Keep algorithms and control flow unchanged. Use shared `ByteReader`, `TextCodec`, `CoverCache`, `CoverDecoder` APIs only where already extracted. Ensure ID3v2 + ID3v1 priority remains current behavior.
  **Must NOT do**: Do not change unsync, extended header, frame id, lyrics timestamp, APIC parsing, or malformed frame handling.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Large format migration with behavior risk.
  - Skills: [] - No special skill applies.
  - Omitted: [`frontend-design`] - Not UI.

  **Parallelization**: Can Parallel: YES | Wave 4A | Blocks: 11 | Blocked By: 3,4,5

  **References**:
  - Pattern: `src/TagReader.cpp:3142-3654` - ID3 metadata/picture cluster.
  - Pattern: `src/TagReader.cpp:4302-4610` - ID3 lyrics cluster.
  - Pattern: `ANALYSIS.md:259-264` - `src/formats/id3/` target files.

  **Acceptance Criteria**:
  - [ ] `src/formats/id3/` contains ID3 metadata, lyrics, picture, frame helper code.
  - [ ] `cmake --build build` exits 0.
  - [ ] ID3 baseline sample stdout matches baseline exactly for all available ID3 samples.

  **QA Scenarios**:
  ```
  Scenario: ID3 sample outputs match baseline
    Tool: Bash
    Steps: Run ./build/TagReaderTest on every baseline sample tagged ID3 in .omo/evidence/baseline/index.md.
    Expected: stdout identical to corresponding .omo/evidence/baseline/*.stdout.
    Evidence: .omo/evidence/task-6-id3-diff.txt

  Scenario: ID3 cover and lyrics paths remain stable
    Tool: Bash
    Steps: Run ./build/TagReaderTest <id3-cover-or-lyrics-sample> /tmp/opencode/tagreader_id3_cover_out.
    Expected: coverPath/lyricsCount/lyrics lines match baseline; no new exceptions.
    Evidence: .omo/evidence/task-6-id3-cover-lyrics.txt
  ```

  **Commit**: YES | Message: `refactor(id3): move id3 parser into format module` | Files: [src/formats/id3/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 7. Migrate Vorbis comment parser into src/formats/vorbis

  **What to do**: Move Vorbis comment key/value parser and Vorbis lyrics entry handling into `src/formats/vorbis/VorbisCommentParser.*`. Provide functions usable by FLAC and Ogg Vorbis modules. Preserve ignored keys (`tracktotal`, `totaltracks`, `disctotal`, `totaldiscs`) and invalid-entry isolation.
  **Must NOT do**: Do not make Vorbis parser read FLAC blocks or Ogg pages; it only parses Vorbis comment entries and lyrics entry semantics.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Cross-format shared parser migration.
  - Skills: [] - No special skill applies.
  - Omitted: [`webapp-testing`] - No browser.

  **Parallelization**: Can Parallel: YES | Wave 4A | Blocks: 8,9,11 | Blocked By: 3,4

  **References**:
  - Pattern: `src/TagReader.cpp:3966-4056` - Vorbis metadata entry handling.
  - Pattern: `src/TagReader.cpp:4732-4748` - Vorbis lyrics entry handling.
  - Pattern: `ANALYSIS.md:266-272` - `src/formats/vorbis/` target role.

  **Acceptance Criteria**:
  - [ ] `src/formats/vorbis/` owns comment entry and lyrics entry logic.
  - [ ] `cmake --build build` exits 0.
  - [ ] Available Vorbis/FLAC/Ogg samples match baseline after this move.

  **QA Scenarios**:
  ```
  Scenario: Vorbis comment entry behavior unchanged
    Tool: Bash
    Steps: Run ./build/TagReaderTest on baseline FLAC/Ogg samples that contain Vorbis comments.
    Expected: stdout matches baseline exactly.
    Evidence: .omo/evidence/task-7-vorbis-diff.txt

  Scenario: Invalid Vorbis entry isolation unchanged
    Tool: Bash
    Steps: Run generated corpus sample with invalid key then valid title if available.
    Expected: Later valid fields still appear as in baseline.
    Evidence: .omo/evidence/task-7-vorbis-invalid-entry.txt
  ```

  **Commit**: YES | Message: `refactor(vorbis): move vorbis comment parser` | Files: [src/formats/vorbis/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 8. Migrate FLAC parser into src/formats/flac

  **What to do**: Move FLAC signature check, metadata block walking, Vorbis comment block handling, and PICTURE block handling into `src/formats/flac/FlacParser.*`. Call Vorbis comment parser for entry semantics and cover modules for PICTURE export.
  **Must NOT do**: Do not move generic Vorbis key/value logic into FLAC; do not change block order, malformed block tolerance, or PICTURE type selection.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Binary container block migration with cover path risk.
  - Skills: [] - No special skill applies.
  - Omitted: [`mcp-builder`] - Not MCP.

  **Parallelization**: Can Parallel: YES | Wave 4B | Blocks: 11 | Blocked By: 5,7

  **References**:
  - Pattern: `src/TagReader.cpp:3656-3763` - FLAC/Vorbis metadata entry points.
  - Pattern: `src/TagReader.cpp:4058-4120` - FLAC picture parser.
  - Pattern: `ANALYSIS.md:270-272` - `src/formats/flac/` target role.

  **Acceptance Criteria**:
  - [ ] `src/formats/flac/` owns FLAC block scanning and PICTURE handling.
  - [ ] `cmake --build build` exits 0.
  - [ ] FLAC baseline samples match exactly.

  **QA Scenarios**:
  ```
  Scenario: FLAC metadata and picture unchanged
    Tool: Bash
    Steps: Run ./build/TagReaderTest on every FLAC baseline sample, with cover dir for cover-capable sample.
    Expected: stdout and coverPath match baseline exactly.
    Evidence: .omo/evidence/task-8-flac-diff.txt

  Scenario: Malformed FLAC block tolerance unchanged
    Tool: Bash
    Steps: Run generated malformed FLAC corpus sample if available.
    Expected: Exit/status and output match baseline; no crash.
    Evidence: .omo/evidence/task-8-flac-malformed.txt
  ```

  **Commit**: YES | Message: `refactor(flac): move flac parser` | Files: [src/formats/flac/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 9. Migrate Ogg Vorbis parser into src/formats/ogg-vorbis

  **What to do**: Move Ogg Vorbis page/packet scanning and comment packet extraction into `src/formats/ogg-vorbis/OggVorbisParser.*`. Reuse `src/formats/vorbis/` for comment entry parsing. Preserve page count, scanned byte, continuation, payload size limits.
  **Must NOT do**: Do not merge Ogg page scanner into generic Vorbis parser. Do not change resource-limit behavior.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Stateful packet/page scanner migration.
  - Skills: [] - No special skill applies.
  - Omitted: [`frontend-design`] - Not UI.

  **Parallelization**: Can Parallel: YES | Wave 4B | Blocks: 11 | Blocked By: 7

  **References**:
  - Pattern: `src/TagReader.cpp:3765-3963` - Ogg Vorbis comment packet scanner.
  - Pattern: `src/TagReader.cpp:4622-4697` - Vorbis lyrics with Ogg branch.
  - Pattern: `ANALYSIS.md:274-276` - `src/formats/ogg-vorbis/` target role.

  **Acceptance Criteria**:
  - [ ] `src/formats/ogg-vorbis/` owns Ogg page/packet scan.
  - [ ] `cmake --build build` exits 0.
  - [ ] Ogg baseline samples match exactly.

  **QA Scenarios**:
  ```
  Scenario: Ogg Vorbis comments and lyrics unchanged
    Tool: Bash
    Steps: Run ./build/TagReaderTest on every Ogg baseline sample.
    Expected: stdout matches baseline exactly.
    Evidence: .omo/evidence/task-9-ogg-diff.txt

  Scenario: Ogg malformed/limit behavior unchanged
    Tool: Bash
    Steps: Run generated Ogg bad continuation/resource limit samples if available.
    Expected: Baseline-compatible output/failure; no crash.
    Evidence: .omo/evidence/task-9-ogg-malformed.txt
  ```

  **Commit**: YES | Message: `refactor(ogg): move ogg vorbis parser` | Files: [src/formats/ogg-vorbis/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 10. Migrate MP4 parser into src/formats/mp4

  **What to do**: Move MP4 atom walker, ilst metadata parsing, `covr`, `trkn`/`disk`, `©lyr`, freeform lyrics, atom state structs/helpers into `src/formats/mp4/Mp4Parser.*` and `src/formats/mp4/Mp4AtomReader.*`. Keep `WalkMp4IlstItems`, visited atom limits, atom path rules, payload limits, and malformed handling unchanged.
  **Must NOT do**: Do not split MP4 state machine into generic callbacks beyond the current walker boundary. Do not alter atom traversal order.

  **Recommended Agent Profile**:
  - Category: `deep` - Highest-risk state machine/parser migration.
  - Skills: [] - No special skill applies.
  - Omitted: [`webapp-testing`] - No browser.

  **Parallelization**: Can Parallel: YES | Wave 4A | Blocks: 11 | Blocked By: 3,4,5

  **References**:
  - Pattern: `include/TagReaderInternal.hpp` - MP4 helper structs currently available.
  - Pattern: `src/TagReader.cpp:2298-2688` - MP4 atom helpers/walker.
  - Pattern: `src/TagReader.cpp:4122-4300` - MP4 metadata parser.
  - Pattern: `src/TagReader.cpp:4699-5212` - MP4 lyrics parser.
  - Pattern: `ANALYSIS.md:278-282` - `src/formats/mp4/` target files.

  **Acceptance Criteria**:
  - [ ] `src/formats/mp4/` owns MP4 atom reader and MP4 metadata/lyrics parser.
  - [ ] `cmake --build build` exits 0.
  - [ ] MP4 baseline samples match exactly.

  **QA Scenarios**:
  ```
  Scenario: MP4 metadata and lyrics unchanged
    Tool: Bash
    Steps: Run ./build/TagReaderTest on every MP4/M4A baseline sample.
    Expected: stdout matches baseline exactly, including lyricsCount and lyrics lines.
    Evidence: .omo/evidence/task-10-mp4-diff.txt

  Scenario: MP4 malformed atom behavior unchanged
    Tool: Bash
    Steps: Run generated MP4 truncated/deep/oversized corpus samples if available.
    Expected: Baseline-compatible output/failure; no crash.
    Evidence: .omo/evidence/task-10-mp4-malformed.txt
  ```

  **Commit**: YES | Message: `refactor(mp4): move mp4 parser` | Files: [src/formats/mp4/**, src/TagReader.cpp, include/TagReaderInternal.hpp, CMakeLists.txt]

- [x] 11. Introduce TagPipeline and DetectTagFormat dispatch

  **What to do**: Create/complete `src/core/TagPipeline.*`. Move orchestration into TagPipeline: validate/open/media handling, `DetectTagFormat(context)`, `ReadMetadataByTagFormat(context, format)`, `ReadLyricsByTagFormat(context, format)`, `BuildMusicTag`. `TagReader.cpp` becomes public facade that delegates to pipeline. `DetectTagFormat()` must prioritize raw byte markers and may use container info as secondary/fallback. Keep existing ID3v2 + ID3v1 supplemental behavior and Unknown fallback.
  **Must NOT do**: Do not change observable ordering, error strategy, `NormalizeMetadata`/`NormalizeLyrics` call points, or cover export validation timing.

  **Recommended Agent Profile**:
  - Category: `deep` - Central dispatch rewrite with behavior risk.
  - Skills: [] - No special skill applies.
  - Omitted: [`docx`] - Not Word document.

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: 12,13 | Blocked By: 6,8,9,10

  **References**:
  - Pattern: `src/TagReader.cpp:1030-1054` - current top-level `Read` flow.
  - Pattern: `src/TagReader.cpp:3010-3067` - current metadata dispatch.
  - Pattern: `src/TagReader.cpp:4862-4900` - current lyrics dispatch.
  - Pattern: `ANALYSIS.md:187-210` - target interaction flow.

  **Acceptance Criteria**:
  - [ ] `TagReader.cpp` only implements public facade and delegates internally.
  - [ ] `TagPipeline` contains `DetectTagFormat`, metadata dispatch, lyrics dispatch, and build assembly.
  - [ ] `cmake --build build` exits 0.
  - [ ] All available baseline samples match exactly.

  **QA Scenarios**:
  ```
  Scenario: Pipeline dispatch preserves all sample outputs
    Tool: Bash
    Steps: Run ./build/TagReaderTest for every sample in .omo/evidence/baseline/index.md and compare stdout to baseline files.
    Expected: All comparable outputs identical.
    Evidence: .omo/evidence/task-11-pipeline-diff.txt

  Scenario: DetectTagFormat fallback documented
    Tool: Bash
    Steps: Inspect src/core/TagPipeline.* for TagFormat switch cases and Unknown handling using rg -n "DetectTagFormat|TagFormat::Unknown|switch" src/core.
    Expected: All TagFormat values have explicit case or documented fallback; Unknown retains existing fallback behavior.
    Evidence: .omo/evidence/task-11-tagformat-switch.txt
  ```

  **Commit**: YES | Message: `refactor(core): route parsing through tag pipeline` | Files: [src/core/**, src/TagReader.cpp, CMakeLists.txt]

- [x] 12. CMake/include/dependency cleanup

  **What to do**: Finalize `CMakeLists.txt` source list, include paths, and internal include dependencies. Remove stale private declarations from `include/TagReader.hpp` if no longer needed. Ensure `TagReaderCore` still links FFmpeg and optional Iconv exactly as before. Ensure `TagReaderTest`, `TagReaderSecuritySmoke`, and optional `TagReaderFuzz` still link only through `TagReaderCore`.
  **Must NOT do**: Do not rename targets. Do not add install/export rules. Do not move public headers from `include/`.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Build graph cleanup.
  - Skills: [] - No special skill applies.
  - Omitted: [`xlsx`] - Not spreadsheet.

  **Parallelization**: Can Parallel: YES | Wave 5 | Blocks: 13 | Blocked By: 11

  **References**:
  - Pattern: `CMakeLists.txt` - current targets and options.
  - Pattern: `include/TagReader.hpp` - public facade header.
  - Pattern: `ANALYSIS.md:212-310` - target file structure.

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build` exits 0 from clean or existing build dir.
  - [ ] `cmake --build build` exits 0.
  - [ ] `rg -n "src/" include` returns no internal include leakage unless justified in evidence.
  - [ ] `rg -n "add_library\(TagReaderCore|add_executable\(TagReaderTest|add_executable\(TagReaderSecuritySmoke" CMakeLists.txt` confirms targets unchanged.

  **QA Scenarios**:
  ```
  Scenario: CMake target structure unchanged
    Tool: Bash
    Steps: Run rg for target declarations and build all default targets.
    Expected: Existing target names remain; build exits 0.
    Evidence: .omo/evidence/task-12-cmake-targets.txt

  Scenario: Internal headers not public
    Tool: Bash
    Steps: Run rg -n "core/|media/|formats/|cover/|text/|io/" include.
    Expected: No internal module paths leak into public headers except include/TagReader.hpp including only public dependencies.
    Evidence: .omo/evidence/task-12-public-headers.txt
  ```

  **Commit**: YES | Message: `refactor(build): finalize internal module build graph` | Files: [CMakeLists.txt, include/TagReader.hpp, src/**]

- [x] 13. Final local verification and architecture evidence

  **What to do**: Run full baseline comparison, security smoke, sanitizer build if supported, fuzz build if Clang available, forbidden-pattern scans, and write `.omo/evidence/final-summary.md`. Record unsupported checks as skipped with reason, not passed.
  **Must NOT do**: Do not hide missing samples. Do not claim lint/CI/unit test execution.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Multi-command QA and evidence consolidation.
  - Skills: [] - No special skill applies.
  - Omitted: [`webapp-testing`] - No browser.

  **Parallelization**: Can Parallel: YES | Wave 5 | Blocks: F1-F4 | Blocked By: 12

  **References**:
  - Test: `test/main.cpp` - baseline stdout contract.
  - Test: `test/security/security_smoke.cpp` - cover cache security smoke.
  - Test: `test/fuzz/tagreader_fuzz.cpp` - optional fuzz target.
  - Pattern: `AGENTS.md` - exact build/test constraints.

  **Acceptance Criteria**:
  - [ ] `.omo/evidence/final-summary.md` lists every command run, exit status, and skipped check reason.
  - [ ] Default build succeeds.
  - [ ] All comparable `TagReaderTest` outputs match baseline exactly.
  - [ ] Security smoke passes for available samples.
  - [ ] Forbidden abstraction scan has no new disallowed patterns.

  **QA Scenarios**:
  ```
  Scenario: Full refactor regression run
    Tool: Bash
    Steps: Run default build, all baseline TagReaderTest comparisons, security smoke, forbidden-pattern scans.
    Expected: All commands exit 0 or are documented as skipped due to missing samples/compiler; all comparable outputs match.
    Evidence: .omo/evidence/final-summary.md

  Scenario: Optional sanitizer/fuzz handling
    Tool: Bash
    Steps: Attempt sanitizer build; attempt Clang fuzz build only if clang++ exists.
    Expected: Supported builds pass; unsupported fuzz is recorded as skipped, not failed.
    Evidence: .omo/evidence/task-13-sanitizer-fuzz.txt
  ```

  **Commit**: YES | Message: `test(refactor): record tagreader refactor verification` | Files: [.omo/evidence/**]

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback -> fix -> re-run -> present again -> wait for okay.
- [x] F1. Plan Compliance Audit — oracle
- [x] F2. Code Quality Review — unspecified-high
- [x] F3. Real Manual QA — unspecified-high
- [x] F4. Scope Fidelity Check — deep

## Commit Strategy
- Commit after each task that changes tracked code/build files.
- Do not combine algorithm movement with cleanup/refactoring of algorithm internals.
- Suggested commits:
  1. `refactor(build): add internal module skeleton`
  2. `refactor(core): move internal tag contracts`
  3. `refactor(common): extract byte and text utilities`
  4. `refactor(cover): extract cover cache and decoder`
  5. `refactor(id3): move id3 parser into format module`
  6. `refactor(vorbis): move vorbis comment parser`
  7. `refactor(flac): move flac parser`
  8. `refactor(ogg): move ogg vorbis parser`
  9. `refactor(mp4): move mp4 parser`
  10. `refactor(core): route parsing through tag pipeline`
  11. `refactor(build): finalize internal module build graph`
  12. `test(refactor): record tagreader refactor verification`

## Success Criteria
- Public API unchanged.
- Output fields and lyrics/cover behavior match baseline for all comparable samples.
- `src/formats/` is organized by tag format subdirectories.
- `TagReader.cpp` no longer owns format parser implementations.
- `TagPipeline` owns the sequence media handling -> `DetectTagFormat` -> switch dispatch -> build.
- No deep parser abstraction introduced.
- No unrecorded behavior change, sample gap, or skipped verification.
