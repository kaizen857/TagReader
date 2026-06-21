---
slug: read-cue-sheet
status: plan-written
intent: clear
pending-action: write .omo/plans/read-cue-sheet.md
approach: Add explicit ReadCueSheet APIs returning std::vector<MusicTag>, implement independent bounded CUE parser/mapper, reuse ReadTag for audio metadata, add same-directory cover fallback, and verify with Catch2/CTest.
---

# Draft: read-cue-sheet

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->
- api | Public `TagReader::ReadCueSheet(cuePath)` and `ReadCueSheet(cuePath, coverExportDir)` return `std::vector<MusicTag>` without changing `Read()` | active | `include/TagReader.hpp:10`, `src/TagReader.cpp:4`, `CUE_HANDLING_RESEARCH.md:150`
- parser | Independent CUE text module decodes to UTF-8 and parses bounded FILE/TRACK/INDEX/TITLE/PERFORMER/SONGWRITER/REM data | active | `CUE_HANDLING_RESEARCH.md:18`, `CUE_HANDLING_RESEARCH.md:112`
- mapper | CUE tracks become complete `MusicTag` objects using CUE-over-audio field precedence and correct offset/duration | active | `include/Tag.hpp:11`, `src/core/TagPipeline.cpp:532`, `CUE_HANDLING_RESEARCH.md:83`
- cover | External same-directory cover fallback runs only when audio embedded cover is absent and reuses PNG/cache/export semantics | active | `src/cover/CoverDecoder.cpp:268`, `src/cover/CoverCache.cpp:42`, `CUE_HANDLING_RESEARCH.md:103`
- tests-docs | Catch2/CTest coverage, `TR-AUDIT-056` boundary migration, docs/AGENTS updates | active | `test/CMakeLists.txt:1`, `test/regression/regression_tests.cpp:175`, `CUE_HANDLING_RESEARCH.md:136`

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->
- Last-track duration | Use referenced audio total duration minus last track offset; if total duration unavailable, keep duration `0` only when metadata is otherwise valid and record no public diagnostic in v1 | Existing `MusicTag` has no diagnostic channel and `ReadTag()` returns `duration` when FFmpeg can provide it | yes
- Malformed local track field | Skip malformed optional field and retain the track when FILE and INDEX 01 are valid | Matches existing parser local-failure policy for malformed fields | yes
- CUE resource limits | 4 MiB file, 10000 lines, 99 tracks, 256 FILE references, 99 indexes per track, 64 KiB text field, 256 cover candidates, 64 MiB image input cap inherited from cover decoder | Consistent with current bounded-parser posture and CUE/CD constraints | yes
- External cover extensions | Recognize common image extensions supported by existing decoder path: `.png`, `.jpg`, `.jpeg`, `.bmp`, `.webp`, `.gif`, `.tif`, `.tiff`; unknown magic fallback may decode content only after extension filter passes | User said any image extension, current decoder supports these concrete codecs | yes

## Findings (cited - path:lines)
- Current public API only has `Read(path)` and `Read(path, coverExportDir)`, both returning `MusicTag`: `include/TagReader.hpp:10`.
- Public wrapper currently only forwards to `tagreader_core::ReadTag()`: `src/TagReader.cpp:4`.
- `ReadTag()` validates path, opens FFmpeg context, validates cover export directory, reads media info/metadata/lyrics, then builds `MusicTag`: `src/core/TagPipeline.cpp:592`.
- `BuildMusicTag()` already maps metadata, lyrics, file path, cover path, duration, offset, lastModified, sampleRate, bitDepth, bitRate, channels, format, playCount, and rating into `MusicTag`: `src/core/TagPipeline.cpp:532`.
- Existing text codec can detect and decode UTF-8/BOM, UTF-16LE/BE, Latin-1, and locale encoded text into UTF-8: `src/text/TextCodec.cpp:610`.
- Existing cover decoder converts PNG/JPEG/BMP/WebP/GIF/TIFF or fallback detected image bytes to PNG with limits: `src/cover/CoverDecoder.cpp:268`.
- Existing cover cache is content-addressed and bounded by 64 MiB input: `src/cover/CoverCache.cpp:42`.
- Current `TR-AUDIT-056` statically forbids `ReadCue`/`ReadAlbum` and batch `Read()` returns; it must be migrated to allow `ReadCueSheet` while still forbidding batch `Read()`: `test/regression/regression_tests.cpp:175`, `test/regression/regression_tests.cpp:7653`.
- Test framework is Catch2 + CTest from `test/CMakeLists.txt`; new CUE tests should register there: `test/CMakeLists.txt:1`.
- CUE research and confirmed decisions are in `CUE_HANDLING_RESEARCH.md:150`.

## Decisions (with rationale)
- API name is fixed as `ReadCueSheet` with two overloads: one default cover export dir overload and one explicit `coverExportDir` overload.
- Return type is v1 `std::vector<MusicTag>`; no diagnostic result object in first implementation.
- Existing `Read()` semantics must remain unchanged: no implicit `.cue` scanning, no directory input, no batch return.
- CUE text must be decoded to UTF-8 before parsing; non-UTF-8 strings must never enter `MusicTag`.
- CUE fields override audio fields when both exist; otherwise CUE and audio values fill each other's gaps.
- Single-file CUE with missing/unreadable referenced audio fails the whole CUE; multi-file CUE skips tracks belonging to missing/unreadable files and returns remaining valid tracks.
- External cover fallback is same-directory only, ordered by `cover`, `front`, `folder`, `album`, `artwork`, and runs only when embedded audio cover is absent.

## Scope IN
- Add public `TagReader::ReadCueSheet(const std::filesystem::path &cuePath)`.
- Add public `TagReader::ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir)`.
- Add CUE parser/reader code under a dedicated CUE module near existing format modules.
- Decode CUE bytes to UTF-8 before command parsing.
- Parse `FILE`, `TRACK`, `INDEX`, `TITLE`, `PERFORMER`, `SONGWRITER`, and selected `REM` metadata (`GENRE`, `DATE`, `YEAR`, `DISCNUMBER` if safely parseable).
- Resolve safe CUE `FILE` references relative to the `.cue` directory.
- Build `std::vector<MusicTag>` with CUE-over-audio field precedence, media/lyrics/cover completion via existing `ReadTag()`.
- Add same-directory external cover fallback for referenced audio files with no embedded cover.
- Update `TR-AUDIT-056`, add CUE Catch2 tests, and update docs/AGENTS/CUE research as current capability.

## Scope OUT (Must NOT have)
- Do not change either existing `TagReader::Read()` overload signature or return type.
- Do not make `Read()` implicitly discover, parse, or apply `.cue` sidecars.
- Do not accept directory input for CUE or audio APIs.
- Do not depend on FFmpeg `AVDictionary` for CUE metadata fields.
- Do not support FLAC embedded binary `CUESHEET` as part of this work.
- Do not expose `ReadAlbum` in this first CUE implementation.
- Do not recursively scan folders or follow path/symlink escapes for CUE `FILE` or external cover candidates.
- Do not introduce a diagnostic/result object unless the user revises the confirmed v1 return type.

## Open questions
- None blocking. Adopted defaults are recorded above and can still be vetoed before execution.

## Approval gate
status: approved-for-plan-write
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
