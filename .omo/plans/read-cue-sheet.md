# read-cue-sheet - Work Plan

## TL;DR (For humans)
**What you'll get:** 新增显式 `ReadCueSheet` 能力：读取 `.cue` 后返回其中所有歌曲的 `MusicTag` 列表，并尽量补全标题、艺人、专辑、时长、偏移、媒体参数、歌词和封面。

**Why this approach:** CUE 是专辑级 sidecar，不适合塞进现有单文件 `Read()`；单独入口能支持批量 track，同时保持当前单曲读取行为稳定。

**What it will NOT do:** 不改变现有 `Read()` 返回值；不隐式扫描同目录 `.cue`；不支持目录递归、路径逃逸、FLAC 内嵌二进制 CUESHEET 或 `ReadAlbum`。

**Effort:** Large
**Risk:** Medium - 跨 public API、文本编码、路径安全、封面副作用和大量回归测试边界。
**Decisions to sanity-check:** `ReadCueSheet` 两个重载、返回 `std::vector<MusicTag>`、CUE 字段优先、单文件缺音频整体失败/多文件跳过、同目录外部封面 fallback。

Your next move: 可以直接 `$start-work` 执行，或先要求我运行一次高精度 Momus 计划审查。Full execution detail follows below.

---

> TL;DR (machine): Large / Medium risk; add explicit `ReadCueSheet` API with independent bounded CUE parser, audio metadata completion, sidecar cover fallback, Catch2/CTest coverage, and docs updates.

## Scope
### Must have
- Public API adds `TagReader::ReadCueSheet(const std::filesystem::path &cuePath)` returning `std::vector<MusicTag>`.
- Public API adds `TagReader::ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir)` returning `std::vector<MusicTag>`.
- Existing `TagReader::Read(path)` and `TagReader::Read(path, coverExportDir)` remain unchanged and return a single `MusicTag`.
- CUE implementation lives in a dedicated CUE module near existing format modules, e.g. `src/formats/cue/`, with parser/reader responsibilities separated enough for focused tests.
- CUE input is bounded before parsing: max file 4 MiB, max lines 10000, max tracks 99, max `FILE` references 256, max indexes per track 99, max text field 64 KiB.
- CUE bytes are decoded/sniffed to UTF-8 before parsing by reusing or extending existing text codec behavior; parser only consumes UTF-8 strings.
- Parser supports at minimum `FILE`, `TRACK`, `INDEX`, `TITLE`, `PERFORMER`, `SONGWRITER`, and selected `REM` keys (`GENRE`, `DATE`, `YEAR`, optionally `DISCNUMBER` if safely parseable).
- Parser safely ignores unsupported commands such as `CATALOG`, `CDTEXTFILE`, `FLAGS`, `ISRC`, `PREGAP`, `POSTGAP`, and unknown `REM` without corrupting following valid tracks.
- CUE `FILE` references resolve relative to the `.cue` file directory and reject empty paths, absolute paths, `..` escapes, symlink escapes, directories, non-regular files, and CUE self-reference as audio.
- Each returned `MusicTag` represents one CUE virtual track; `filePath` is the referenced audio file, `offset` is `INDEX 01`, `duration` is next track start minus current offset or audio total duration minus current offset for the final track.
- Field merge rule is CUE-over-audio: CUE fields win when both CUE and audio provide the same field; otherwise either side can fill gaps.
- Referenced audio files are read through existing single-file `ReadTag()` behavior to fill media info, lyrics, embedded cover, lastModified, format, and missing tag fields.
- Single-audio-file CUE fails the whole `ReadCueSheet()` call if the sole referenced audio is missing/unreadable/unopenable.
- Multi-audio-file CUE skips tracks for missing/unreadable/unopenable referenced audio, and returns remaining valid tracks; if no tracks remain, the whole CUE fails.
- External cover fallback runs only when the referenced audio produced no cover; it scans the audio file's own directory, not recursively, for regular image files with cover-like names in priority order `cover`, `front`, `folder`, `album`, `artwork`.
- External cover fallback converts image bytes to PNG and stores them through the same cover export/cache semantics as embedded cover handling; explicit `coverExportDir` must be honored and validated.
- Cover export/cache errors continue to propagate; malformed optional CUE fields are local failures that do not drop an otherwise valid track.
- `TR-AUDIT-056` is updated to allow `ReadCueSheet` but still forbid `Read()` returning `std::vector<MusicTag>` or exposing `ReadAlbum`.
- Catch2/CTest coverage is added for API shape, parser syntax, encoding, mapping, path safety, failure policy, cover fallback, and regression boundary.
- `CUE_HANDLING_RESEARCH.md`, `docs/DESIGN.md`, and `AGENTS.md` are updated from “CUE unsupported” to the exact new supported boundary.
### Must NOT have (guardrails, anti-slop, scope boundaries)
- Must not change public signatures or semantics of existing `Read()` overloads.
- Must not make `Read()` implicitly discover `.cue` files, parse sidecars, scan directories, or return multiple tracks.
- Must not add `ReadAlbum` in this implementation.
- Must not use FFmpeg `AVDictionary` as the source for CUE metadata.
- Must not support FLAC embedded binary `CUESHEET` metadata block as part of this `.cue` sidecar feature.
- Must not accept directory input or recursively scan a folder for CUE/audio/cover files.
- Must not allow CUE `FILE` references or external cover lookup to escape the cue/audio directory via absolute path, `..`, or symlink.
- Must not create a diagnostic result object or change the v1 return type unless the user revises the confirmed `std::vector<MusicTag>` decision.
- Must not commit generated audio samples, generated cover cache files, fuzz corpus, or build outputs.

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD + Catch2/CTest/CMakePresets. Each todo adds or updates tests with the implementation it requires; focused CTest gates run before broader default suite gates.
- Evidence: `.omo/evidence/task-<N>-read-cue-sheet.txt` for each todo, plus `.omo/evidence/final-*-read-cue-sheet.txt` for final review wave.
- Default build gate: `cmake --preset default && cmake --build --preset default`.
- Focused CUE gate: `ctest --test-dir build/default -R "Cue|TR-AUDIT-056" --output-on-failure`.
- Full gate: `ctest --preset default --output-on-failure`.
- Sanitizer gate near the end: `cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize --output-on-failure -R "Cue|TR-AUDIT-056"`.

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 0: dirty-worktree isolation and API/test boundary lock.
- Wave 1: public API skeleton, CMake target wiring, CUE parser data model and text decoding.
- Wave 2: path safety, time math, track mapping, and audio completion via `ReadTag()`.
- Wave 3: external cover fallback and failure policy.
- Wave 4: integration tests, audit migration, docs, sanitize/full verification.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 0 | none | 1,2,3,4,5,6,7,8,9,10,11 | none |
| 1 | 0 | 3,4,5,6,7,8,9,10,11 | 2 |
| 2 | 0 | 3,4,5,6,7,8,9,10,11 | 1 |
| 3 | 1,2 | 4,5,6,7,8,9,10,11 | none |
| 4 | 3 | 5,6,7,8,9,10,11 | none |
| 5 | 4 | 6,7,8,9,10,11 | none |
| 6 | 5 | 8,9,10,11 | 7 |
| 7 | 5 | 8,9,10,11 | 6 |
| 8 | 6,7 | 9,10,11 | none |
| 9 | 8 | 10,11 | none |
| 10 | 9 | 11 | none |
| 11 | 10 | Final verification | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 0. 隔离当前脏工作树并锁定执行范围
  What to do / Must NOT do: 在实现前记录 `git status --short`，确认当前未跟踪的 `.omo/run-continuation/*`、`.omo/drafts/read-cue-sheet.md`、`.omo/plans/read-cue-sheet.md` 与 `CUE_HANDLING_RESEARCH.md` 是既有规划/文档状态；后续产品代码提交不得混入无关 `.omo/run-continuation` 或生成产物。`.omo/drafts/read-cue-sheet.md`、`.omo/plans/read-cue-sheet.md`、`.omo/evidence/*read-cue-sheet*` 属于计划/执行证据允许范围，但是否提交由用户决定。若工作树还有用户未确认改动，执行者必须先记录排除策略。不得恢复、删除或覆盖用户文件。
  Parallelization: Wave 0 | Blocked by: none | Blocks: 1,2,3,4,5,6,7,8,9,10,11
  References (executor has NO interview context - be exhaustive): `CUE_HANDLING_RESEARCH.md:150`, `AGENTS.md:3`, `.omo/drafts/read-cue-sheet.md:1`, `.omo/plans/read-cue-sheet.md:1`, current `git status --short` showing `.omo/run-continuation/*`, `.omo/drafts/read-cue-sheet.md`, `.omo/plans/read-cue-sheet.md`, and `CUE_HANDLING_RESEARCH.md`.
  Acceptance criteria (agent-executable): `.omo/evidence/task-0-read-cue-sheet.txt` contains `git status --short`, the protected/unrelated path list, and the intended allowed path families (`include/`, `src/`, `test/`, `docs/`, `AGENTS.md`, `CUE_HANDLING_RESEARCH.md`, `CMakeLists.txt`, `test/CMakeLists.txt`, `.omo/drafts/read-cue-sheet.md`, `.omo/plans/read-cue-sheet.md`, `.omo/evidence`).
  QA scenarios (name the exact tool + invocation): Happy: Bash `git status --short | tee .omo/evidence/task-0-read-cue-sheet.txt` then append scope decision. Failure: Bash `git status --short --untracked-files=all | tee .omo/evidence/task-0-read-cue-sheet-error.txt`; if unrelated source/doc edits appear outside allowed families, stop before Todo 1.
  Commit: N | planning/worktree guard only

- [x] 1. 新增 `ReadCueSheet` public API 骨架与编译期边界测试
  What to do / Must NOT do: 在 `include/TagReader.hpp` 添加 `<vector>` include 和两个 `static std::vector<MusicTag> ReadCueSheet(...)` overload；在 `src/TagReader.cpp` 添加只转发到待建 core/cue reader 的薄 wrapper；先用 stub/empty implementation 让编译通过；更新 `TR-AUDIT-056` concept/static_assert，明确允许 `ReadCueSheet(path)` 与 `ReadCueSheet(path, coverExportDir)` 返回 `std::vector<MusicTag>`，继续断言两个 `Read()` overload 返回 `MusicTag` 且没有 `ReadAlbum`。不得改变 `Read()` 签名或让 `Read()` 调用 CUE 逻辑。
  Parallelization: Wave 1 | Blocked by: 0 | Blocks: 3,4,5,6,7,8,9,10,11 | Can parallelize with: 2
  References (executor has NO interview context - be exhaustive): `include/TagReader.hpp:10`, `src/TagReader.cpp:4`, `test/regression/regression_tests.cpp:145`, `test/regression/regression_tests.cpp:175`, `test/regression/regression_tests.cpp:7653`, `CUE_HANDLING_RESEARCH.md:77`, `CUE_HANDLING_RESEARCH.md:150`.
  Acceptance criteria (agent-executable): `cmake --build --preset default` succeeds; `ctest --test-dir build/default -R TR-AUDIT-056 --output-on-failure` succeeds and its output/evidence confirms `ReadCueSheet` exists, `Read()` is still scalar, `ReadAlbum` absent.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R TR-AUDIT-056 --output-on-failure | tee .omo/evidence/task-1-read-cue-sheet.txt`. Failure: Bash `rg -n "Read\(.*std::vector|ReadAlbum|ReadCue\(" include src test | tee .omo/evidence/task-1-read-cue-sheet-error.txt`; any changed `Read()` batch signature or `ReadAlbum` fails.
  Commit: Y | api(cue): add explicit ReadCueSheet entry points

- [x] 2. 注册 CUE 测试目标与共享样本工具
  What to do / Must NOT do: 在 `test/CMakeLists.txt` 新增 `TagReaderCueSheetCatch2Tests`，链接 `Catch2::Catch2WithMain` 和 `TagReaderCore`，注册 `catch_discover_tests()`；新增 CUE 测试 support 文件，提供临时目录、CUE 文本写入、1x1 图片写入、ffmpeg 音频样本生成/跳过策略、路径断言 helper。不得把生成音频/图片样本提交到仓库。
  Parallelization: Wave 1 | Blocked by: 0 | Blocks: 3,4,5,6,7,8,9,10,11 | Can parallelize with: 1
  References (executor has NO interview context - be exhaustive): `test/CMakeLists.txt:1`, `test/CMakeLists.txt:19`, `test/regression/catch2_regression_support.cpp`, `test/regression/catch2_sample_support.cpp`, `test/security/generate_samples.py:123`, `test/regression/flac_malformed_metadata_support.cpp:33`.
  Acceptance criteria (agent-executable): `ctest --test-dir build/default -N` lists CUE tests; new support writes all generated files under temp/build dirs, not `test/`; `git status --short` shows no generated audio/image binaries.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -N | tee .omo/evidence/task-2-read-cue-sheet.txt && ! git status --short | rg "test/.+\.(wav|flac|mp3|png|jpg|jpeg|webp|gif|bmp|tiff)$"`. Failure: Bash `git status --short --untracked-files=all | tee .omo/evidence/task-2-read-cue-sheet-error.txt`; fail if generated binary samples appear under repo paths.
  Commit: Y | test(cue): add Catch2 CUE test harness

- [x] 3. 实现 CUE 文本读取、编码嗅探与 UTF-8 输入层
  What to do / Must NOT do: 新增 CUE module 的文本加载层：校验 `.cue` 路径为普通文件、大小不超过 4 MiB；读取原始字节；复用/扩展 `tagreader_text::DecodeRawText()` / `DetectTextEncoding()` / `DecodeTextToUtf8()` 以支持 UTF-8、UTF-8 BOM、UTF-16LE/BE BOM/heuristic、Latin-1/locale fallback；返回 parser 只消费的 UTF-8 字符串。不得让非 UTF-8 字符串进入 `MusicTag`，不得使用 FFmpeg metadata dictionary。
  Parallelization: Wave 1 | Blocked by: 1,2 | Blocks: 4,5,6,7,8,9,10,11
  References (executor has NO interview context - be exhaustive): `src/text/TextCodec.cpp:610`, `src/text/TextCodec.cpp:652`, `src/text/TextCodec.cpp:717`, `src/io/ByteReader.cpp:14`, `CUE_HANDLING_RESEARCH.md:112`, `CUE_HANDLING_RESEARCH.md:150`.
  Acceptance criteria (agent-executable): Catch2 tests pass for UTF-8, UTF-8 BOM, UTF-16LE/BE, Latin-1/fallback CUE text; oversized CUE fails; invalid undecodable CUE fails before parser; all decoded metadata fields are valid UTF-8.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*Encoding" --output-on-failure | tee .omo/evidence/task-3-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*InvalidEncoding|Cue.*Oversize" --output-on-failure | tee .omo/evidence/task-3-read-cue-sheet-error.txt` confirms invalid/oversized inputs fail as expected, not crash.
  Commit: Y | feat(cue): decode cue sheets to utf8

- [x] 4. 实现 bounded CUE parser 与私有中间态
  What to do / Must NOT do: 解析 UTF-8 CUE 文本为私有结构：global fields、file blocks、track blocks、indexes、selected `REM` fields、unsupported-command diagnostics/ignored state。支持命令大小写不敏感、缩进、空行、quoted/unquoted 参数、`FILE "name" WAVE/MP3/...`、`TRACK NN AUDIO`、`INDEX NN mm:ss:ff`。资源限制：10000 lines、99 tracks、256 FILE refs、99 indexes/track、64 KiB field。不得把 CUE 中间态暴露到 public API。
  Parallelization: Wave 2 | Blocked by: 3 | Blocks: 5,6,7,8,9,10,11
  References (executor has NO interview context - be exhaustive): `CUE_HANDLING_RESEARCH.md:28`, `CUE_HANDLING_RESEARCH.md:38`, `CUE_HANDLING_RESEARCH.md:46`, `CUE_HANDLING_RESEARCH.md:120`, `CUE_HANDLING_RESEARCH.md:162`.
  Acceptance criteria (agent-executable): Catch2 parser tests cover basic single-file CUE, multi-file CUE, quoted filenames/titles, lowercase/mixedcase commands, unknown commands ignored, `REM GENRE/DATE/YEAR/DISCNUMBER`, too many tracks/files/indexes/lines rejected, invalid `INDEX` frame `75` rejected, overflow values rejected.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*Parser" --output-on-failure | tee .omo/evidence/task-4-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*Limit|Cue.*Overflow|Cue.*InvalidIndex" --output-on-failure | tee .omo/evidence/task-4-read-cue-sheet-error.txt` confirms invalid cases are rejected without crash.
  Commit: Y | feat(cue): parse cue sheet commands safely

- [x] 5. 实现 CUE `FILE` 路径解析与音频引用安全策略
  What to do / Must NOT do: 将 CUE `FILE` 引用解析为 `.cue` 所在目录下的真实普通音频文件；拒绝 empty path、absolute path、`..` escape、symlink escape、directory、non-regular file、cue file itself as audio；支持多文件 CUE 的 file-to-track association (`FILE` in global/track context affects following tracks per CUE behavior)。不得递归扫描目录或跟随不安全 symlink。
  Parallelization: Wave 2 | Blocked by: 4 | Blocks: 6,7,8,9,10,11
  References (executor has NO interview context - be exhaustive): `CUE_HANDLING_RESEARCH.md:54`, `CUE_HANDLING_RESEARCH.md:105`, `CUE_HANDLING_RESEARCH.md:126`, `CUE_HANDLING_RESEARCH.md:150`, `src/core/TagPipeline.cpp:596`.
  Acceptance criteria (agent-executable): Catch2 tests cover safe relative paths, spaces/quotes, multi-file association, absolute path rejection, `..` rejection, symlink escape rejection, directory rejection, missing file classification, self-reference rejection.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*Path" --output-on-failure | tee .omo/evidence/task-5-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*PathEscape|Cue.*Symlink|Cue.*Absolute|Cue.*Directory" --output-on-failure | tee .omo/evidence/task-5-read-cue-sheet-error.txt` confirms unsafe paths fail/skip per policy.
  Commit: Y | feat(cue): resolve cue file references safely

- [x] 6. 映射 CUE track 为 `MusicTag` 并复用 `ReadTag()` 补全音频字段
  What to do / Must NOT do: 实现 `ReadCueSheet` core reader：对每个有效 track 调用现有 `tagreader_core::ReadTag(audioPath, coverExportDir)` 获取音频字段；用 CUE-over-audio 规则合并字段；track `TITLE/PERFORMER/SONGWRITER/TRACK` 覆盖 `title/artist/composer/trackNumber`；global `TITLE/PERFORMER/SONGWRITER/REM GENRE/DATE/YEAR/DISCNUMBER` 填充 `album/albumArtist/composer/genre/year/discNumber`；保持 audio-provided sampleRate/bitDepth/bitRate/channels/format/lastModified/lyrics/coverPath/playCount/rating unless CUE has a more specific mapped field. 不得复制 `ReadTag()` 主流程，也不得改变 `BuildMusicTag()` 给普通 audio 的行为。
  Parallelization: Wave 2 | Blocked by: 5 | Blocks: 8,9,10,11 | Can parallelize with: 7 after path/parser basics
  References (executor has NO interview context - be exhaustive): `src/core/TagPipeline.cpp:592`, `src/core/TagPipeline.cpp:532`, `include/Tag.hpp:11`, `CUE_HANDLING_RESEARCH.md:79`, `CUE_HANDLING_RESEARCH.md:101`, `CUE_HANDLING_RESEARCH.md:150`.
  Acceptance criteria (agent-executable): Catch2 integration tests produce `std::vector<MusicTag>` with correct size, CUE title/artist overriding audio title/artist, audio media parameters populated, lyrics preserved when audio has lyrics, `filePath` points to referenced audio, playCount/rating remain current defaults.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*Mapping|Cue.*AudioFill" --output-on-failure | tee .omo/evidence/task-6-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*CueOverridesAudio|Cue.*ReadUnchanged" --output-on-failure | tee .omo/evidence/task-6-read-cue-sheet-error.txt` confirms CUE precedence and existing `Read()` behavior.
  Commit: Y | feat(cue): map cue tracks to music tags

- [x] 7. 实现 CUE offset/duration 时间推导
  What to do / Must NOT do: 将 `mm:ss:ff` 转为微秒，严格使用 75 frames/sec；每个 track `offset = INDEX 01`；同一音频文件中非最后 track duration = 下一 track `INDEX 01` - 当前 `INDEX 01`；最后 track duration = audio total duration - current offset；多文件 CUE 中每个文件单独计算；若 duration 无法推导但 track 其余有效，按 adopted default 保留 duration 0。拒绝负 duration、同一文件时间倒退、非法 frame/second/minute 或整数溢出。
  Parallelization: Wave 2 | Blocked by: 5 | Blocks: 8,9,10,11 | Can parallelize with: 6 after path/parser basics
  References (executor has NO interview context - be exhaustive): `src/media/MediaInfoReader.cpp:197`, `include/Tag.hpp:108`, `include/Tag.hpp:113`, `CUE_HANDLING_RESEARCH.md:38`, `CUE_HANDLING_RESEARCH.md:79`, `CUE_HANDLING_RESEARCH.md:130`.
  Acceptance criteria (agent-executable): Catch2 tests verify microsecond conversion for `00:00:00`, `00:00:74`, `00:01:00`, multiple tracks in one file, final track duration from audio total, multi-file durations independent, invalid/time-backward cases rejected.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*Time|Cue.*Duration" --output-on-failure | tee .omo/evidence/task-7-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*NegativeDuration|Cue.*TimeBackward|Cue.*InvalidFrame" --output-on-failure | tee .omo/evidence/task-7-read-cue-sheet-error.txt` confirms invalid timings reject safely.
  Commit: Y | feat(cue): derive cue track timing

- [x] 8. 实现外部封面 fallback 与 cover export 重载语义
  What to do / Must NOT do: 当 audio `ReadTag()` 返回空 `coverPath` 时，在该音频文件同目录查找 regular image candidates；排序先按名字关键词 priority `cover`, `front`, `folder`, `album`, `artwork`，再稳定按 filename；扩展名集合至少覆盖现有 decoder 支持的 `.png`, `.jpg`, `.jpeg`, `.bmp`, `.webp`, `.gif`, `.tif`, `.tiff`，大小写不敏感；候选数量限制 256；读取文件大小遵循 64 MiB cap；用现有 cover decoder 转 PNG，用现有 content-addressed cache/export dir 写入；显式 `coverExportDir` 重载必须使用调用者目录并拒绝 symlink/invalid dir；默认重载使用当前默认 cover dir hardening。不得在有 embedded cover 时覆盖它，不得递归查找。
  Parallelization: Wave 3 | Blocked by: 6,7 | Blocks: 9,10,11
  References (executor has NO interview context - be exhaustive): `src/cover/CoverDecoder.cpp:268`, `src/cover/CoverCache.cpp:42`, `src/core/TagPipeline.cpp:85`, `src/core/TagPipeline.cpp:154`, `src/core/TagPipeline.cpp:592`, `CUE_HANDLING_RESEARCH.md:103`, `CUE_HANDLING_RESEARCH.md:150`.
  Acceptance criteria (agent-executable): Catch2 tests verify embedded cover wins; fallback finds `cover.*` before `front.*` before `folder.*` etc.; explicit `coverExportDir` is used; fallback image becomes PNG cache path; bad image is ignored/next candidate tried; symlink cover candidate/escape rejected; no recursive discovery.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*Cover" --output-on-failure | tee .omo/evidence/task-8-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*CoverSymlink|Cue.*CoverPriority|Cue.*BadCover" --output-on-failure | tee .omo/evidence/task-8-read-cue-sheet-error.txt` confirms safe fallback semantics.
  Commit: Y | feat(cue): add sidecar cover fallback

- [x] 9. 实现单文件/多文件 CUE 失败策略与 partial return 规则
  What to do / Must NOT do: Enforce confirmed policy: if a CUE has exactly one distinct audio `FILE`, missing/unreadable/unopenable audio makes `ReadCueSheet()` fail; if a CUE has multiple distinct audio files, missing/unreadable/unopenable file skips tracks tied to that file; if no valid tracks remain, fail; malformed optional track fields are local field drops. Cover export/cache errors still propagate. Must not silently generate tags for missing files with only CUE fields.
  Parallelization: Wave 3 | Blocked by: 8 | Blocks: 10,11
  References (executor has NO interview context - be exhaustive): `CUE_HANDLING_RESEARCH.md:105`, `CUE_HANDLING_RESEARCH.md:150`, `src/core/TagPipeline.cpp:360`, `src/core/TagPipeline.cpp:592`, `test/CMakeLists.txt:136`.
  Acceptance criteria (agent-executable): Catch2 tests cover single-file missing audio throws/fails; multi-file missing one file returns remaining tracks only; all files missing fails; unreadable/invalid audio follows same policy; cover cache error still propagates.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --build --preset default && ctest --test-dir build/default -R "Cue.*FailurePolicy|Cue.*MissingAudio" --output-on-failure | tee .omo/evidence/task-9-read-cue-sheet.txt`. Failure: Bash `ctest --test-dir build/default -R "Cue.*AllMissing|Cue.*CoverCacheError" --output-on-failure | tee .omo/evidence/task-9-read-cue-sheet-error.txt` confirms failure paths are intentional.
  Commit: Y | feat(cue): enforce cue failure policy

- [x] 10. 更新审计、文档与当前能力说明
  What to do / Must NOT do: 更新 `TR-AUDIT-056` 输出 marker/summary，更新 Catch2 wrapper expectations if needed；更新 `docs/DESIGN.md`、`AGENTS.md`、`CUE_HANDLING_RESEARCH.md`，把 CUE 从未实现改为“支持 `.cue` sidecar via `ReadCueSheet`”，同时仍说明 FLAC binary `CUESHEET`、directory album scan、`ReadAlbum` 不支持。文档必须包含 build/test commands and CUE targeted CTest commands。不得写成 `Read()` 自动处理 CUE。
  Parallelization: Wave 4 | Blocked by: 9 | Blocks: 11
  References (executor has NO interview context - be exhaustive): `docs/DESIGN.md:58`, `AGENTS.md:19`, `AGENTS.md:35`, `CUE_HANDLING_RESEARCH.md:150`, `test/regression/regression_tests.cpp:7660`, `test/regression/tr_audit_032_056_catch2_tests.cpp`.
  Acceptance criteria (agent-executable): `rg -n "ReadCueSheet|CUE|ctest.*Cue|TR-AUDIT-056" AGENTS.md docs/DESIGN.md CUE_HANDLING_RESEARCH.md test/regression` finds updated facts; `! rg -n "CUE.*未实现|不支持 CUE 读取入口|Read\(\).*CUE" AGENTS.md docs/DESIGN.md CUE_HANDLING_RESEARCH.md` except historical/current-fact context that is explicitly removed or reworded.
  QA scenarios (name the exact tool + invocation): Happy: Bash `ctest --test-dir build/default -R TR-AUDIT-056 --output-on-failure && rg -n "ReadCueSheet|CUE" AGENTS.md docs/DESIGN.md CUE_HANDLING_RESEARCH.md | tee .omo/evidence/task-10-read-cue-sheet.txt`. Failure: Bash `rg -n "CUE.*未实现|不支持 CUE 读取入口|Read\(\).*隐式.*CUE" AGENTS.md docs/DESIGN.md CUE_HANDLING_RESEARCH.md | tee .omo/evidence/task-10-read-cue-sheet-error.txt`; fail on stale claims not explicitly marked out-of-scope.
  Commit: Y | docs(cue): document ReadCueSheet support

- [x] 11. 运行完整默认与 sanitizer 验证并清理产物
  What to do / Must NOT do: Run configure/build/test gates for default and focused sanitizer CUE tests; inspect git status for generated artifacts; ensure no generated samples/covers are tracked. If fuzz preset unsupported, record as unrelated/not required; do not make fuzz pass mandatory for CUE feature. Do not fix unrelated test failures except those caused by this work.
  Parallelization: Wave 4 | Blocked by: 10 | Blocks: Final verification
  References (executor has NO interview context - be exhaustive): `CMakePresets.json:5`, `CMakePresets.json:14`, `AGENTS.md:35`, `AGENTS.md:36`, `test/CMakeLists.txt:1`.
  Acceptance criteria (agent-executable): `cmake --preset default` succeeds; `cmake --build --preset default` succeeds; `ctest --preset default --output-on-failure` succeeds; `ctest --test-dir build/default -R "Cue|TR-AUDIT-056" --output-on-failure` succeeds; `cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize --output-on-failure -R "Cue|TR-AUDIT-056"` succeeds; `git status --short` shows no generated build/sample/cache files.
  QA scenarios (name the exact tool + invocation): Happy: Bash `cmake --preset default && cmake --build --preset default && ctest --preset default --output-on-failure && ctest --test-dir build/default -R "Cue|TR-AUDIT-056" --output-on-failure && cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize --output-on-failure -R "Cue|TR-AUDIT-056" | tee .omo/evidence/task-11-read-cue-sheet.txt`. Failure: Bash `git status --short --untracked-files=all | tee .omo/evidence/task-11-read-cue-sheet-error.txt && ! git status --short --untracked-files=all | rg "(build/|test/.+\.(wav|flac|mp3|png|jpg|jpeg|webp|gif|bmp|tiff)|tagreader-covers)"` confirms no generated artifacts are tracked.
  Commit: Y | test(cue): verify ReadCueSheet workflow

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [x] F1. Plan compliance audit
- [x] F2. Code quality review
- [x] F3. Real manual QA
- [x] F4. Scope fidelity

F1 Plan compliance audit must verify all Must-have items were implemented, every todo has evidence, `ReadCueSheet` API and tests exist, `Read()` remains unchanged, and docs match the final behavior. Required invocation: spawn `task(subagent_type="oracle", prompt="TASK: Audit .omo/plans/read-cue-sheet.md compliance against the final working tree. DELIVERABLE: APPROVE/REJECT with evidence for every Must-have, every todo evidence file, ReadCueSheet API shape, unchanged Read() semantics, and docs alignment. VERIFY: cite exact files and commands inspected; do not edit files.")`, then write the result to `.omo/evidence/final-plan-compliance-read-cue-sheet.txt`. Pass condition: verdict contains `APPROVE` and no blocking missing todo/evidence/scope item.

F2 Code quality review must verify the CUE module is bounded, maintainable, not over-coupled to test helpers, uses C++23 idioms, avoids duplicated TagPipeline logic, and has no broad parser side effects. Required invocation: spawn `task(subagent_type="oracle", prompt="TASK: Review CUE implementation code quality. DELIVERABLE: APPROVE/REJECT with findings on parser bounds, C++23 style, ownership/error handling, test-helper coupling, duplicated TagPipeline logic, and broad side effects. VERIFY: inspect include/src/test diffs and cite files; do not edit files.")`, then write the result to `.omo/evidence/final-code-quality-read-cue-sheet.txt`. Pass condition: verdict contains `APPROVE` and no high-confidence correctness/maintainability blocker.

F3 Real manual QA must independently run default build/full CTest, focused CUE/`TR-AUDIT-056` CTest, sanitizer focused CUE/`TR-AUDIT-056`, and inspect representative generated cover cache behavior. Required invocation: Bash `cmake --preset default && cmake --build --preset default && ctest --preset default --output-on-failure && ctest --test-dir build/default -R "Cue|TR-AUDIT-056" --output-on-failure && cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize --output-on-failure -R "Cue|TR-AUDIT-056" | tee .omo/evidence/final-agent-qa-read-cue-sheet.txt`. Then append `git status --short --untracked-files=all` and `ctest --test-dir build/default -R "Cue.*Cover" --output-on-failure` output to the same file. Pass condition: all commands exit 0 and no generated build/sample/cache files are tracked.

F4 Scope fidelity must verify no `Read()` batch/sidecar behavior, no `ReadAlbum`, no directory scan, no FLAC binary CUESHEET claim, no FFmpeg dictionary metadata dependency, no path/symlink escape, and no generated artifacts in git. Required invocation: Bash `{ rg -n "ReadCueSheet|ReadAlbum|std::vector<MusicTag>|AVDictionary|CUESHEET|recursive|directory" include src test docs AGENTS.md CUE_HANDLING_RESEARCH.md; git status --short --untracked-files=all; ctest --test-dir build/default -R "TR-AUDIT-056|Cue.*Path|Cue.*Symlink|Cue.*ReadUnchanged" --output-on-failure; } | tee .omo/evidence/final-scope-fidelity-read-cue-sheet.txt`. Pass condition: inspection confirms `ReadAlbum` absent, `Read()` not batch/sidecar, CUE docs do not claim FLAC binary CUESHEET support, CUE metadata does not use `AVDictionary`, path/symlink tests pass, and no generated artifacts are tracked.

## Commit strategy
- Commit after each todo that changes product/test/docs code, once that todo's acceptance and QA commands pass.
- Use atomic commits in this suggested order:
  1. `api(cue): add explicit ReadCueSheet entry points`
  2. `test(cue): add Catch2 CUE test harness`
  3. `feat(cue): decode cue sheets to utf8`
  4. `feat(cue): parse cue sheet commands safely`
  5. `feat(cue): resolve cue file references safely`
  6. `feat(cue): map cue tracks to music tags`
  7. `feat(cue): derive cue track timing`
  8. `feat(cue): add sidecar cover fallback`
  9. `feat(cue): enforce cue failure policy`
  10. `docs(cue): document ReadCueSheet support`
  11. `test(cue): verify ReadCueSheet workflow`
- Do not commit `.omo/run-continuation/*`, `build/`, generated samples, generated cover caches, fuzz corpus, or unrelated dirty worktree files.
- Do not `git push`; only commit if explicitly requested by the user during execution.

## Success criteria
- `TagReader::ReadCueSheet(cuePath)` and `TagReader::ReadCueSheet(cuePath, coverExportDir)` compile and return `std::vector<MusicTag>`.
- Existing `TagReader::Read()` overloads still compile, still return `MusicTag`, and do not parse `.cue` sidecars.
- CUE parser handles UTF-8/BOM/fallback-decoded text, bounded commands, selected metadata, safe relative FILE references, and invalid input rejection.
- CUE output fills each `MusicTag` as fully as possible from CUE + referenced audio, with CUE fields taking precedence.
- Offsets and durations are correct for single-file and multi-file CUE sheets, including final-track duration from audio total when available.
- Single-file missing audio fails the whole CUE; multi-file missing audio skips affected tracks and returns remaining valid tracks.
- External cover fallback uses same-directory cover-like image files only when embedded cover is absent, converts to PNG, and uses existing cache/export semantics.
- `TR-AUDIT-056` verifies the new API boundary: `ReadCueSheet` exists, `Read()` remains scalar, `ReadAlbum` remains absent.
- `ctest --preset default --output-on-failure` passes.
- Focused `ctest -R "Cue|TR-AUDIT-056"` passes in default and sanitize presets.
- Docs and agent notes accurately describe CUE support and still distinguish unsupported FLAC binary CUESHEET/directory album scan.
