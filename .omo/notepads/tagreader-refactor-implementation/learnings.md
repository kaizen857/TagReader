
## 2026-05-31 Task 1 baseline

- `cmake -S . -B build` 和 `cmake --build build` 在当前仓库通过，构建出 `TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke`。
- 仓库内没有真实 `.mp3/.flac/.ogg/.m4a/.mp4` 样本；已用 `python3 test/corpus/generate_corpus.py` 生成 `/tmp/opencode/tagreader_fuzz_corpus` 作为 fallback。
- `TagReaderTest` 对 73 个 generated seed 全量尝试后，只有 8 个 FLAC seed 返回 0 并保存 stdout；完整记录在 `.omo/evidence/baseline/tagreadertest-runs.json`。

## 2026-05-31 Task 2 skeleton build

- 已新增 `src/core`、`src/media`、`src/io`、`src/text`、`src/cover`、`src/formats/id3`、`src/formats/vorbis`、`src/formats/flac`、`src/formats/ogg-vorbis`、`src/formats/mp4` 的最小目录/源码骨架，只接入 `TagReaderCore` 构建，不移动 `src/TagReader.cpp` 中的解析算法。
- `CMakeLists.txt` 已把内部 `src` 目录加入 `TagReaderCore` 的 `PRIVATE` include path，同时保留 `include/` 的 public surface 不变。
- `cmake -S . -B build`、`cmake --build build` 均通过；`./build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_valid_chain.flac` 的 stdout 与 `.omo/evidence/baseline/flac_flac_valid_chain.flac.stdout` 完全一致。

## 2026-05-31 Task 3 core contracts

- `include/TagReader.hpp` 已缩减为 public facade：只包含 `Tag.hpp`、`<filesystem>` 和两个 `TagReader::Read` 重载，`ReadContext`/raw 数据/ID3 视图等内部类型不再暴露在 public header。
- 新增 `src/core/ReadContext.hpp`、`src/core/RawTagData.hpp`、`src/core/TagFormat.hpp`；字段、默认值和所有权语义按原 private nested struct 原样迁移，`DetectedContainer` 继续作为内部容器枚举。
- 为避免重新搬迁解析算法，`src/TagReader.cpp` 只把原 private static helper 改为同一翻译单元内的内部函数，并通过 private include 使用 `tagreader_core` 契约；`TagReader::Read` public 入口保持原签名和调用流程。
- `cmake -S . -B build`、`cmake --build build` 通过；指定 FLAC seed 输出与 `.omo/evidence/baseline/flac_flac_valid_chain.flac.stdout` 完全一致。

## 2026-05-31 Task 3 review fix

- 从 public header 抽离 private helper 时，不能只把 `TagReader::` 成员函数改成自由函数；所有非 public helper 的声明和定义都要放在匿名 namespace 中，否则会意外获得 external linkage。
- `src/TagReader.cpp` 目前保持 `TagReader::Read` 两个 public 定义在全局作用域，`tagreader_core::ReadContext::FormatContextDeleter::operator()` 用限定名定义，其余 helper 声明/定义分别位于匿名 namespace 块中。

## 2026-05-31 Task 4 ByteReader/Text extraction

- `src/io/ByteReader.*` 现在承载跨 ID3、FLAC、Ogg、MP4 分支共用的 `ReadRange`、大小端读取、syncsafe 校验/读取、safe add 和 `ByteCursor`；`TagReader.cpp` 通过内部 namespace using 使用它们，没有新增 public `include/` API。
- `src/text/TextCodec.*` 现在承载原 UTF-8/UTF-16/Latin-1/iconv 兼容解码、编码探测、trim 和 ID3 byte-string decode；`src/text/TextNormalize.*` 承载 `NormalizeMetadata`、`NormalizeLyrics`、`ReadLyricsFromPlainText` 及其 LRC 直接依赖。
- CMake 已把 `src/io/ByteReader.cpp`、`src/text/TextCodec.cpp`、`src/text/TextNormalize.cpp` 加入 `TagReaderCore`，同时保留既有目标名与解析分支位置不变。
- 8 个 runnable FLAC baseline stdout 重跑后全部与 `.omo/evidence/baseline/*.stdout` 完全一致；详细结果在 `.omo/evidence/task-4-text-baseline-diff.txt`。

## 2026-05-31 Task 5 CoverCache/CoverDecoder extraction

- `src/cover/CoverCache.*` 现在承载封面 hash、content-addressed shard 路径、existing cache validation、atomic temp/write/link/fsync/publish 与 `WriteCoverAsPng` 出口；`src/TagReader.cpp` 只通过 `tagreader_cover::WriteCoverAsPng` 调用，不改变 ID3/FLAC/MP4 解析分支。
- `src/cover/CoverDecoder.*` 现在承载原 FFmpeg image format sniff、decode、RGB24 conversion 和 PNG encode 逻辑；资源限制继续复用 `tagreader_internal::CoverDecodeLimits` 的既有数值。
- `TagReaderSecuritySmoke` 对 `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac` 通过，polluted cache diagnostic 仍包含 `cover cache` 和路径；证据在 `.omo/evidence/task-5-cover-security.txt`。
- 不传 cover export dir 运行 cover-capable FLAC seed 时，`coverPath` 为空且 `/tmp/opencode/tagreader_no_cover_out` 未创建；证据在 `.omo/evidence/task-5-no-cover-side-effect.txt`。

## 2026-05-31 Task 6 ID3 migration repair

- ID3 parser entrypoints now live in `src/formats/id3/Id3Parser.*`; ID3 frame walking, unsync handling, genre mapping, APIC/PIC, and ID3 lyrics frame parsing live in `src/formats/id3/Id3Frames.*`.
- Recovery required preserving Tasks 1-5 uncommitted modules; `git HEAD` was pre-refactor and unsuitable as a wholesale restore source. Future migrations should avoid script-based slicing unless function-definition boundaries are verified first.
- `Id3Frames.cpp` should call `tagreader_io`, `tagreader_text`, and `tagreader_cover` APIs rather than copying TextCodec/Iconv or cover-cache internals.

## 2026-05-31 Task 7 Vorbis comment parser

- `src/formats/vorbis/VorbisCommentParser.*` now owns only Vorbis comment key/value entry semantics and lyrics entry semantics; FLAC metadata-block walking and Ogg page/packet scanning remain in `src/TagReader.cpp`.
- `CMakeLists.txt` must compile `src/formats/vorbis/VorbisCommentParser.cpp`; the empty `VorbisMetadata.cpp` stub can remain present but should not be mistaken for the migrated parser implementation.
- The three generated invalid Vorbis FLAC seeds confirm malformed key/value or lyrics entries remain isolated and later valid entries still populate metadata.

## 2026-05-31 Task 8 FLAC parser

- `src/formats/flac/FlacParser.*` now owns FLAC signature validation, metadata block walking, Vorbis comment block extraction, PICTURE parsing/export, and FLAC-specific lyrics block scanning.
- FLAC parsing should keep using `tagreader_vorbis::ReadVorbisCommentEntry` / `ReadVorbisLyricsEntry` for entry semantics and `tagreader_cover::WriteCoverAsPng` for cover export; do not duplicate text/cover internals in the FLAC module.
- `src/TagReader.cpp` should keep only FLAC container detection and dispatch to `tagreader_flac`, while Ogg page/packet scanning remains in `TagReader.cpp` until its own migration task.

## 2026-05-31 Task 9 Ogg Vorbis parser

- `src/formats/ogg-vorbis/OggVorbisParser.*` now owns Ogg page/packet scanning, stream serial/sequence/continuation state, lacing payload assembly, resource limits, and Vorbis comment packet extraction.
- Ogg metadata and lyrics entry semantics should continue to route through `tagreader_vorbis::ReadVorbisCommentEntry` / `ReadVorbisLyricsEntry`; the Ogg module should stay focused on Ogg container packet discovery rather than generic Vorbis key/value rules.
- `src/TagReader.cpp` now dispatches Ogg metadata/lyrics to `tagreader_ogg_vorbis` and should not regain scanner constants or `ReadOggVorbisCommentEntries` logic in later migrations.

## 2026-05-31 Task 10 MP4 parser

- `src/formats/mp4/Mp4AtomReader.*` now owns MP4 atom headers, path state, ilst traversal, size-zero sibling recovery, visited atom limits, payload reads, and MP4-only atom state structs that were removed from `include/TagReaderInternal.hpp`.
- `src/formats/mp4/Mp4Parser.*` now owns MP4 ilst metadata and lyrics semantics, including text `data` atoms, `covr`, `trkn`/`disk`, `©lyr`, and iTunes freeform lyrics; `src/TagReader.cpp` only dispatches MP4 metadata/lyrics to `tagreader_mp4`.
- Baseline stdout comparison must reuse the baseline cover directory when comparing cover-capable FLAC seeds, because cover cache paths are part of stdout and a task-local cover directory creates a false mismatch.

## 2026-05-31 Task 11 TagPipeline

- `src/core/TagPipeline.*` now owns public-entry orchestration: path validation, optional cover export dir validation, FFmpeg context open/stream detect, raw-byte-first tag format detection, media read, metadata/lyrics dispatch, normalization, and final `MusicTag` assembly.
- `src/TagReader.cpp` is now only the public facade with the two existing `TagReader::Read` overloads delegating to `tagreader_core::ReadTag`; public header surface remains unchanged.
- Baseline comparison scripts should execute JSON commands via argument vectors (`shlex.split`) instead of `shell=True`, because this workspace path contains parentheses and shell parsing can produce false failures before `TagReaderTest` runs.

## 2026-05-31 Task 12 CMake/include cleanup

- `TagReaderCore` now lists only real implementation `.cpp` files: facade, `TagPipeline`, IO/text/cover helpers, and migrated ID3/Vorbis/FLAC/Ogg/MP4 parser modules. Empty Task 2 scaffold sources were removed from both CMake and the filesystem.
- Removed zero-byte scaffold files: `src/core/TagReaderCore.cpp`, `src/media/MediaInfo.cpp`, `src/io/ReadContext.cpp`, `src/formats/id3/Id3Metadata.cpp`, `src/formats/vorbis/VorbisMetadata.cpp`, `src/formats/flac/FlacMetadata.cpp`, `src/formats/ogg-vorbis/OggVorbisMetadata.cpp`, and `src/formats/mp4/Mp4Metadata.cpp`.
- Public include boundary remains unchanged: `include/TagReader.hpp` only includes `Tag.hpp` and `<filesystem>`; `include/TagReaderInternal.hpp` remains limited to shared cover decode/output limits used by `src/cover`.

## 2026-05-31 Task 13 final verification

- Final local verification passed default configure/build, exact `TagReaderTest` stdout comparison for all 8 comparable generated FLAC baseline entries, and `TagReaderSecuritySmoke` against `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac` with a fresh `/tmp/opencode` cover directory.
- Architecture evidence stayed clean: no forbidden parser abstraction/placeholder patterns in `src/include`, exactly two public `TagReader::Read` overload declarations, no internal module include path leakage under `include`, and expected CMake target declarations remained present.
- Sanitizer and fuzz handling were both supported locally: GNU sanitizer build passed in `build-sanitize`, and `/usr/bin/clang++` built `TagReaderFuzz` in `build-fuzz-clang`.

## 2026-05-31 F1 remediation

- F1 rejection was valid: the plan listed `src/media/FfmpegSession.*`, `src/media/MediaInfoReader.*`, and `src/media/ContainerDetector.*`, but Task 13 still left FFmpeg open/session, stream/media info, and raw-byte-first container/tag detection inside `src/core/TagPipeline.cpp`.
- The remediation uses a behavior-preserving move: `ReadContext::FormatContextDeleter::operator()` and `OpenContext()` live in `FfmpegSession.cpp`; `DetectStream()` and `ReadMediaInfo()` live in `MediaInfoReader.cpp`; `DetectTagFormat()` and `ContainerFromTagFormat()` live in `ContainerDetector.cpp`.
- Final `reinterpret_cast` evidence must be a current full inventory, not only a clean/dirty scan summary. The remediation records all 29 current matches from `src include` in `.omo/evidence/final-cast-inventory.txt` with boundary classification.
