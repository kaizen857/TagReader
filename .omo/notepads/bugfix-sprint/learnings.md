# Bugfix Sprint Learnings

## HIGH-001 + MEDIUM-005 + AD-001: Stream State Recovery in TagPipeline

### Root Cause
After a swallowed exception in `ReadMetadata()` or `ReadLyrics()`, `std::ifstream` can retain `failbit` or `eofbit`. Subsequent `ReadRange()` calls rely on `seekg()` which fails when failbit is set, causing downstream parsers to silently produce no data.

### Fix (4 strategic `context.input.clear()` insertions)

| Point | Bug ID | Location | Rationale |
|-------|--------|----------|-----------|
| A | HIGH-001 | `ignoreMalformedMetadata` lambda — both catch blocks | Catch blocks swallow errors but leave stream in bad state. `clear()` added in `filesystem_error` catch and `runtime_error` catch (non-throw path). Lambda capture changed from `[]` to `[&context]`. |
| B | MEDIUM-005 | Between ID3v2 and ID3v1 parser calls | ID3v2 parser may leave stream in bad state (e.g., partial read past tag boundary), preventing ID3v1 fallback. |
| C | AD-001 | `ReadMetadata()` entry | Defensive — ensures clean stream state before any parser call regardless of prior pipeline stage. |
| D | AD-001 | `ReadLyrics()` after `is_open()` guard | Defensive — ensures clean stream state before lyrics parsing, which runs after `ReadMetadata()`. |

### File Modified
- `src/core/TagPipeline.cpp` (4 insertions, 1 lambda capture change, 6 lines added)

### Verification
- `cmake --build build 2>&1` — 0 errors, all 6 targets linked
- `lsp_diagnostics` — clean


## MEDIUM-004: ID3 Frame Resync Scan Budget

### Root Cause
`TryResyncId3v22Frame()` / `TryResyncId3v23Or24Frame()` had no scan budget — they would iterate from `cursor+1` to the end of the tag (potentially 16 MiB), causing degenerate O(n²) scanning on corrupt tags with many invalid frames.

### Fix (7 call sites, budget = 4096 bytes)
Added `kId3ResyncScanBudget = 4096` constant. At each resync call site in `ReadID3v22Frames()` (3 calls) and `ReadID3v23Or24Frames()` (4 calls), the `limit` parameter is capped to `cursor + 4096`, preventing resync from scanning more than 4096 bytes per attempt.

### What was NOT changed
- `ReadID3v22LyricsFrames` and `ReadID3v23Or24LyricsFrames` resync calls — task scope was metadata frame walkers only
- `TryResyncId3v22Frame()` / `TryResyncId3v23Or24Frame()` implementations — existing logic is fine
- Padding detection (`IsId3PaddingStartAtOriginalCursor`) — still returns false to trigger immediate break

### File Modified
- `src/formats/id3/Id3Frames.cpp` (+1 const, 7 limit → budget-capped)

### Verification
- `cmake --build build 2>&1` — 0 errors, all targets linked
- `lsp_diagnostics` — clean
