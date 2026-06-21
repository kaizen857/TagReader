# external-cover-fallback - Work Plan

## TL;DR (For humans)
<!-- Fill this LAST, after the detailed plan below is written, so it summarizes the REAL plan. -->
<!-- Plain English for a non-engineer: NO file paths, NO todo numbers, NO wave/agent/tool names. -->

**What you'll get:** 所有普通音频格式都会在没有内嵌封面时，按 CUE 现有规则从同目录寻找 `cover/front/folder/album/artwork` 图片并导出为封面缓存路径。CUE 自身也会改为复用同一套逻辑，避免两份实现漂移。

**Why this approach:** 当前所有格式最终都汇聚到同一个单文件读取流水线，所以在核心读取流程中统一补封面，比逐个 parser 增加重复逻辑更安全；外部图片查找算法从 CUE 抽出为共享 helper，保证“与 CUE 一致”。

**What it will NOT do:** 不会让外部图片覆盖内嵌封面；不会递归或跨目录扫描；不会改变 `Read()` / `ReadCueSheet()` 的公开 API。

**Effort:** Medium
**Risk:** Medium - 改动位于核心读取流水线，会影响所有格式的 `coverPath` 副作用和默认封面导出目录行为。
**Decisions to sanity-check:** 默认实现位置为 core pipeline；共享 helper 要求传入已解析/已校验的非空导出目录；CUE 删除重复 fallback；测试采用 TDD，并用 MP3 加一个非 MP3 代表格式验证统一入口。

Your next move: 如果同意该计划，请用 `$start-work` 或“开始执行计划”启动实现。Full execution detail follows below.

---

> TL;DR (machine): Medium-risk core pipeline change: extract CUE sidecar cover fallback to shared cover helper, invoke it for all ReadTag formats only when embedded cover is absent, refactor CUE to reuse it, add targeted Catch2 coverage and docs.

## Scope
### Must have
- 新增共享 sidecar cover helper，行为逐项等同当前 CUE：同目录、非递归、候选名 `cover` → `front` → `folder` → `album` → `artwork`，扩展 `.png/.jpg/.jpeg/.bmp/.webp/.gif/.tiff`，最多 256 候选，单个候选最大 64 MiB，拒绝 symlink/非普通文件，读到图片后调用 `WriteCoverAsPng()`。
- 共享 helper 的接口契约必须明确：`coverExportDir` 必须是非空且已由调用方完成默认目录解析与安全校验的目录；helper 不负责选择默认目录，也不重复调用目录校验函数。
- `tagreader_core::ReadTag()` 对所有格式统一支持：只有 `ReadMetadata()` 结束后 `metadata.coverPath.empty()` 时，才尝试 sidecar cover；内嵌/容器内封面已有路径时绝不覆盖。
- `Read(path)` 使用默认封面目录时，sidecar cover 也走同一个默认目录硬化/校验；`Read(path, coverExportDir)` 使用调用方目录并保持 symlink 拒绝。
- CUE 删除私有 sidecar cover 复制实现，复用共享 helper 或依赖 `ReadTag()` 结果，最终行为保持现有 CUE 测试不变。
- 测试覆盖普通格式 fallback、内嵌优先、候选优先级、显式导出目录、无有效候选时空 coverPath、至少一个非 MP3 代表格式，以及 CUE 旧行为保持。
- 文档更新说明：外部封面 fallback 不再只属于 CUE，而是所有单文件读取在无内嵌封面时都会尝试。
### Must NOT have (guardrails, anti-slop, scope boundaries)
- 不新增或改变 public API；不新增 `ReadAlbum`；不让 `Read()` 返回批量结果。
- 不在每个格式 parser 内复制 sidecar 查找逻辑；不引入多份候选名/扩展名常量。
- 不递归扫描目录、不跨目录、不跟随 symlink、不把 CUE 文件目录当作普通格式封面目录。
- 不改标题、艺人、专辑、歌词解析来源；FFmpeg 仍只负责 probe/media info/封面解码 PNG 编码链路。
- 不吞掉 `cover export` / `cover cache` 错误；这些错误仍应向外抛出。
- 不把 malformed/oversized/non-image sidecar 当顶层读取失败；应局部跳过并保留 tag 其它字段。

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD + Catch2/CTest。先写普通格式 sidecar cover 失败测试，再实现共享 helper 与 pipeline 接入。
- Specific commands: `cmake --preset default`; `cmake --build --preset default`; `ctest -N --test-dir build/default -R "Sidecar|Cue"` 确认测试可发现；targeted `ctest --test-dir build/default -R "Sidecar|Cue" --output-on-failure`; final `ctest --preset default --output-on-failure`; sanitizer `cmake --preset sanitize && cmake --build --preset sanitize && ctest -N --test-dir build/sanitize -R "Sidecar|Cue" && ctest --preset sanitize --output-on-failure -R "Sidecar|Cue"`。
- Evidence: `.omo/evidence/task-<N>-external-cover-fallback.txt`，每个 todo 记录命令、退出码、关键断言和相关路径。

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 1: 测试先行与共享 helper 抽取，可并行准备测试样本/断言与 helper 接口设计。
- Wave 2: core pipeline 接入、CUE 去重、文档同步，依赖 Wave 1 helper。
- Wave 3: 全量验证、sanitize 定向验证、提交整理。

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2,3,4 | none |
| 2 | 1 | 3,4 | none |
| 3 | 2 | 5,6 | 4 |
| 4 | 2 | 5,6 | 3 |
| 5 | 3,4 | 6,7 | none |
| 6 | 5 | 7 | none |
| 7 | 5,6 | final wave | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 1. 新增普通格式 sidecar cover 回归测试（先失败）
  What to do / Must NOT do: 在 `test/regression/sidecar_cover_catch2_tests.cpp` 新增独立测试，并在 `test/CMakeLists.txt` 新增 `TagReaderSidecarCoverCatch2Tests` target：包含 `regression/catch2_regression_support.cpp`、`regression/catch2_sample_support.cpp`、新测试文件，链接 `Catch2::Catch2WithMain` 和 `TagReaderCore`，调用 `tagreader_enable_sanitizers()`、`target_include_directories()`、`catch_discover_tests()`。测试名必须包含 `Sidecar`，保证 `ctest -R Sidecar` 可发现。测试必须通过 `TagReader::Read(audioPath, exportDir)` 覆盖普通单文件 MP3：无内嵌封面时同目录 `cover.jpg` 生效；同目录同时存在 `folder.png` 与 `cover.jpg` 时 `cover` 优先；音频已有 APIC 时外部 `cover.jpg` 不覆盖。不得依赖人工样本；使用 `GenerateBaseMp3()`、`GenerateCoverSample()`、`OneByOnePng()`、`OneByOneJpeg()`。
  Parallelization: Wave 1 | Blocked by: none | Blocks: 2,3,4
  References (executor has NO interview context - be exhaustive): `test/regression/catch2_sample_support.cpp:7`, `test/regression/catch2_sample_support.cpp:61`, `test/regression/catch2_sample_support.cpp:79`, `test/regression/cue_catch2_tests.cpp:268`, `test/regression/cue_catch2_tests.cpp:293`, `test/CMakeLists.txt:77`, `test/CMakeLists.txt:94`。
  Acceptance criteria (agent-executable): `cmake --build --preset default --target TagReaderSidecarCoverCatch2Tests` 成功；`ctest -N --test-dir build/default -R Sidecar` 至少列出新增测试；`ctest --test-dir build/default -R Sidecar --output-on-failure` 在实现前至少出现预期失败，证明确实锁住新需求。
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build/default -R Sidecar --output-on-failure` 断言无内嵌 MP3 使用 `cover.jpg` 导出的路径等于 `WriteCoverAsPng(exportDir, OneByOneJpeg())`；failure: 同目录 malformed/非图片或无候选时 `coverPath().empty()` 且读取仍返回媒体 tag。Evidence `.omo/evidence/task-1-external-cover-fallback.txt`。
  Commit: Y | test(cover): add sidecar cover fallback coverage

- [x] 2. 抽出 CUE 外部封面算法为共享 cover helper
  What to do / Must NOT do: 新增 `src/cover/SidecarCover.hpp` 与 `src/cover/SidecarCover.cpp`，把 CUE 当前候选名、扩展名、大小限制、目录扫描、排序、读文件、`WriteCoverAsPng()` 调用迁移进去，命名建议 `tagreader_cover::ExportSidecarCover(audioPath, coverExportDir)`。helper 接收已解析音频路径和已校验/已选定的非空 cover export dir；不得在 helper 内调用 `DefaultCoverExportDir()`、`ValidateCoverExportDir()` 或 `ValidateDefaultCoverExportDir()`，避免 core 与 CUE 目录语义分叉。空 `coverExportDir` 只作为防御性分支返回空，不得成为生产调用路径。把新 `.cpp` 加入根 `CMakeLists.txt` 的 `TagReaderCore` 源列表。
  Parallelization: Wave 1 | Blocked by: 1 | Blocks: 3,4
  References (executor has NO interview context - be exhaustive): `src/formats/cue/CueReader.cpp:28`, `src/formats/cue/CueReader.cpp:36`, `src/formats/cue/CueReader.cpp:84`, `src/formats/cue/CueReader.cpp:102`, `src/formats/cue/CueReader.cpp:129`, `src/formats/cue/CueReader.cpp:141`, `src/formats/cue/CueReader.cpp:150`, `src/formats/cue/CueReader.cpp:198`, `src/cover/CoverCache.hpp:10`, `src/cover/CoverCache.cpp:391`, `CMakeLists.txt:71`。
  Acceptance criteria (agent-executable): `cmake --build --preset default --target TagReaderCore` 编译通过；`rg -n "ExportSidecarCover|SidecarCover" src/cover src/formats/cue src/core CMakeLists.txt` 显示 helper 定义在 `src/cover` 且 CMake 包含新源文件。
  QA scenarios (name the exact tool + invocation): happy: 临时使用新增 Sidecar tests 构建到链接阶段，确认 helper 可被测试/核心链接；failure: `rg -n "DefaultCoverExportDir|ValidateCoverExportDir|ValidateDefaultCoverExportDir" src/cover/SidecarCover.*` 无命中，确认默认目录选择仍由 caller 完成。Evidence `.omo/evidence/task-2-external-cover-fallback.txt`。
  Commit: Y | refactor(cover): extract sidecar cover helper

- [x] 3. 在 `ReadTag()` 中统一接入 sidecar fallback
  What to do / Must NOT do: 修改 `src/core/TagPipeline.cpp` 引入 `cover/SidecarCover.hpp`；在 `RawMetadata metadata = ReadMetadata(context, tagFormat);` 后、`ReadLyrics()` 前或 `BuildMusicTag()` 前检查 `metadata.coverPath.empty()`，仅为空时调用 `tagreader_cover::ExportSidecarCover(context.filePath, context.coverExportDir)` 并写回 `metadata.coverPath`。不得改变 `ReadMetadata()` 分发，不得改变 `BuildMusicTag()` 字段映射，不得让外部封面覆盖内嵌封面。
  Parallelization: Wave 2 | Blocked by: 2 | Blocks: 5,6
  References (executor has NO interview context - be exhaustive): `src/core/TagPipeline.cpp:332`, `src/core/TagPipeline.cpp:533`, `src/core/TagPipeline.cpp:577`, `src/core/TagPipeline.cpp:609`, `src/core/TagPipeline.cpp:617`, `src/core/TagPipeline.cpp:633`, `src/core/TagPipeline.cpp:636`, `src/core/TagPipeline.hpp:17`。
  Acceptance criteria (agent-executable): `ctest --test-dir build/default -R Sidecar --output-on-failure` 通过；新增测试证明普通 MP3 无内嵌时有 sidecar `coverPath`，有 APIC 时仍使用内嵌封面缓存路径；另一个非 MP3 代表格式在同一 core pipeline 下也能获得 sidecar `coverPath`。
  QA scenarios (name the exact tool + invocation): happy: `TagReader::Read(base.mp3, exportDir)` 在同目录 `cover.jpg` 存在时返回 `coverPath` 且位于 `exportDir` 下；failure: `GenerateCoverSample(audio-with-cover.mp3)` + 外部 `cover.jpg` 时，返回路径等于内嵌 PNG 的缓存路径而不是外部 JPEG；representative: 生成一个非 MP3 样本（推荐 WAV 或 FLAC，优先使用仓库现有样本生成 helper，如无现成 helper 则在测试内用 ffmpeg 生成短音频）并断言 sidecar `cover.jpg` 生效。Evidence `.omo/evidence/task-3-external-cover-fallback.txt`。
  Commit: Y | feat(cover): apply sidecar fallback to ReadTag

- [x] 4. CUE 改为复用统一 sidecar 逻辑并删除重复 fallback
  What to do / Must NOT do: 修改 `src/formats/cue/CueReader.cpp`，删除私有 `kCueCoverNames`、`kCueCoverExtensions`、`EqualsIgnoreCase()`、`IsCueCoverFile()`、`ResolveCueCoverExportDir()`、`ExportCueSidecarCover()` 和两处 `audioTag.coverPath().empty()` fallback 块；保留 `ReadTag(resolution.resolvedPath, coverExportDir)`，因为统一 fallback 后 `audioTag` 已带封面。不得改变 CUE 路径解析、metadata 应用、timing 失败策略，也不得改变 `catch (const std::exception &ex)` 中 `IsCoverExportOrCacheError(ex.what())` 时继续向外抛的边界。
  Parallelization: Wave 2 | Blocked by: 2 | Blocks: 5,6
  References (executor has NO interview context - be exhaustive): `src/formats/cue/CueReader.cpp:1`, `src/formats/cue/CueReader.cpp:8`, `src/formats/cue/CueReader.cpp:28`, `src/formats/cue/CueReader.cpp:116`, `src/formats/cue/CueReader.cpp:129`, `src/formats/cue/CueReader.cpp:275`, `src/formats/cue/CueReader.cpp:294`, `src/formats/cue/CueReader.cpp:310`, `src/formats/cue/CueReader.cpp:331`。
  Acceptance criteria (agent-executable): `ctest --test-dir build/default -R Cue --output-on-failure` 通过；`rg -n "ExportCueSidecarCover|kCueCoverNames|kCueCoverExtensions|directory_iterator" src/formats/cue/CueReader.cpp` 无命中；`rg -n "IsCoverExportOrCacheError" src/formats/cue/CueReader.cpp` 仍有命中。
  QA scenarios (name the exact tool + invocation): happy: 既有 `cue read falls back to same-directory cover priority when audio lacks embedded cover` 仍通过；failure: `cue read keeps embedded cover over same-directory fallback` 仍通过。Evidence `.omo/evidence/task-4-external-cover-fallback.txt`。
  Commit: Y | refactor(cue): reuse shared sidecar cover fallback

- [x] 5. 补齐安全/失败边界测试
  What to do / Must NOT do: 在 sidecar cover Catch2 测试中增加失败边界：同目录候选为 symlink 时跳过；候选扩展不在允许列表时跳过；候选图像 malformed 时 `coverPath` 为空但 `Read()` 不失败；候选文件大于 64 MiB 时跳过（可用 sparse/seek 写文件，避免提交二进制）。不得要求人工准备文件。
  Parallelization: Wave 2 | Blocked by: 3,4 | Blocks: 6,7
  References (executor has NO interview context - be exhaustive): `src/formats/cue/CueReader.cpp:141`, `src/formats/cue/CueReader.cpp:150`, `src/formats/cue/CueReader.cpp:180`, `src/cover/CoverCache.cpp:391`, `test/regression/catch2_sample_support.cpp:61`, `test/regression/cue_catch2_tests.cpp:304`。
  Acceptance criteria (agent-executable): `ctest --test-dir build/default -R Sidecar --output-on-failure` 通过，且测试输出/断言覆盖 symlink、bad extension、malformed image、oversized candidate、无有效候选、无效显式导出目录。
  QA scenarios (name the exact tool + invocation): happy: 合法 `cover.jpg` 被导出；failure: `cover.jpg` symlink、`cover.txt`、`cover.jpg` 非图片、`cover.png` 超 64 MiB 均不产生 `coverPath`，但 `title/format/duration` 等基础读取结果仍可用；error-propagation: 显式 `coverExportDir` 为 symlink 或不可用路径时，仍通过现有 `ValidateCoverExportDir()` / cover cache 路径抛出错误而不是被 sidecar helper 吞掉。Evidence `.omo/evidence/task-5-external-cover-fallback.txt`。
  Commit: Y | test(cover): cover sidecar failure boundaries

- [x] 6. 更新文档与仓库 agent notes
  What to do / Must NOT do: 更新 `AGENTS.md` 的“封面副作用”段落和 `docs/DESIGN.md` 中相关边界，说明 `Read(path)` / `Read(path, coverExportDir)` 在无内嵌封面时会查找音频同目录外部封面；CUE 只是复用同一规则。不得把目录扫描、递归扫描、跨目录封面写成支持能力。
  Parallelization: Wave 2 | Blocked by: 3,4 | Blocks: 7
  References (executor has NO interview context - be exhaustive): `AGENTS.md` 当前“封面副作用”段落，`docs/DESIGN.md` 当前 CUE 边界段落，`src/core/TagPipeline.cpp:609`, `src/formats/cue/CueReader.cpp:294`。
  Acceptance criteria (agent-executable): `rg -n "外部封面|sidecar|cover/front/folder|递归|跨目录" AGENTS.md docs/DESIGN.md` 能找到新边界；文档不声称递归/目录级扫描。
  QA scenarios (name the exact tool + invocation): happy: 文档描述普通 `Read()` 与 CUE 一致；failure: 搜索确认没有“目录级扫描已支持”“递归封面查找”等越界表述。Evidence `.omo/evidence/task-6-external-cover-fallback.txt`。
  Commit: Y | docs(cover): document sidecar cover semantics

- [x] 7. 执行默认与 sanitizer 验证并整理提交
  What to do / Must NOT do: 按项目入口运行默认配置、默认构建、定向测试、全量默认测试、sanitizer 定向测试。只修复本计划引入的问题；不要顺手修改无关格式解析或历史测试。
  Parallelization: Wave 3 | Blocked by: 5,6 | Blocks: final wave
  References (executor has NO interview context - be exhaustive): `AGENTS.md` 构建与验证段落，`CMakeLists.txt:16`, `test/CMakeLists.txt:274`。
  Acceptance criteria (agent-executable): `cmake --preset default`、`cmake --build --preset default`、`ctest -N --test-dir build/default -R "Sidecar|Cue"`、`ctest --test-dir build/default -R "Sidecar|Cue" --output-on-failure`、`ctest --preset default --output-on-failure`、`cmake --preset sanitize`、`cmake --build --preset sanitize`、`ctest -N --test-dir build/sanitize -R "Sidecar|Cue"`、`ctest --preset sanitize --output-on-failure -R "Sidecar|Cue"` 全部通过或记录环境性跳过。
  QA scenarios (name the exact tool + invocation): happy: 所有命令退出 0，`ctest -N` 确认新增 Sidecar 与既有 Cue 测试被命中并通过；failure: 若 ffmpeg CLI/codec 缺失导致样本生成跳过，记录具体 SKIP/失败原因，不伪造通过。Evidence `.omo/evidence/task-7-external-cover-fallback.txt`。
  Commit: Y | chore(cover): verify sidecar fallback rollout

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [x] F1. Plan compliance audit
- [x] F2. Code quality review
- [x] F3. Real manual QA
- [x] F4. Scope fidelity

## Commit strategy
- 推荐 5 个原子提交：
  1. `test(cover): add sidecar cover fallback coverage` — 新增普通格式 sidecar cover TDD 测试和 CMake target。
  2. `refactor(cover): extract sidecar cover helper` — 抽共享 helper、加入 CMake，不接入核心行为。
  3. `feat(cover): apply sidecar fallback to ReadTag` — core pipeline 接入，普通格式测试转绿。
  4. `refactor(cue): reuse shared sidecar cover fallback` — CUE 删除重复实现，既有 CUE 测试保持通过。
  5. `docs(cover): document sidecar cover semantics` — 更新 AGENTS/docs 与最终验证证据。
- 每个提交必须包含其直接测试或文档；不要把产品代码、测试、文档、`.omo` 证据全部揉成一个提交。
- 若用户要求一次性提交，仍按上述原子提交顺序执行；当前分支是 `main`，禁止历史重写。

## Success criteria
- `TagReader::Read(audio.mp3, exportDir)` 在 `audio.mp3` 无内嵌封面且同目录有合法 `cover.jpg` 时返回非空 `coverPath`，路径位于 `exportDir` 的 SHA-256 分片 PNG cache 下。
- `TagReader::Read(audio-with-apic.mp3, exportDir)` 在同目录也有合法外部封面时仍使用内嵌封面，外部封面不覆盖。
- `TagReader::Read()` 无显式目录时，外部封面走默认私有封面目录，与现有内嵌封面导出策略一致。
- `TagReader::ReadCueSheet()` 的外部封面行为与现有 CUE 测试一致，但 `CueReader.cpp` 不再有私有目录扫描/封面候选实现。
- malformed/oversized/symlink/非允许扩展 sidecar 文件被局部跳过，不导致普通 tag 读取失败。
- 显式导出目录错误和 cover cache 污染错误仍按现有 `cover export` / `cover cache` 语义向外抛出，不被 sidecar helper 吞掉。
- `ctest -N --test-dir build/default -R Sidecar` 能发现新增测试，避免“构建了测试但 CTest 没注册”的假阳性。
- 默认全量测试与 sanitizer 定向测试通过；所有验证证据写入 `.omo/evidence/task-*-external-cover-fallback.txt`。
