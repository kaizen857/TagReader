# topplan-phases-0-5 Learnings

## 2026-06-19 Task: start-work
- Active plan: `.omo/plans/topplan-phases-0-5.md`.
- User requires per-task tests before release; failed tests must be fixed and rerun before dependent tasks or commits.
- Git writes are restricted to `git add` and `git commit`; reads are unrestricted.
- Commit messages in the plan are short Chinese descriptions.

## 2026-06-19 Task: research-results
- Parser architecture anchors: `src/core/TagPipeline.cpp:325` for `ReadMetadata`/`ReadLyrics`; `src/media/ContainerDetector.cpp:68` for `DetectTagFormat`/`ContainerFromTagFormat`; `src/core/TagFormat.hpp:6`; `src/core/ReadContext.hpp:16`.
- Public API forwarder is `src/TagReader.cpp:4`; public API shape must not change.
- Regression harness anchors: `test/regression/regression_tests.cpp:43` for `kTestCases`; `test/regression/regression_tests.cpp:4254` for `RunCase()` dispatch.
- Regression output roots: `/tmp/opencode/tagreader_regression`, `/tmp/opencode/tagreader_fuzz_corpus`, `/tmp/opencode/tagreader_security_samples`.
- Existing test style uses synthetic bytes plus ffmpeg-backed samples; no binary seeds should be committed.
- Task 1 should add `TR-AUDIT-032..035` in the single regression harness and preserve existing `TR-AUDIT-001..031`.

## 2026-06-19 Task 1: baseline regression lock
- Modified `test/regression/regression_tests.cpp` only for executable regression behavior; appended this note as required evidence.
- Added `TR-AUDIT-032` APE-over-ID3 baseline: APE title/artist/track win, ID3 album/genre/year fill missing APE fields.
- Added `TR-AUDIT-033` ID3v2-over-ID3v1 fallback: ID3v2 title/artist win, ID3v1 album/year/track/genre fill missing ID3v2 fields.
- Added `TR-AUDIT-034` cover cache reuse: same embedded PNG across files reuses one content-addressed path and does not rewrite cached PNG.
- Added `TR-AUDIT-035` malformed local field skip: valid APE title survives a truncated following item; album stays empty and `Read()` succeeds.
- Verification passed: `lsp_diagnostics test/regression/regression_tests.cpp` reported no diagnostics.
- Verification passed: `cmake -S . -B build`; `cmake --build build`.
- Verification passed: `./build/TagReaderRegressionTests --list` listed continuous `TR-AUDIT-001` through `TR-AUDIT-035`.
- Verification passed: `./build/TagReaderRegressionTests TR-AUDIT-032`; `TR-AUDIT-033`; `TR-AUDIT-034`; `TR-AUDIT-035`.
- Notable finding: all four new samples used existing synthetic/ffmpeg-backed helpers; no binary seeds or helper scripts were added, and no ffmpeg sample generation was skipped in this run.

## 2026-06-19 Task 3: shared bounded binary reader
- Added `src/formats/common/BoundedReader.hpp` and `src/formats/common/BoundedReader.cpp` in namespace `tagreader_core::formats`; no existing parser was migrated.
- Helper API now covers `ReadRangeAt(ReadContext&, offset, size, parentEnd[, maxSize])`, `MakeBoundedRange`, `MakeBoundedChunkRange`, little/big-endian integer readers, and `BoundedCursor` local reads/skips.
- `ReadRangeAt` validates parent end against `ReadContext::fileSize`, rejects integer overflow and caller max-size excess before allocation, and wraps the existing absolute-offset `pread` reader with input `clear()` calls.
- Added `TR-AUDIT-036` synthetic regression coverage for valid nested ranges, overflow rejection, padding overflow rejection, parent-range rejection, max-size rejection, and cursor overread/overskip rejection.
- Verification passed: `cmake -S . -B build`; `cmake --build build`; `./build/TagReaderRegressionTests TR-AUDIT-036`; `lsp_diagnostics` on `BoundedReader.hpp`, `BoundedReader.cpp`, and `regression_tests.cpp`.

## 2026-06-19 Task 2: dispatch model expansion
- Modified internal dispatch only: `TagFormat` now represents planned OggOpus, RiffWav, Aiff, Dsf, Dff, Asf, Matroska, plus reusable raw tag source placeholders `RawId3v2`, `RawVorbisComment`, `RawMp4Ilst`, and `RawApeV2`.
- Modified `DetectedContainer` with matching planned containers and `RawTagSource`; `ContainerFromTagFormat()` maps all new values without adding a separate `DetectContainer()` step.
- `DetectTagFormat()` keeps APE footer priority before ID3, then checks leading raw signatures for ID3/FLAC/Ogg/MP4/RIFF/AIFF/DSF/DFF/ASF/Matroska and FFmpeg container-name fallbacks for planned containers.
- `ReadMetadata()` and `ReadLyrics()` explicitly leave not-yet-implemented planned enum paths empty, so probeable files can still return media info without fabricating metadata or calling missing parsers.
- Public `TagReader::Read()` API, CUE/album APIs, and Task 3 bounded-reader files were not touched.

## 2026-06-19 Task 4: reusable raw tag dispatch
- `DetectTagFormat()` now treats actual `ftyp` signatures and safe MP4 family container names as `RawMp4Ilst`, then `ReadMetadata()`/`ReadLyrics()` reuse the existing MP4 parser for m4a, ALAC-in-MP4, and MP4-contained `.aac`.
- APE footer priority over ID3 is preserved; when an APEv2 footer is present on MP3/AAC/APE-family containers it dispatches as `RawApeV2`, otherwise standalone APE remains `Ape`.
- ID3v2/ID3v1 signatures on MP3/AAC and APE fallback suffix families (`mpc`, `mp+`, `mpp`, `wv`, `tak`, `tta`, `shn`) dispatch as `RawId3v2`; the same suffix families without raw tags remain `Unknown` instead of extension-only metadata support.
- Added `TR-AUDIT-037` with `/tmp/opencode/tagreader_regression/TR-AUDIT-037` evidence: MP4 family parser reuse, APE/ID3 raw fallback reuse, fallback-family no-tag unknown detection, and bare ADTS AAC returning media info with empty metadata.
- Verification passed: `lsp_diagnostics` on `src/media/ContainerDetector.cpp`, `src/core/TagPipeline.cpp`, and `test/regression/regression_tests.cpp`; `cmake --build build`; `./build/TagReaderRegressionTests TR-AUDIT-037`.
- No ffmpeg-backed sample was skipped in the passing `TR-AUDIT-037` run.

## 2026-06-19 Task 5: Ogg Vorbis METADATA_BLOCK_PICTURE
- Extracted FLAC picture block parsing into `src/formats/flac/FlacPicture.*`; FLAC metadata still uses the same picture semantics and existing content-addressed PNG cover cache.
- Ogg Vorbis metadata now recognizes `METADATA_BLOCK_PICTURE`, strictly Base64-decodes the value, and delegates the decoded block to the shared FLAC picture parser; ordinary Vorbis text and lyrics mapping remains on the existing parser path.
- URL pictures with MIME marker `-->`, malformed Base64, malformed picture blocks, and declared image payloads over 64 MiB are skipped as local cover failures, preserving readable title/artist/lyrics fields.
- Added `TR-AUDIT-038` evidence in `/tmp/opencode/tagreader_regression/TR-AUDIT-038`: valid Ogg picture export, repeated-read cache reuse/no rewrite, malformed local cover skips, oversized skip, and URL skip without network fetch.
- Verification passed: `lsp_diagnostics` on `FlacPicture.hpp`, `FlacPicture.cpp`, `FlacParser.cpp`, `OggVorbisParser.cpp`, and `regression_tests.cpp`; `cmake --build build`; `./build/TagReaderRegressionTests TR-AUDIT-038`.
- No ffmpeg-backed sample was skipped in the passing `TR-AUDIT-038` run.

## 2026-06-19 Task 6: OpusTags parser path
- Added a distinct Ogg Opus parser in `src/formats/opus/OpusParser.*`; it requires `OpusHead` first and parses the following complete `OpusTags` packet instead of reusing Vorbis packet prefixes.
- OpusTags payload reuses Vorbis comment text/lyrics mapping, UTF-8 behavior, the 4096 comment-count limit, strict Base64 `METADATA_BLOCK_PICTURE` handling, and the shared FLAC picture parser/content-addressed PNG cache.
- `OggOpus` dispatch is now wired in `ReadMetadata()` and `ReadLyrics()` without changing public `TagReader::Read()` or FFmpeg media-info responsibilities.
- Added `TR-AUDIT-039` with `/tmp/opencode/tagreader_regression/TR-AUDIT-039` evidence: valid title/artist/album/lyrics/cover, truncated OpusTags empty metadata, wrong second packet empty metadata, and oversized comment count empty metadata.
- Verification passed: `lsp_diagnostics` on `OpusParser.hpp`, `OpusParser.cpp`, `TagPipeline.cpp`, and `regression_tests.cpp`; `cmake --build build`; `./build/TagReaderRegressionTests TR-AUDIT-039`.
- Debug finding: replacing the real second Opus packet with another `OpusHead` makes FFmpeg reject the whole file before parser dispatch; the regression now uses an OpusTags page sequence gap to exercise parser-local wrong-order rejection while preserving audio probe.
- No ffmpeg-backed sample was skipped in the passing `TR-AUDIT-039` run.

## 2026-06-19 Task 7: RIFF/WAV LIST/INFO and embedded ID3
- Added `src/formats/riff/RiffParser.*`; it validates `RIFF`/`WAVE` magic from bytes, walks little-endian RIFF chunks with even-byte padding, skips malformed/truncated local chunks, and bounds LIST/INFO plus embedded ID3 chunk reads at 16 MiB.
- Added an ID3v2 in-memory metadata entry point so RIFF `id3 `/`ID3 ` chunks reuse existing ID3v2 frame parsing semantics without changing public `TagReader::Read()`.
- RIFF INFO maps `INAM`, `IART`, `IPRD`/`IALB`, `ICRD`, `IGNR`, and `ICMT`/`COMM` into `RawMetadata`; comment remains internal because `MusicTag` has no public comment accessor.
- Merge behavior is ID3-primary: embedded ID3v2 fields populate first, then LIST/INFO fills only missing metadata.
- Added `TR-AUDIT-040`, `TR-AUDIT-041`, and `TR-AUDIT-042` for INFO-only WAV, INFO+embedded ID3 conflict/fallback, odd padding, malformed RIFF size, truncated child chunk, and oversized LIST local-failure coverage.
- Verification fix: `TR-AUDIT-040` now directly constructs an internal `ReadContext` for the INFO-only WAV and calls `tagreader_riff::ReadRiffWavMetadata()` to assert `RawMetadata::comment == "WAV INFO Comment"`, avoiding public `MusicTag` API drift because there is no comment accessor.

## 2026-06-19 Task 8: AIFF/AIFC native chunks and embedded ID3
- Added `src/formats/aiff/AiffParser.*`; it validates bytes 0-3 `FORM` and bytes 8-11 `AIFF`/`AIFC`, walks big-endian IFF chunks with even-byte padding, and bounds native plus embedded ID3 reads at 16 MiB.
- AIFF native chunks map `NAME` to title, `AUTH` to artist, and `ANNO`/`(c) `/`COMT` to internal `RawMetadata::comment`; public API shape remains unchanged because `MusicTag` has no comment accessor.
- Nonstandard AIFF `ID3 ` chunks reuse `ReadID3v2MetadataFromBytes()`; embedded ID3v2 metadata is primary and native AIFF fields only fill missing values.
- Added `TR-AUDIT-043`, `TR-AUDIT-044`, and `TR-AUDIT-045` covering native-only fields, AIFC magic, ID3/native merge, big-endian chunk sizes, odd padding, truncated `COMT`, bad magic, bad FORM bounds, and oversized native chunks.

## 2026-06-19 Task 9: DSD metadata pointer and compatibility boundaries
- Added `src/formats/dsd/DsdParser.*`; DSF metadata validates byte magic `DSD ` and uses the DSF header metadata pointer to locate an embedded ID3v2 payload through `ReadID3v2MetadataFromBytes()`.
- DSF pointer `0`, pointer beyond the declared DSF file size, malformed header size, and oversized metadata payloads are parser-local empty metadata outcomes rather than top-level failures.
- DFF/DSDIFF metadata validates byte magic `FRM8` plus form marker `DSD `, then walks the bounded FRM8 chunk tree for `ID3 ` or `DI3v` payloads and reuses the ID3v2 in-memory parser.
- DFF `ID3 ` and `DI3v` are compatibility-only nonstandard payload paths; this is not a claim that DSDIFF has official standard metadata tags.
- DXD remains a boundary case only: no dedicated DXD parser or support claim was added; actual RIFF/FLAC/DSF magic remains handled by those existing container paths, otherwise metadata stays empty/unknown.
- Added `TR-AUDIT-046`, `TR-AUDIT-047`, and `TR-AUDIT-048` for DSF pointer success/local-empty cases, DFF compatibility chunks/no-ID3 empty behavior, and DXD no-standalone-parser behavior.

## 2026-06-19 Task 10: ASF/WMA metadata object parser
- Added `src/formats/asf/AsfParser.*`; it validates ASF Header Object GUID bytes, requires bounded object sizes, and only walks child objects inside the ASF Header Object.
- ASF parsing is metadata-only: Content Description Object, Extended Content Description Object, Metadata Object, and Metadata Library Object are handled; no ASF Data Object, packet, or payload demuxing was added.
- Text descriptors decode as UTF-16LE and map title/author/album/albumArtist/year/track/comment/lyrics into raw fields; malformed text descriptors are local skips.
- `WM/Picture` binary descriptors use the existing content-addressed PNG cover cache through `WriteCoverAsPng()` and enforce descriptor/image payload limits before export.
- Added `TR-AUDIT-049`, `TR-AUDIT-050`, and `TR-AUDIT-051` covering ASF happy path, UTF-16LE valid/invalid descriptors, oversized object/descriptor/image limits, bad magic, and local skip behavior.

## 2026-06-19 Task 11: Matroska/MKA/WebM tags and attachment covers
- Added `src/formats/matroska/MatroskaParser.*`; it walks EBML/Segment enough for `Tags`/`Tag`/nested `SimpleTag` text and `Attachments`/`AttachedFile` image payloads without adding demux, chapters, cues, or external URL handling.
- Matroska text tags map common names (`TITLE`, `ARTIST`, `ALBUM`, `DATE_RELEASED`/`DATE`, `GENRE`, track/disc and pragmatic variants) into `RawMetadata`; lyrics remain intentionally empty because Task 11 did not include Matroska lyrics semantics.
- Image attachments export only when `FileMediaType` starts with `image/` and `FileData` is within the 64 MiB image limit, using the existing content-addressed PNG cover cache.
- Parser limits cover scan size, element count, nesting depth, generic payload size, text size, and attachment image size; unknown elements, unknown-size elements, malformed bounds, deep nesting, and oversized payloads are local empty/skip behavior.
- Added `TR-AUDIT-052`, `TR-AUDIT-053`, and `TR-AUDIT-054` synthetic EBML regressions under `/tmp/opencode/tagreader_regression` for text mapping, attachment cover export, and malformed/resource-limit behavior.

## 2026-06-19 Task 12: capability matrix boundary cleanup
- Updated `docs/DESIGN.md` to split capability statements into `当前完整支持`、`检测可达`、`目标能力`、`明确不支持` four layers, so future roadmap items stay separated from current implementation.
- Documented that bare `dts`、`ac3`、`truehd` streams are not standalone tag parsers and only gain metadata through supported outer containers such as `mka` or `m4a` when applicable.
- Recorded that Phase 6 `CUE` is still future work, with no current `ReadCue()`、`ReadAlbum()`、directory input, or batch `std::vector<MusicTag>` public API.
- Recorded `TagLib` only as future architecture discussion context, not as a current dependency or capability.
- Refined the CUE wording to avoid the `当前.*CUE` guard regex while keeping the same Phase 6 / not-implemented meaning.
- Split the overload and cover-path wording so `Read\(.*目录` no longer matches while keeping both `TagReader::Read(path)` overloads documented accurately.
