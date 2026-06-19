# MEDIUM-001 Learnings

## TR-AUDIT-019: Encoding Detection Regression Test

### Architecture Finding
**`DetectLegacyLocalEncoding` is only exercised through ID3v1 and MP4 parsers** — the APE parser (`ProcessApeTextItem`) and ID3v2 text frame parser both use `ReadUtf8Text` / `ReadId3ByteString` respectively, neither of which performs encoding detection. They rely on explicit encoding markers (UTF-8 flag in APE, encoding byte in ID3v2).

### Test Design Decision
The TR-AUDIT-019 regression test exercises `DetectTextEncoding → DetectLegacyLocalEncoding` through an **ID3v1 tag** with GB18030-encoded Chinese Title ("测试标题", GB18030 bytes: B2 E2 CA D4 B1 EA CC E2). This is the only MP3-family code path that invokes the encoding detection logic.

### Code Changes
- `src/text/TextCodec.cpp` L150-157: Added `#warning` in the `#else` block (no-iconv path) listing all affected encodings
- Upgraded comment from "WARNING" to "CRITICAL LIMITATION"

### Test Result
- `TR-AUDIT-019 PASS`: GB18030 encoding correctly detected, title decoded to UTF-8 "测试标题"
- All 18 existing regression tests continue to pass
- Sanitizer build clean, no memory errors

### Key Limitation
The APE parser does NOT support `DetectLegacyLocalEncoding` for text fields. Non-UTF-8 text in APE tags is passed through by `ReadUtf8Text` as raw bytes, then cleared by `NormalizeMetadata` (which validates UTF-8). This means CJK text encoded in legacy encodings (GB18030/GBK/SHIFT_JIS/BIG5) in APE tags will be lost even in with-iconv builds.

---

# MEDIUM-006 Learnings

## TR-AUDIT-020: APE itemRegionOffset Unsigned Subtraction Wrap

### Vulnerability
In `ReadApeMetadata()` (pre-fix L293-295), when a malformed APE footer has `tagSize > fileSize`, the expression `context.fileSize - static_cast<uint64_t>(tagSize)` wraps around to a huge `uint64_t` value due to unsigned integer arithmetic. Although downstream `ReadRange()` would eventually catch the out-of-range offset, the subtraction itself is a logic defect that produces a garbage offset.

### Fix
Added a guard at `ApeParser.cpp` L295-298, immediately after `hasHeader` assignment and before `itemRegionOffset` calculation:

```cpp
if (tagSize > context.fileSize - 32)
{
    return;
}
```

The guard uses `fileSize - 32` because the 32-byte APE footer is always at EOF. Without a header, items sit before the footer; with a header, items start at `fileSize - tagSize`. In both cases, `tagSize` must not exceed `fileSize - 32`.

### Test Design
`RunTrAudit020()` constructs a malformed APE footer (`tagSize=0xFFFFFFFF`, `itemCount=1`, `flags=0`) appended to a base MP3. The 32-byte footer structure:
- magic: "APETAGEX" (8 bytes)
- version: 2000 (4 bytes LE)
- tagSize: 0xFFFFFFFF (4 bytes LE)
- itemCount: 1 (4 bytes LE)  
- flags: 0 (4 bytes LE)
- reserved: 8 bytes of zero

### Code Changes
- `src/formats/ape/ApeParser.cpp` L291-298: Added guard with explanatory comment
- `test/regression/regression_tests.cpp`: Added `RunTrAudit020()`, updated `kTestCases` array to 20 entries, added RunCase dispatch

### Test Result
- `TR-AUDIT-020 PASS`: No crash, title/artist/album all empty (malformed APE silently rejected)
- `TR-AUDIT-016 PASS`: Normal APE with header still works correctly
- All 20 regression tests pass (19 existing + 1 new)
- LSP diagnostics clean on both changed files

---

# MEDIUM-004 Learnings

## TR-AUDIT-021: ID3 Resync Scan Budget Increase (4096 → 16384)

### Architecture Finding
`kId3ResyncScanBudget` controls how far `TryResyncId3v23Or24Frame` scans (byte-by-byte) past a malformed frame before giving up. It's a `constexpr` used at 4 call sites in `ReadID3v23Or24Frames`:
1. `!IsLikelyId3FrameId(frameId)` — frame ID has non-alphanumeric chars
2. `frameSize == 0` — zero-length frame
3. `frameSize > limit - cursor - 10` — oversize frame

### Key Implementation Detail
The resync function has a critical fast-path: **`IsId3PaddingStartAtOriginalCursor`** checks if `tagBytes[cursor] == 0` and immediately aborts the scan. This means **zero-byte gaps are treated as padding and never scanned for resync**. Malformed data must use non-zero bytes (e.g., `0xFF`) to actually exercise the scan loop.

### Test Design
`RunTrAudit021()` constructs an ID3v2.3 tag with exactly three regions:
- Valid `TIT2` frame ("TestTitle")
- Invalid frame ID `"!!!!"` (not alphanumeric → triggers resync) followed by 5000 bytes of `0xFF` (non-zero, avoids padding fast-path)
- Valid `TALB` frame ("RecoveredAlbum")

The 5000-byte gap exceeds the old budget of 4096 but fits within the new budget of 16384.

### Code Changes
- `src/formats/id3/Id3Frames.cpp` L43: `kId3ResyncScanBudget` changed from 4096 → 16384 with explanatory comment
- `test/regression/regression_tests.cpp`: Added `RunTrAudit021()`, updated `kTestCases` array to 21 entries, added RunCase dispatch for TR-AUDIT-021

### Test Result
- `TR-AUDIT-021 PASS`: Title="TestTitle", Album="RecoveredAlbum" (resync recovers TALB at 16384 budget)
- `TR-AUDIT-006 PASS`: Existing resync test still passes
- All 21 regression tests pass (20 existing + 1 new)
- LSP diagnostics clean on both changed files

---

# LOW-002 Learnings

## TR-AUDIT-022: 0x00 Byte Handling in Text Decoding

### Vulnerability
`ReadLatin1Text()` and `ReadUtf8Text()` both treated embedded 0x00 bytes as C-string terminators, truncating the decoded text. For Latin-1: `if (ch == 0) { break; }` stopped iteration. For UTF-8: `value.find('\\0')` with `value.resize(nul)` truncated. Text like "Hello\0World" would be decoded as "Hello".

### Design Context
- **`FindEncodedTerminator()`** already strips structural 0x00 delimiters (ID3 frame separators) before `ReadLatin1Text()` is called, so this change only affects cases where 0x00 is genuinely embedded in text content.
- Both Latin-1 and UTF-8 code paths affected: Latin-1 in ID3 text frames (after terminator stripping), UTF-8 in APE text items (`ProcessApeTextItem → ReadUtf8Text`).
- For ASCII-range bytes (< 0x80), UTF-8 and Latin-1 are byte-identical, so both functions need the same 0x00→space treatment.

### Fix
**`ReadLatin1Text()`** — `TextCodec.cpp` L475-483:
```cpp
if (ch == 0)
{
    value.push_back(' ');
    continue;
}
```
Previously: `if (ch == 0) { break; }` (truncation).

**`ReadUtf8Text()`** — `TextCodec.cpp` L506-523:
Rewrote from single-string construction + find-resize to byte-by-byte loop matching `ReadLatin1Text()` pattern, with 0x00 → space.

### Test Design
`RunTrAudit022()` constructs a base MP3, creates an APE tag with `TITLE` key and value containing embedded 0x00 (`{'H','e','l','l','o', 0x00, 'W','o','r','l','d'}`), then verifies `TagReader::Read()` returns `title() == "Hello World"` (11 chars including space).

### Code Changes
- `src/text/TextCodec.cpp` L475-483: Changed `break` to `value.push_back(' ')` + `continue` in `ReadLatin1Text()`, updated comment
- `src/text/TextCodec.cpp` L506-523: Rewrote `ReadUtf8Text()` with byte-by-byte 0x00→space loop matching `ReadLatin1Text()` pattern
- `test/regression/regression_tests.cpp`: Added `RunTrAudit022()`, updated `kTestCases` array to 22 entries, added `RunCase` dispatch for TR-AUDIT-022

### Test Result
- `TR-AUDIT-022 PASS`: title="Hello World" (embedded 0x00 → space, length 11)
- `TR-AUDIT-006 PASS`: ID3 frame parsing unaffected (terminators stripped by FindEncodedTerminator)
- All 22 regression tests pass (21 existing + 1 new)
- LSP diagnostics clean on both changed files

---

# AD-001 Learnings

## RAII Stream State Guard for ReadMetadata Parser Calls

### Architecture Finding
`ReadMetadata()` 的 `ignoreMalformedMetadata` lambda 在两个 catch 分支中手动调用 `context.input.clear()` 恢复流状态。若未来添加新的 parser 调用或新的 catch 分支，容易遗漏 clear，导致后续 parser 在 failbit 流上操作。

### Fix
在匿名命名空间中添加 `StreamStateGuard` RAII 类：
```cpp
class StreamStateGuard {
public:
    explicit StreamStateGuard(std::ifstream &stream) noexcept : stream_(stream) {}
    ~StreamStateGuard() { stream_.clear(); }
    StreamStateGuard(const StreamStateGuard &) = delete;
    StreamStateGuard &operator=(const StreamStateGuard &) = delete;
private:
    std::ifstream &stream_;
};
```

在 `ignoreMalformedMetadata` lambda 的 try 块首行实例化 guard，删除两个 catch 块中的手动 `context.input.clear()`：
```cpp
auto ignoreMalformedMetadata = [&context](auto &&readMetadata)
{
    try
    {
        StreamStateGuard guard(context.input);  // RAII: 析构时自动 clear
        readMetadata();
    }
    catch (const std::filesystem::filesystem_error &)
    {
        // guard 析构恢复流状态
    }
    catch (const std::runtime_error &ex)
    {
        if (IsCoverExportOrCacheError(ex.what())) { throw; }
        // guard 析构恢复流状态
    }
};
```

### Preserved Explicit Clears
以下显式 `context.input.clear()` 保留不动（跨格式分支切换需要显式恢复）：
- L117: `ReadMetadata()` 入口 clear
- L155: APE→ID3 回退前 clear
- L166: ID3v2→ID3v1 间 clear
- L194: `ReadLyrics()` 入口 clear

### Code Changes
- `src/core/TagPipeline.cpp`: 新增 `StreamStateGuard` 类（12 行），修改 `ignoreMalformedMetadata` lambda（+1 行 guard，-2 行手动 clear）

### Test Result
- 全部 21 个现有回归测试通过（TR-AUDIT-001~021），零行为变更
- TR-AUDIT-022 为并行 Wave 2 任务新增测试，与本变更无关
- 普通构建 + sanitizer 构建均零错误
- LSP diagnostics 清洁

---

# MEDIUM-007 Learnings

## TR-AUDIT-023: ProcessApeCoverItem valueSize Upper Bound (8 MiB)

### Vulnerability
`ProcessApeTextItem()` has `kMaxApeItemValueBytes` (1 MiB) to limit individual text item size, but `ProcessApeCoverItem()` had no per-item size check. A single cover item could consume up to `kMaxApeTagBytes` (16 MiB), 1/4 of the 64 MiB total cover input limit. This inconsistency left cover parsing without a proportional item-level guard.

### Fix
Added `kMaxApeCoverItemBytes = 8 MiB` check early in `ProcessApeCoverItem()`, immediately after the `valueData == nullptr || valueSize == 0` guard and before the `coverPath.empty()` duplicate-prevention check:

```cpp
constexpr std::size_t kMaxApeCoverItemBytes = 8z * 1024 * 1024; // 8 MiB
if (valueSize > kMaxApeCoverItemBytes)
{
    return;  // silently skip oversized cover item
}
```

Oversized cover items are silently skipped (consistent with the "local tag malformed" philosophy).

### Test Design
`RunTrAudit023()` generates a base MP3, then appends an APE tag with two items:
- Item 1: `Cover Art (Front)`, binary, valueSize = 8 MiB + 1 byte (8388609), payload = zeros
- Item 2: `TITLE`, text, value = "Hello"

Since `GenerateApeFile`/`AppendApeTag` uses `item.value.size()` as the header valueSize, the item value MUST have actual backing data for the parser's internal bounds check to pass. The ~8 MiB test file is acceptably sized for a regression test.

The test verifies:
- `tag.coverPath().empty()` — oversized cover silently skipped
- `tag.title() == "Hello"` — subsequent normal item parsed correctly

### Key Implementation Detail
The parser loop in `ReadApeMetadata()` has its own bounds check (`if (valueSizeSz > itemBytes.size() - cursor) { break; }`) BEFORE calling `ProcessApeCoverItem`. This means the `ProcessApeCoverItem` guard only fires when the item data actually has backing bytes — the `tagSize` in the footer must be large enough to cover the claimed valueSize. Using a value with real backing data (the 8 MiB zero vector) satisfies both checks in sequence: the loop's bounds check passes → `ProcessApeCoverItem` is called → the 8 MiB guard fires → return.

### Code Changes
- `src/formats/ape/ApeParser.cpp` L215-219: Added `kMaxApeCoverItemBytes` guard
- `test/regression/regression_tests.cpp`: Added `RunTrAudit023()`, updated `kTestCases` array from 22 to 23, added `RunCase` dispatch for TR-AUDIT-023

### Test Result
- `TR-AUDIT-023 PASS`: title="Hello", cover="" (oversized cover silently skipped, title parsed normally)
- `TR-AUDIT-016 PASS`: Normal APE fields unaffected by the change
- All 23 regression tests pass (22 existing + 1 new)
- LSP diagnostics clean on both changed files

---

# AD-002 Learnings

## Optional Diagnostics Channel for Parser Errors

### Architecture Finding
`ReadMetadata()` 和 `ReadLyrics()` 的 catch 块静默吞掉所有解析异常，无任何方式观测内部解析错误。对于调试和监控场景，需要可选的诊断输出通道。

### Fix
在 `ReadContext` 结构体中添加 `std::ostream *diagnostics = nullptr` 成员（`<iosfwd>` 前向声明），默认 nullptr 保持现有行为不变。

在 `ReadMetadata()` 的 `ignoreMalformedMetadata` lambda catch 块中：
- `std::filesystem::filesystem_error` → 写入 diagnostics
- `std::runtime_error` → 写入 diagnostics（在 `IsCoverExportOrCacheError` rethrow 检查之前）

在 `ReadLyrics()` 的 catch 块中：
- `std::filesystem::filesystem_error` → 写入 diagnostics 后重置 lyrics 为 {}
- `std::runtime_error` → 同上

诊断格式：
```
parser metadata error: <exception->what()>
parser lyrics error: <exception->what()>
```

### Code Changes
- `src/core/ReadContext.hpp`: 添加 `#include <iosfwd>`，添加 `std::ostream *diagnostics = nullptr` 成员
- `src/core/TagPipeline.cpp`: `ReadMetadata()` 两个 catch 块各添加 4 行 diagnostics 写入，`ReadLyrics()` 两个 catch 块各添加 4 行（同时将未捕获的 exception param 改为命名捕获 `&ex`）

### Test Result
- 全部 23 个回归测试通过（diagnostics=nullptr 默认行为零变更）
- 普通构建 + sanitizer 构建均零错误
- 无新增回归测试（默认 nullptr 行为不变）

---

# LOW-003 Learnings

## TR-AUDIT-024: Tightened LooksLikeUtf16WithoutBom() Heuristic Threshold

### Vulnerability
`LooksLikeUtf16WithoutBom()` used a 3:2 threshold (`expectedNuls * 3 >= units * 2`, i.e. ratio ≥ 0.667) to decide whether byte data looks like UTF-16 without BOM. This low threshold could produce false positives on ASCII-heavy data with coincidental paired low/high byte patterns, misidentifying Latin-1 text as UTF-16BE.

### Fix
**`TextCodec.cpp` L113-114**: Changed threshold from `expectedNuls * 3 < units * 2` to `expectedNuls * 4 < units * 3`:
```cpp
// Threshold tightened from 3:2 to 4:3 to reduce false positives on ASCII-heavy data
if (expectedNuls * 4 < units * 3)
```
The new threshold requires ≥75% NUL-byte ratio (vs. old ≥67%). The `4:3` ratio was chosen because it catches data with 10 expected NULs out of 15 units (0.667 ratio) — a false positive vector for the old threshold.

Updated the function-level comment to document the current threshold value (4:3 ≈ 75%) and its purpose.

### Test Design
`RunTrAudit024()` constructs an ID3v1 tag (which exercises `DecodeRawText → DetectTextEncoding → LooksLikeUtf16WithoutBom`) with a 30-byte title field containing:
- 10 pairs of [0x00, ASCII letter] (null-high-byte pairs for BE detection: expectedNuls=10)
- 5 pairs of [0x80, 0x81] (break UTF-8 validity, forcing heuristic evaluation)

For BE detection: expectedNuls=10, units=15, ratio=0.667.
- Old threshold (3:2): 10*3=30 ≥ 15*2=30 → PASSES (false UTF-16BE positive)
- New threshold (4:3): 10*4=40 < 15*3=45 → FAILS (correctly rejected)

Under new threshold, the data falls through to Latin-1 decoding, producing control characters (U+0080/U+0081). Under old threshold, false UTF-16BE detection would produce CJK characters (U+8081 → UTF-8 E8 82 81).

The test asserts:
- Title is non-empty (encoding detection succeeded)
- Title does not contain UTF-8 byte sequence `E8 82 81` (no CJK garbage from false UTF-16BE)

### Code Changes
- `src/text/TextCodec.cpp` L58-61: Updated heuristic documentation comment
- `src/text/TextCodec.cpp` L113-114: Threshold `3:2` → `4:3`
- `test/regression/regression_tests.cpp`: Added `RunTrAudit024()`, updated `kTestCases` array from 23 to 24, added `RunCase` dispatch for TR-AUDIT-024

### Test Result
- `TR-AUDIT-024 PASS`: title="A B C D E F G H I J..." (Latin-1 decoding, no CJK garbage)
- `TR-AUDIT-007 PASS`: Normal UTF-16 detection unaffected
- All 24 regression tests pass (23 existing + 1 new)
- LSP diagnostics clean on both changed files

---

# AD-003 + LOW-005 Learnings

## Shared Parse Helpers Extraction + ParseUInt16 Unification

### Architecture Finding
`ParseUInt16`, `ParseSlashNumber`, `ParseYearOnly`, `ToLower`, and `IEquals` had duplicated local implementations across 9 source files. The `ParseUInt16` implementations diverged in strictness: the ID3 version required `consumed == trimmed.size()` (strict: all characters must be consumed), while the APE and Vorbis versions only checked `consumed == 0` (lenient: at least one digit consumed). This inconsistency meant track numbers like "5 abc" would be parsed as 5 in APE/Vorbis but rejected (0) in ID3 — a divergence TR-AUDIT-025 now verifies is eliminated.

### Fix
Created `src/common/ParseHelpers.hpp` (header-only, namespace `tagreader_common`) with 5 unified inline functions:

- **`ParseUInt16`**: Unified to ID3 strict behavior (TrimText + `consumed == trimmed.size()`)
- **`ParseSlashNumber`**: Unified to ID3 behavior (TrimText on both sides, empty/zero rejection)
- **`ParseYearOnly`**: String-view based, manual whitespace trimming (all implementations were already identical)
- **`ToLower`**: Value-by-value, consistent `std::tolower` via `std::transform` (all identical)
- **`IEquals`**: Case-insensitive equality via `std::equal` + `std::tolower` (was only in APE)

Removed 4× `ParseUInt16` (ID3, APE ×2, Vorbis), 3× `ParseSlashNumber` (ID3, APE, Vorbis), 5× `ParseYearOnly` (ID3×2, APE, Vorbis, MP4), 6× `ToLower` (ID3, Vorbis, MP4, ContainerDetector, MediaInfoReader, CoverCache, TextNormalize), 1× `IEquals` (APE) — total 399 lines removed.

### Files Changed (11 total)
- **New**: `src/common/ParseHelpers.hpp` — 5 unified helper functions
- **Updated** (include + using declarations): `Id3Frames.cpp`, `ApeParser.cpp`, `VorbisCommentParser.cpp`, `Mp4Parser.cpp`, `Id3Parser.cpp`, `ContainerDetector.cpp`, `MediaInfoReader.cpp`, `CoverCache.cpp`, `TextNormalize.cpp`
- **Test**: `test/regression/regression_tests.cpp` — Added `RunTrAudit025()` with 4 sub-scenarios

### OggVorbisParser.cpp
Confirmed this file has NO local `ParseYearOnly` or `ToLower` — it delegates to `VorbisCommentParser` for metadata parsing. No changes needed.

### Test Design (TR-AUDIT-025)
Four sub-scenarios across 3 format families (MP3+APE, FLAC+Vorbis, MP3+ID3):

| Sub-scenario | Track value | Expected trackNumber | Rationale |
|---|---|---|---|
| A | "5 abc" | 0 | Non-numeric suffix → strict ParseUInt16 returns 0 |
| B | "5/10" | 5 | Valid slash parsing unchanged |
| C | "5" | 5 | Plain number → 5 |
| D | "5 " | 5 | Trailing space handled by TrimText |

### Test Result
- `TR-AUDIT-025 PASS`: All 12 file-variant combinations verified (4 sub-scenarios × 3 formats)
- All 24 existing regression tests (TR-AUDIT-001~024) continue to pass
- `cmake --build build` and `cmake --build build-sanitize` both zero errors
- LSP diagnostics clean on all 11 changed files
