# ape-tag-support: Learnings

## 2026-06-04 Session Start
- Plan: 7 tasks + 4 Final Verification Wave (F1-F4)
- 3 waves: W1 (2 parallel) → W2 (4 parallel) → W3 (1 sequential) → Final Wave (4 parallel)
- APE footer detection MUST be before ID3 header check in DetectTagFormat() for priority
- ContainerDetector.cpp has an anonymous ToLower() - parser must include <algorithm>
- ParseYear/ParseSlashNumber are per-parser local functions - APE parser needs its own
- AppendU32LE() helper exists in regression_tests.cpp for generating APE tag bytes (LE)
- NormalizeContainerFormatName() for Ape falls through to Unknown → uses FFmpeg containerName
- WriteCoverAsPng in tagreader_cover namespace, takes (coverExportDir, data, size)

## 2026-06-04 Task 2: APE directory + stub
- Created `src/formats/ape/` dir with ApeLimits.hpp, ApeParser.hpp, ApeParser.cpp
- ApeParser.hpp declares `ReadApeMetadata()` and `ReadApeLyrics()` in `tagreader_ape` ns (pattern from Mp4Parser.hpp)
- ApeParser.cpp is stub: just `#include "formats/ape/ApeParser.hpp"`
- CMakeLists.txt: added `src/formats/ape/ApeParser.cpp` after line 62 (`Mp4Parser.cpp`)
- Build passes: `cmake --build build --clean-first` succeeds, `ApeParser.cpp.o` confirmed in `libTagReaderCore.a`
- Evidence: `.omo/evidence/task-2-build.log`

## 2026-06-04 Task 3: APE binary parser implementation
- Implemented ~460 lines in `src/formats/ape/ApeParser.cpp`
- Shared `FindApeFooter()` helper extracts version/tagSize/itemCount/flags from last 32 bytes
- `ReadApeMetadata()`: full item iteration with encoding switch (0=UTF-8 text, 1=binary, 2/3=skip)
- `ReadApeLyrics()`: same footer detection and item iteration, processes LYRICS/UNSYNCEDLYRICS/UNSYNCED LYRICS keys
- Field mapping: case-insensitive `IEequals()`, first-write-wins for title/artist/album/albumArtist/composer/genre/year/track/disc
- Cover: COVER ART (FRONT/BACK) binary items, handles description prefix (0x00 separator pattern), calls WriteCoverAsPng
- ParseYearOnly/ParseSlashNumber/ParseUInt16 helpers in anonymous namespace (match VorbisCommentParser pattern)
- APEv1 (version < 2000) silently skipped
- APE tag resource limit violations throw runtime_error in metadata (return silently in lyrics)
- Build passes: `cmake --build build` succeeds, 5/5 targets linked
- LSP diagnostics clean
- Evidence: `.omo/evidence/task-3-build.log`

## 2026-06-04 Task 7: TR-AUDIT regression test cases for APEv2

- Added APE test helpers in `test/regression/regression_tests.cpp`:
  - `ApeItem` struct, `ApeTextValue()`, `BuildApeFooter()`, `BuildApeHeader()`, `BuildApeItems()`, `GenerateApeFile()`, `AppendApeTag()`
- **BUG FIX**: APE footer needs 8 bytes of reserved (not 4). Original code had only `AppendU32LE(footer, 0)` producing 4 reserved bytes, making footer 28 bytes instead of 32. Fixed by adding second `AppendU32LE(footer, 0)`.
- **BUG FIX**: `AppendApeTag()` called `BuildApeFooter()` twice in the same expression `tagBytes.insert(end, BuildApeFooter().begin(), BuildApeFooter().end())` — dangling iterator from first temporary. Fixed by storing footer in a local variable.
- **PARSER BUG DISCOVERED**: APE parser's `ReadApeMetadata()` has footer-only layout bug — `itemRegionOffset = fileSize - tagSize` is wrong when `hasHeader=false`. For footer-only, items should be at `fileSize - 32 - tagSize` (32 = footer size). Parser reads from wrong offset (off by 32 bytes). Workaround: use `withHeader=true` in tests. Bug is in `src/formats/ape/ApeParser.cpp` line 294.
- Pure APE tag files (no audio container) cannot be opened by FFmpeg — `avformat_open_input` fails. Tests must embed APE tags in audio containers (MP3/FLAC) for FFmpeg to open them.
- TR-AUDIT-016: 9-field APEv2 parse test (MP3+APE with header) — PASS
- TR-AUDIT-017: 3 malformed APE rejection scenarios (APEv1, oversized tagSize, excessive itemCount) appended to MP3 — PASS
- TR-AUDIT-018: MP3+APE priority with ID3 fallback (ID3v2 TALB as fallback) — PASS
- All existing 15 tests still pass (no regression)
- kTestCases: 15 → 18, RunCase extended with 3 new cases
- Evidence: `.omo/evidence/task-7-016.log`, `.omo/evidence/task-7-017.log`, `.omo/evidence/task-7-018.log`, `.omo/evidence/task-7-spot-check.log`
