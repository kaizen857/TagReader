# catch2-ctest-presets - Work Plan

## TL;DR (For humans)
**What you'll get:** 项目测试会从自建 runner 迁移到标准 Catch2 + CTest，并用 CMake presets 统一普通、sanitizer、fuzz 构建目录。迁移期间先并行保留旧测试并验证覆盖等价，确认后再删除旧 runner。

**Why this approach:** Catch2 最贴合当前大量命名回归 case 的结构，CTest 负责统一发现、单案运行、标签筛选和自动化输出；CMakePresets 把所有构建产物收束到根目录的 `build/` 下，同时给 clangd 稳定的编译数据库。

**What it will NOT do:** 不改变 TagReader 解析行为或 public API；不删除人工字段打印工具；不把 fuzz 变成默认测试；不提交生成样本或 build 产物。

**Effort:** Large
**Risk:** Medium - 主要风险是 `TR-AUDIT-001..056`、security smoke 和生成样本语义必须保持等价。
**Decisions to sanity-check:** Catch2 依赖采用系统包优先、FetchContent fallback；clangd 通过 `.clangd` 指向 `build/default`；旧 runner 只在新测试覆盖确认后删除。

Your next move: 可以直接 `$start-work` 执行该计划，或先要求我运行一次高精度 Momus 计划审查。Full execution detail follows below.

---

> TL;DR (machine): Large effort / Medium risk; migrate custom tests to Catch2 v3 + CTest + CMakePresets with side-by-side parity before deleting old runners.

## Scope
### Must have
- `CMakePresets.json` 提供 `default`、`sanitize`、`fuzz` configure/build/test 工作流，所有 `binaryDir` 位于 `${sourceDir}/build/<preset>`。
- 默认 preset 设置 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，并新增 clangd 可识别的项目配置，使 clangd 使用 `build/default/compile_commands.json`。
- 根 `CMakeLists.txt` 启用 `include(CTest)` / `BUILD_TESTING`，测试目标移入 `test/CMakeLists.txt` 或等价的测试子模块入口。
- Catch2 v3 通过 `find_package(Catch2 3 CONFIG QUIET)` 优先获取；找不到时使用固定版本 `FetchContent` fallback。
- 先新增 Catch2/CTest 目标并保留旧自建测试目标；新测试覆盖并验证旧目标全部行为后，再删除旧 runner 代码与旧目标。
- `TR-AUDIT-001..056` 必须在 Catch2 测试名称或标签中保留 case id，并能通过 `ctest -R TR-AUDIT-032 --output-on-failure` 单独运行。
- `TagReaderFlacMalformedMetadataTests`、`TagReaderDefaultCoverExportDirectoryTests`、`TagReaderLyricsNormalizeComplexityTests` 的语义必须迁移到 Catch2/CTest。
- `TagReaderSecuritySmoke` 的样本生成、cover cache 复用/并发/污染检查必须由 CTest 可执行地覆盖。
- `TagReaderFuzz` 继续只在 fuzz preset / `TAGREADER_ENABLE_FUZZING=ON` 下可用，默认 preset 不构建、不运行 fuzz。
- `AGENTS.md`、`docs/DESIGN.md`、`test/corpus/README.md` 同步更新新测试入口和构建目录。
- 执行迁移前必须先隔离当前已存在的 `.omo` 删除状态和其它非本计划改动，避免后续迁移提交混入旧规划产物清理。
### Must NOT have (guardrails, anti-slop, scope boundaries)
- 不改变 `include/TagReader.hpp`、`src/TagReader.cpp`、`src/core/TagPipeline.cpp` 的 public API 或读取主流程。
- 不修改解析器行为来“适配测试”。
- 不删除 `test/main.cpp` / `TagReaderTest` 人工字段打印工具。
- 不提交 `build/`、生成音频样本、fuzz corpus、cover cache 或 `/tmp/opencode` 内容。
- 不把 `ctest` 默认测试设计成依赖 libFuzzer、Clang-only fuzz 目标或无限 fuzz run。
- 不保留文档中的旧 `build-sanitize` / `build-fuzz` 根目录构建方式作为推荐入口。
- 不把迁移计划文件、旧 `.omo` 删除状态或其它既有脏工作树内容混入测试框架迁移提交。

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: tests-after + side-by-side parity. 每个迁移任务先保留旧测试，新增 Catch2/CTest 后运行旧入口和新入口对照；最终删除旧 runner 前必须全量通过。
- Framework: Catch2 v3 + CTest + CMakePresets; fuzz keeps libFuzzer opt-in.
- Evidence: `.omo/evidence/task-<N>-catch2-ctest-presets.txt`，最终审查使用 `.omo/evidence/final-*-catch2-ctest-presets.txt`。

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 0: dirty-worktree isolation and commit-scope guard.
- Wave 1: build/test infrastructure foundation, clangd compile database, Catch2 dependency skeleton.
- Wave 2: migrate small specialty regression tests and establish CTest discovery/parity pattern.
- Wave 3: migrate the large `TR-AUDIT-001..056` regression surface in batches while old runner remains.
- Wave 4: integrate security smoke, generators, fuzz preset, and resource locking.
- Wave 5: delete old runners/targets after parity, update docs, run final full verification.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 0 | none | 1,2,3,4,5,6,7,8,9 | none |
| 1 | 0 | 2,3,4,5,6,7,8,9 | none |
| 2 | 1 | 3,4,5,6,7,8,9 | none |
| 3 | 2 | 8,9 | 4 |
| 4 | 2 | 8,9 | 3 |
| 5 | 3,4 | 8,9 | 6 |
| 6 | 3,4 | 8,9 | 5 |
| 7 | 3,4 | 8,9 | 5,6 |
| 8 | 5,6,7 | 9 | none |
| 9 | 8 | Final verification | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 0. 隔离当前脏工作树与 `.omo` 删除状态
  What to do / Must NOT do: 在任何迁移实现前检查 `git status --short`，识别当前已存在的 `.omo` 删除、`.omo/drafts` / `.omo/plans` 计划产物、以及 `AGENTS.md`、`docs/DESIGN.md`、`test/*`、`topPlan.md` 等既有改动；按用户决策把这些旧改动恢复、单独提交或明确排除出后续迁移提交范围。若用户此前确认删除 `.omo`，则应先将 `.omo` 删除作为独立 cleanup commit 或在后续提交策略中明确永不 stage `.omo` 旧删除。不得在测试框架迁移提交中混入旧 `.omo` 删除状态或非本任务源码改动。
  Parallelization: Wave 0 | Blocked by: none | Blocks: 1,2,3,4,5,6,7,8,9
  References (executor has NO interview context - be exhaustive): 当前 `git status --short` 显示大量 `.omo/*` 删除和多个非 `.omo` 修改；`.omo/plans/catch2-ctest-presets.md` 是本计划文件；用户已要求 `.omo` 旧任务产物可全部删除。
  Acceptance criteria (agent-executable): `git status --short` 被保存到 `.omo/evidence/task-0-catch2-ctest-presets.txt`；迁移实现开始前，`git diff --name-status -- .omo` 要么为空，要么已作为独立 cleanup commit 处理，要么后续每个 commit 步骤明确使用 pathspec 排除 `.omo` 旧删除；`git diff --name-status -- CMakeLists.txt test docs AGENTS.md CMakePresets.json .clangd` 只显示当前迁移任务允许的文件。
  QA scenarios (name the exact tool + invocation): Happy: Bash `git status --short | tee .omo/evidence/task-0-catch2-ctest-presets.txt` 并确认 worker 记录旧改动处理决策。Failure: Bash `git diff --name-status -- .omo | tee .omo/evidence/task-0-catch2-ctest-presets-error.txt`；若仍有 `.omo` 旧删除，则不得继续 Todo 1，除非已有用户确认的独立 cleanup commit/排除策略记录在 evidence 中。
  Commit: Optional | chore(meta): isolate existing omo cleanup state

- [x] 1. 建立 Presets、CTest 与 clangd 编译数据库基础
  What to do / Must NOT do: 新增 `CMakePresets.json`，把 `default`、`sanitize`、`fuzz` 的 `binaryDir` 固定到 `${sourceDir}/build/default`、`${sourceDir}/build/sanitize`、`${sourceDir}/build/fuzz`；default/sanitize 均设置 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`；新增 `.clangd`，让 clangd 使用 `build/default` 的 compilation database；根 `CMakeLists.txt` 加入 `include(CTest)` 和 `BUILD_TESTING` 入口，但暂时不删除任何旧测试目标。不得把 build 目录放回根级 `build-sanitize` 或 `build-fuzz`。
  Parallelization: Wave 1 | Blocked by: 0 | Blocks: 2,3,4,5,6,7,8,9
  References (executor has NO interview context - be exhaustive): `CMakeLists.txt:1`, `CMakeLists.txt:10`, `CMakeLists.txt:103`, `AGENTS.md:35`, `AGENTS.md:37`
  Acceptance criteria (agent-executable): `cmake --preset default` exit code 0；`test -f build/default/compile_commands.json` exit code 0；`cmake --build --preset default` exit code 0；`cmake --preset sanitize` exit code 0；`cmake --preset fuzz` exit code 0 或在非 Clang/libFuzzer 环境中给出明确 warning 且不影响 default。
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --preset default && cmake --build --preset default && test -f build/default/compile_commands.json`，Evidence `.omo/evidence/task-1-catch2-ctest-presets.txt`。Failure: Bash `! test -d build-sanitize && ! test -d build-fuzz && rg -n "CompilationDatabase: build/default|CMAKE_EXPORT_COMPILE_COMMANDS" .clangd CMakePresets.json`，Evidence `.omo/evidence/task-1-catch2-ctest-presets-error.txt`。
  Commit: Y | build(test): add CMake presets and compile database config

- [x] 2. 集成 Catch2 v3 依赖与 CTest 发现骨架
  What to do / Must NOT do: 添加 Catch2 v3 获取逻辑：优先 `find_package(Catch2 3 CONFIG QUIET)`，找不到时 `FetchContent` 拉取固定 v3 release；新增 `test/CMakeLists.txt`，通过 `add_subdirectory(test)` 或等价方式从根 CMake 接入；创建最小 Catch2 smoke 测试目标并使用 `include(Catch)` / `catch_discover_tests()` 注册到 CTest。不得删除旧 `TagReaderRegressionTests` 等目标。
  Parallelization: Wave 1 | Blocked by: 1 | Blocks: 3,4,5,6,7,8,9
  References (executor has NO interview context - be exhaustive): `CMakeLists.txt:41`, `CMakeLists.txt:103`, `test/regression/lyrics_normalize_complexity_tests.cpp:19`, `test/regression/regression_tests.cpp:134`
  Acceptance criteria (agent-executable): `cmake --preset default && cmake --build --preset default` exit code 0；`ctest --test-dir build/default -N` 输出至少一个 Catch2/CTest 测试；旧目标 `./build/default/TagReaderRegressionTests --list` 仍可运行。
  QA scenarios (name the exact tool + invocation): Happy: Bash `ctest --test-dir build/default -N | tee .omo/evidence/task-2-catch2-ctest-presets.txt`。Failure: Bash `./build/default/TagReaderRegressionTests --list >/tmp/opencode/old-regression-list.txt && rg -n "Catch2|catch_discover_tests|FetchContent|find_package\(Catch2" CMakeLists.txt test/CMakeLists.txt`，Evidence `.omo/evidence/task-2-catch2-ctest-presets-error.txt`。
  Commit: Y | test: integrate Catch2 and CTest discovery

- [x] 3. 迁移歌词规范化复杂度测试作为 Catch2 样板
  What to do / Must NOT do: 将 `test/regression/lyrics_normalize_complexity_tests.cpp` 的三个语义检查迁移为 Catch2 `TEST_CASE`，保留旧 executable 作为对照；使用 Catch2 assertions 替代本文件自定义 `Expect()`；CTest 必须可按测试名或 label 运行该专项测试。不得改变 `src/text/TextNormalize.cpp` 行为。
  Parallelization: Wave 2 | Blocked by: 2 | Blocks: 5,6,7,8,9
  References (executor has NO interview context - be exhaustive): `test/regression/lyrics_normalize_complexity_tests.cpp:17`, `test/regression/lyrics_normalize_complexity_tests.cpp:66`, `test/regression/lyrics_normalize_complexity_tests.cpp:99`, `test/regression/lyrics_normalize_complexity_tests.cpp:121`, `CMakeLists.txt:168`
  Acceptance criteria (agent-executable): 旧入口 `./build/default/TagReaderLyricsNormalizeComplexityTests` exit code 0；新 Catch2/CTest 入口 `ctest --test-dir build/default -R LyricsNormalize --output-on-failure` exit code 0；`ctest --test-dir build/default -N` 显示相关测试。
  QA scenarios (name the exact tool + invocation): Happy: Bash `./build/default/TagReaderLyricsNormalizeComplexityTests && ctest --test-dir build/default -R LyricsNormalize --output-on-failure`，Evidence `.omo/evidence/task-3-catch2-ctest-presets.txt`。Failure: Bash `ctest --test-dir build/default -R LyricsNormalize --output-on-failure --repeat until-fail:2`，Evidence `.omo/evidence/task-3-catch2-ctest-presets-error.txt`。
  Commit: Y | test: migrate lyrics normalization regression to Catch2

- [x] 4. 迁移 FLAC malformed 与默认封面目录专项测试
  What to do / Must NOT do: 将 `flac_malformed_metadata_tests.cpp` 和 `default_cover_export_directory_tests.cpp` 的场景迁移为 Catch2 tests；保留旧 executables 做并行对照；将 ffmpeg 缺失时的行为转为明确 skip 或 failure 规则，不能把未运行误报为通过；需要使用独立临时目录或 CTest properties 避免 cover cache 并发污染。不得提交生成音频样本。
  Parallelization: Wave 2 | Blocked by: 2 | Blocks: 5,6,7,8,9
  References (executor has NO interview context - be exhaustive): `test/regression/flac_malformed_metadata_tests.cpp:25`, `test/regression/flac_malformed_metadata_tests.cpp:159`, `test/regression/flac_malformed_metadata_tests.cpp:260`, `test/regression/default_cover_export_directory_tests.cpp:33`, `test/regression/default_cover_export_directory_tests.cpp:138`, `test/regression/default_cover_export_directory_tests.cpp:425`, `CMakeLists.txt:146`, `CMakeLists.txt:157`
  Acceptance criteria (agent-executable): 旧入口 `./build/default/TagReaderFlacMalformedMetadataTests` 和 `./build/default/TagReaderDefaultCoverExportDirectoryTests` exit code 0；新 CTest `ctest --test-dir build/default -R "FlacMalformed|DefaultCover" --output-on-failure` exit code 0；`git status --short` 不出现生成样本文件。
  QA scenarios (name the exact tool + invocation): Happy: Bash `./build/default/TagReaderFlacMalformedMetadataTests && ./build/default/TagReaderDefaultCoverExportDirectoryTests && ctest --test-dir build/default -R "FlacMalformed|DefaultCover" --output-on-failure`，Evidence `.omo/evidence/task-4-catch2-ctest-presets.txt`。Failure: Bash `git status --short | tee .omo/evidence/task-4-catch2-ctest-presets-error.txt && ! git status --short | rg "test/.+\.(flac|mp3|png|wav)$"`。
  Commit: Y | test: migrate specialty regression tests to Catch2

- [x] 5. 迁移 `TR-AUDIT-001..031` 到 Catch2 并保持旧 runner 对照
  What to do / Must NOT do: 把 `RunTrAudit001()` 到 `RunTrAudit031()` 迁移到 Catch2 测试文件或分组文件；每个测试名必须包含原始 `TR-AUDIT-xxx`；可复用旧 helper，但不得通过调用旧 runner binary 来伪造覆盖；旧 `TagReaderRegressionTests` 保留并作为对照。不得删除 `--list` runner 或 `RunCase()`。
  Parallelization: Wave 3 | Blocked by: 3,4 | Blocks: 8,9
  References (executor has NO interview context - be exhaustive): `test/regression/regression_tests.cpp:55`, `test/regression/regression_tests.cpp:119`, `test/regression/regression_tests.cpp:2497`, `test/regression/regression_tests.cpp:5298`, `test/regression/regression_tests.cpp:8321`
  Acceptance criteria (agent-executable): `./build/default/TagReaderRegressionTests --list` still lists `TR-AUDIT-001..031`；for each id `TR-AUDIT-001..031`, old runner and `ctest --test-dir build/default -R <id> --output-on-failure` both exit 0；`ctest --test-dir build/default -N` includes all `TR-AUDIT-001..031` ids.
  QA scenarios (name the exact tool + invocation): Happy: Bash loop `for n in $(seq -f "%03g" 1 31); do ./build/default/TagReaderRegressionTests TR-AUDIT-$n && ctest --test-dir build/default -R TR-AUDIT-$n --output-on-failure || exit 1; done`，Evidence `.omo/evidence/task-5-catch2-ctest-presets.txt`。Failure: Bash `ctest --test-dir build/default -N | tee /tmp/opencode/ctest-list.txt && for n in $(seq -f "%03g" 1 31); do rg "TR-AUDIT-$n" /tmp/opencode/ctest-list.txt >/dev/null || exit 1; done`，Evidence `.omo/evidence/task-5-catch2-ctest-presets-error.txt`。
  Commit: Y | test: migrate legacy audit regressions to Catch2

- [x] 6. 迁移 `TR-AUDIT-032..056` 到 Catch2 并保持新增格式覆盖
  What to do / Must NOT do: 把 `RunTrAudit032()` 到 `RunTrAudit056()` 迁移到 Catch2，保持 Ogg/Opus/RIFF/AIFF/DSD/ASF/Matroska/CUE guard 等新增格式测试语义；每个 CTest case 名必须含原 id；旧 runner 仍保留作为对照。不得降低任何资源上限、malformed、cover cache、API absence guard 的断言强度。
  Parallelization: Wave 3 | Blocked by: 3,4 | Blocks: 8,9
  References (executor has NO interview context - be exhaustive): `test/regression/regression_tests.cpp:5474`, `test/regression/regression_tests.cpp:5806`, `test/regression/regression_tests.cpp:6094`, `test/regression/regression_tests.cpp:6249`, `test/regression/regression_tests.cpp:6356`, `test/regression/regression_tests.cpp:7061`, `test/regression/regression_tests.cpp:7314`, `test/regression/regression_tests.cpp:7560`, `test/regression/regression_tests.cpp:7638`
  Acceptance criteria (agent-executable): For each id `TR-AUDIT-032..056`, old runner and `ctest --test-dir build/default -R <id> --output-on-failure` both exit 0；`ctest --test-dir build/default -N` includes all `TR-AUDIT-032..056` ids；forbidden API checks remain represented in `TR-AUDIT-056`.
  QA scenarios (name the exact tool + invocation): Happy: Bash loop `for n in $(seq -f "%03g" 32 56); do ./build/default/TagReaderRegressionTests TR-AUDIT-$n && ctest --test-dir build/default -R TR-AUDIT-$n --output-on-failure || exit 1; done`，Evidence `.omo/evidence/task-6-catch2-ctest-presets.txt`。Failure: Bash `ctest --test-dir build/default -R TR-AUDIT-056 --output-on-failure && ! rg -n "ReadCue|ReadAlbum|std::vector<MusicTag>" include src`，Evidence `.omo/evidence/task-6-catch2-ctest-presets-error.txt`。
  Commit: Y | test: migrate extended audit regressions to Catch2

- [x] 7. 将 security smoke、样本生成和 fuzz preset 纳入 CTest 工作流
  What to do / Must NOT do: 将 `test/security/generate_samples.py` 注册为 CTest setup 或显式 test；让 security smoke 通过 CTest 使用 generated sample directory 和 cover export directory；必要时保留 `TagReaderSecuritySmoke` CLI 但由 CTest 包装运行；将 fuzz corpus generation 和短 runs fuzz smoke 只挂到 `fuzz` preset 或 label，不进入 default `ctest`。使用 `RESOURCE_LOCK` 或独立目录避免 cover cache 并发污染。不得让缺失 ffmpeg/libFuzzer 的环境误报 full fuzz pass。
  Parallelization: Wave 4 | Blocked by: 3,4 | Blocks: 8,9
  References (executor has NO interview context - be exhaustive): `test/security/security_smoke.cpp:23`, `test/security/security_smoke.cpp:42`, `test/security/security_smoke.cpp:77`, `test/security/security_smoke.cpp:148`, `test/security/generate_samples.py:646`, `test/corpus/generate_corpus.py`, `test/fuzz/tagreader_fuzz.cpp`, `CMakeLists.txt:184`
  Acceptance criteria (agent-executable): `ctest --test-dir build/default -L security --output-on-failure` runs sample generation plus smoke and exits 0 or documents ffmpeg-dependent skips without hiding failures；`ctest --test-dir build/default -N` does not list unbounded fuzz run；`cmake --build --preset fuzz` builds fuzz target only when supported; if `TagReaderFuzz` exists, a bounded `-runs=100` corpus smoke passes.
  QA scenarios (name the exact tool + invocation): Happy: Bash `ctest --test-dir build/default -L security --output-on-failure`，Evidence `.omo/evidence/task-7-catch2-ctest-presets.txt`。Failure: Bash `ctest --test-dir build/default -N | tee /tmp/opencode/default-ctest-list.txt && ! rg "TagReaderFuzz|-runs=" /tmp/opencode/default-ctest-list.txt`，Evidence `.omo/evidence/task-7-catch2-ctest-presets-error.txt`。
  Commit: Y | test: register security smoke and fuzz presets

- [x] 8. 删除旧自建测试 runner 与根 CMake 测试堆叠
  What to do / Must NOT do: 在新 Catch2/CTest 全部通过后，删除旧的 `TagReaderRegressionTests` 自建 runner 入口和已经迁移的旧 specialty executable targets；整理 `test/regression/` 文件结构，避免同时维护重复测试；根 `CMakeLists.txt` 不再直接堆叠测试 executable，测试目标归入 `test/CMakeLists.txt`；保留 `TagReaderTest` manual CLI 和必要的 `TagReaderSecuritySmoke` CLI（若 CTest 仍包装使用）。不得删除新 Catch2 覆盖或 `TR-AUDIT-*` 可追踪名称。
  Parallelization: Wave 5 | Blocked by: 5,6,7 | Blocks: 9
  References (executor has NO interview context - be exhaustive): `CMakeLists.txt:103`, `CMakeLists.txt:119`, `CMakeLists.txt:130`, `CMakeLists.txt:146`, `CMakeLists.txt:157`, `CMakeLists.txt:168`, `test/main.cpp:50`, `test/regression/regression_tests.cpp:8321`
  Acceptance criteria (agent-executable): `cmake --preset default && cmake --build --preset default` exit code 0；`ctest --test-dir build/default --output-on-failure` exit code 0；`ctest --test-dir build/default -R TR-AUDIT-001 --output-on-failure` exit code 0；`! rg -n "TagReaderRegressionTests|TagReaderFlacMalformedMetadataTests|TagReaderDefaultCoverExportDirectoryTests|TagReaderLyricsNormalizeComplexityTests" CMakeLists.txt test/CMakeLists.txt` exit code 0 unless in deliberate compatibility documentation.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --preset default && cmake --build --preset default && ctest --test-dir build/default --output-on-failure`，Evidence `.omo/evidence/task-8-catch2-ctest-presets.txt`。Failure: Bash `ctest --test-dir build/default -N | tee /tmp/opencode/final-ctest-list.txt && for n in $(seq -f "%03g" 1 56); do rg "TR-AUDIT-$n" /tmp/opencode/final-ctest-list.txt >/dev/null || exit 1; done`，Evidence `.omo/evidence/task-8-catch2-ctest-presets-error.txt`。
  Commit: Y | test: remove legacy custom test runners

- [x] 9. 更新文档、AGENTS 指令与最终迁移验收
  What to do / Must NOT do: 更新 `AGENTS.md`、`docs/DESIGN.md`、`test/corpus/README.md`，把旧“没有单元测试框架/不要用 ctest”描述改为 Catch2/CTest/Presets 事实；记录新命令：`cmake --preset default`、`cmake --build --preset default`、`ctest --preset default`，以及 sanitize/fuzz preset；说明 clangd 使用 `build/default/compile_commands.json`；最终运行普通、sanitize、fuzz 可用性验证。不得声称存在 CI、lint 或 formatter，除非同时实现。
  Parallelization: Wave 5 | Blocked by: 8 | Blocks: Final verification
  References (executor has NO interview context - be exhaustive): `AGENTS.md:35`, `AGENTS.md:37`, `AGENTS.md:38`, `AGENTS.md:39`, `docs/DESIGN.md:152`, `docs/DESIGN.md:158`, `test/corpus/README.md`
  Acceptance criteria (agent-executable): `rg -n "Catch2|CTest|CMakePresets|cmake --preset default|ctest --preset default|build/default/compile_commands.json" AGENTS.md docs/DESIGN.md test/corpus/README.md .clangd CMakePresets.json` exit code 0；`! rg -n "没有.*单元测试框架|不要用 `ctest`|build-sanitize|build-fuzz" AGENTS.md docs/DESIGN.md test/corpus/README.md` exit code 0；`ctest --preset default --output-on-failure` exit code 0.
  QA scenarios (name the exact tool + invocation): Happy: Bash `ctest --preset default --output-on-failure && cmake --build --preset sanitize`，Evidence `.omo/evidence/task-9-catch2-ctest-presets.txt`。Failure: Bash `rg -n "Catch2|CTest|CMakePresets|build/default/compile_commands.json" AGENTS.md docs/DESIGN.md test/corpus/README.md .clangd CMakePresets.json && ! rg -n "没有.*单元测试框架|不要用 `ctest`|build-sanitize|build-fuzz" AGENTS.md docs/DESIGN.md test/corpus/README.md`，Evidence `.omo/evidence/task-9-catch2-ctest-presets-error.txt`。
  Commit: Y | docs: document Catch2 CTest preset workflow

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [x] F1. Plan compliance audit
- [x] F2. Code quality review
- [x] F3. Real manual QA
- [x] F4. Scope fidelity

F1 Plan compliance audit must verify every todo was completed in order, old runner deletion happened only after parity evidence, and all `TR-AUDIT-001..056` remain CTest-addressable. Evidence: `.omo/evidence/final-plan-compliance-catch2-ctest-presets.txt`.

F2 Code quality review must verify CMake structure is maintainable, Catch2 helpers are not over-coupled to old runner internals, generated sample paths remain repo-external, and no parser behavior was changed for tests. Evidence: `.omo/evidence/final-code-quality-catch2-ctest-presets.txt`.

F3 Real manual QA must run `cmake --preset default`, `cmake --build --preset default`, `ctest --preset default --output-on-failure`, `ctest --test-dir build/default -R TR-AUDIT-001 --output-on-failure`, `ctest --test-dir build/default -R TR-AUDIT-056 --output-on-failure`, `cmake --build --preset sanitize`, and fuzz preset build/smoke when supported. Evidence: `.omo/evidence/final-agent-qa-catch2-ctest-presets.txt`.

F4 Scope fidelity must verify no `TagReader::Read()` public API drift, no parser semantic changes unrelated to test migration, no generated samples/build outputs tracked, no root-level `build-sanitize`/`build-fuzz` recommendation remains, and clangd config points to the actual compile database. Evidence: `.omo/evidence/final-scope-fidelity-catch2-ctest-presets.txt`.

## Commit strategy
- 每个 todo 完成后提交一次，提交前必须运行该 todo 的 acceptance 和 QA 命令。
- Todo 0 必须先处理当前脏工作树，迁移提交不得混入旧 `.omo` 删除或无关改动；若用户希望保留 `.omo` 删除，应先单独提交 cleanup，再开始测试迁移提交。
- 不得把多个迁移阶段混在一个提交里；尤其是“新增 Catch2 覆盖”和“删除旧 runner”必须分开提交。
- 提交只包含当前 todo 相关源码、CMake、测试、文档和 `.omo/evidence`；不得提交 `build/`、生成样本、cover cache、fuzz corpus。
- 建议提交顺序：build/test presets → Catch2 skeleton → small specialty tests → specialty tests → TR-AUDIT 1-31 → TR-AUDIT 32-56 → security/fuzz CTest → remove legacy runners → docs.
- 不执行 `git push`；只有用户明确要求才提交。

## Success criteria
- 默认入口统一为 `cmake --preset default`、`cmake --build --preset default`、`ctest --preset default`。
- 所有构建产物位于根目录 `build/` 下的 preset 子目录；不再推荐或生成根级 `build-sanitize` / `build-fuzz`。
- clangd 能通过项目配置找到 `build/default/compile_commands.json`。
- Catch2/CTest 覆盖 `TR-AUDIT-001..056`，并支持 `ctest -R TR-AUDIT-xxx` 单案运行。
- Specialty regression、security smoke、generator smoke 和 fuzz opt-in 行为均被 CTest/presets 表达。
- 旧自建测试 runner 被删除或降级为必要 CLI wrapper，且不存在重复维护同一测试语义的旧/新两套代码。
- 文档与 AGENTS 说明和实际测试入口一致。
