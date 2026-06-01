# Architecture Overview

本轮审计从公开入口重新建立调用图，未沿用历史 bug 结论。公开 API 只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`，入口在 `src/TagReader.cpp` 中直接转发到 `tagreader_core::ReadTag()`。核心执行链为：`ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。

parser 架构是单入口 facade 加内部多格式 parser。`ReadContext` 保存路径、封面导出目录、共享 `std::ifstream input`、文件大小、FFmpeg `AVFormatContext`、音频流索引、容器探测状态和 FFmpeg 容器名。FFmpeg 负责打开容器、定位音频流、读取基础媒体信息和封面图像转码；标题、歌手、专辑、歌词、封面块由 `src/formats/*` 直接读取原始字节解析。

格式 dispatch 由 `DetectTagFormat()` 完成。它先读文件头和 ID3v1 footer：`ID3` -> ID3v2，`fLaC` -> FLAC，`OggS` -> Ogg Vorbis，offset 4 的 `ftyp` -> MP4，尾部 `TAG` -> ID3v1；如果原始字节不能识别，再回退到 FFmpeg 容器名。`ContainerFromTagFormat()` 再把 tag format 映射为面向用户的容器格式名。

数据流分三层。`RawMediaInfo` 来自 FFmpeg；`RawMetadata` 由 ID3、FLAC/Vorbis、Ogg Vorbis 或 MP4 parser 填充；`RawLyrics` 由对应歌词分支填充。`NormalizeMetadata()` 和 `NormalizeLyrics()` 在写入 `MusicTag` 前修剪空白、校验 UTF-8、排序/去重 timed lyrics，并把非法文本清空。`BuildMusicTag()` 最终把中间态复制到公开对象。

IO 模型以 `ReadRange(std::ifstream&, offset, size, maxSize)` 为公共读取 helper。它检查单次读取上限、`streamoff`/`streamsize` 可表示范围、加法溢出，然后清理 stream 状态、`seekg`、`read`，短读时返回空 vector。多数 parser 在调用前还会用 `context.fileSize` 或父 range 校验范围。

失败策略是“顶层媒体不可用则抛错，局部 tag malformed 则跳过”。`ReadMetadata()` 会吞掉 parser 抛出的 `runtime_error`，但包含 `cover export` / `cover cache` 的错误会重新抛出；`ReadLyrics()` 会吞掉 filesystem/runtime 错误并返回空歌词。这一策略降低崩溃概率，但也会隐藏部分 malformed 输入的具体 parser 状态。

# Supported Formats Matrix

| 类别 | 实际支持 | 主要代码路径 | 备注 |
|---|---|---|---|
| 容器 | MP3/ID3、FLAC、Ogg Vorbis、MP4/M4A | `src/media/ContainerDetector.cpp`、`src/media/MediaInfoReader.cpp` | 容器名优先由 raw bytes 推导，失败时回退 FFmpeg probe 名称 |
| ID3 metadata | ID3v1、ID3v1.1、ID3v2.2、ID3v2.3、ID3v2.4 | `src/formats/id3/Id3Parser.cpp`、`src/formats/id3/Id3Frames.cpp` | 支持常见文本帧、track/disc/year/genre 映射；跳过压缩/加密帧 |
| FLAC metadata | FLAC metadata block、Vorbis Comment、PICTURE | `src/formats/flac/FlacParser.cpp` | 读取 block type 4 和 6 |
| Ogg metadata | Ogg page + Vorbis comment packet | `src/formats/ogg-vorbis/OggVorbisParser.cpp` | 按 page、segment table、packet lacing 拼包 |
| MP4 metadata | `moov/udta/meta/ilst` item、`data` atom | `src/formats/mp4/Mp4AtomReader.cpp`、`src/formats/mp4/Mp4Parser.cpp` | 支持 `©nam`、`©ART`、`aART`、`©alb`、`©wrt`、`©gen`、`©day`/`date`、`trkn`、`disk`、`covr` |
| 歌词 | ID3 `USLT`/`SYLT`/`TXXX`、ID3v2.2 `ULT`/`SLT`、Vorbis `lyrics`/`unsyncedlyrics`/`syncedlyrics`、MP4 `©lyr`、iTunes freeform lyrics、LRC/plain text | `Id3Frames.cpp`、`VorbisCommentParser.cpp`、`Mp4Parser.cpp`、`TextNormalize.cpp` | SYLT 只接受毫秒时间戳格式；LRC timestamp 支持 `mm:ss[.mmm]` |
| 图片 | ID3v2.2 `PIC`、ID3v2.3/2.4 `APIC`、FLAC `PICTURE`、MP4 `covr` | `Id3Frames.cpp`、`FlacParser.cpp`、`Mp4Parser.cpp`、`CoverCache.cpp`、`CoverDecoder.cpp` | 只导出 front cover type 3；导出统一 PNG |
| 编码 | Latin-1、UTF-8、UTF-16 with BOM、UTF-16BE、UTF-16LE、可选 iconv legacy encoding sniff | `src/text/TextCodec.cpp` | 最终公开文本字段必须通过 UTF-8 校验 |

# Critical Risk Areas

最危险 parser 是 MP4 atom walker、Ogg Vorbis page/packet 状态机、ID3v2 frame walker、封面解码/缓存路径。它们共同特点是直接消费攻击者控制的长度字段、offset、segment table 或图片 payload。

最危险函数包括：`ForEachMp4ChildAtom()`、`FindNextMp4SiblingAfterSizeZero()`、`ReadMp4ItemAtom()`、`ReadOggVorbisCommentEntries()`、`ForEachVorbisCommentEntry()` / `ForEachFlacVorbisCommentEntry()`、`ReadId3TagBytes()`、`ReadID3v23Or24Frames()`、`PrepareId3v24FrameRegion()`、`WriteCoverAsPng()`、`ConvertImageToPng()`、`ReadLyricsFromPlainText()`。

递归风险总体较低：MP4 atom traversal 用显式 stack 而不是递归，并有 `kMaxMp4Atoms = 100000`；其它 parser 基本是线性循环。但 MP4 `size==0` 的恢复扫描会在父 range 内逐字节寻找“看起来像 atom header”的位置，属于 fuzzing 下最容易制造 CPU/I/O 放大的点。

最容易 fuzz 出问题的位置是 MP4 atom size/extended size/size-zero 组合、Ogg 大量不同 serial 的页面序列、Vorbis comment 中伪造超大 `commentCount`、多张大封面 `covr`/`PICTURE`、ID3v2 malformed frame 截断后续解析、ID3v2.4 extended header/footer/unsync/frame flags 组合、LRC 超长数字时间戳，以及 UTF-16 BOM/odd length/surrogate 边界。

# Bug Report

## TR-AUDIT-001

### 风险等级

High

### 问题类型

DoS / Spec violation / Parser recovery bug

### 位置

`src/formats/mp4/Mp4AtomReader.hpp:104`，`src/formats/mp4/Mp4AtomReader.cpp:134`，函数 `ForEachMp4ChildAtom()`、`FindNextMp4SiblingAfterSizeZero()`。

### 触发条件

攻击者构造 MP4/M4A，在 `moov/udta/meta/ilst` 可达路径或其父 range 内放置 `atomSize == 0` 的 atom，并在后续 payload 中填入大量近似 atom header 的字节或让下一个可识别 sibling 很远。

### 根本原因

ISO BMFF 中 size 0 通常表示 box 延伸到当前容器或文件末尾。当前实现允许 child walker 遇到 size 0 后调用 `FindNextMp4SiblingAfterSizeZero()` 从 payload offset 开始逐字节扫描到父 limit，试图恢复下一个 sibling。该恢复逻辑既改变了 size 0 的规范语义，也把 payload 字节当作潜在 sibling header 搜索。

### 实际风险

单个 malformed MP4 可以触发大范围逐字节扫描，造成 CPU/I/O 放大；也可能把合法 size-zero box 的 payload 误识别为 sibling atom，导致 parser 状态偏移并读取错误 metadata/lyrics。当前 `kMaxMp4Atoms` 限制的是 atom 数量，不限制恢复扫描的字节数和失败前的 header 读取次数。

### 是否可被 fuzzing 命中

是。libFuzzer 很容易通过修改 32-bit atom size 为 0、改变 payload 中 4 字节 type 字段来覆盖该路径。

### 修复建议

不要对 size 0 atom 做逐字节 sibling 恢复。对非顶层或不允许 size 0 的上下文直接返回 malformed；对允许 size 0 的顶层 box 将其 `atomEnd` 固定为当前 limit，并停止扫描 sibling。若确实需要容错恢复，应设置很小的扫描预算并只在明确的 top-level recovery 模式启用。

## TR-AUDIT-002

### 风险等级

Medium

### 问题类型

DoS / Parser state performance bug

### 位置

`src/formats/ogg-vorbis/OggVorbisParser.cpp:88`，`src/formats/ogg-vorbis/OggVorbisParser.cpp:138`，函数 `FindState()`、`ReadOggVorbisCommentEntries()`。

### 触发条件

攻击者构造 Ogg 文件，包含大量页面，每页使用不同 logical stream serial，且保持总扫描字节低于 `kMaxOggScannedBytes`、页面数低于 `kMaxOggPages`。

### 根本原因

每个页面都会调用 `FindState(states, serial)`，而 `states` 是 vector，查找使用 `std::find_if` 线性扫描。不同 serial 数随页面数增长时，总复杂度接近 O(n²)。

### 实际风险

解析一个体积不大的 Ogg 文件也可能消耗显著 CPU。虽然有 64MiB 扫描上限和 100000 页上限，但攻击者可以用小页面把 state 数量推高，形成 fuzzing 下稳定的慢路径。

### 是否可被 fuzzing 命中

是。生成多个 `OggS` page，并递增/随机 serial 即可触发。

### 修复建议

用 `std::unordered_map<std::uint32_t, VorbisStreamState>` 替代 vector 线性查找，或限制并发 logical stream serial 数，例如超过 64/256 个 serial 后停止解析 comment packet。

## TR-AUDIT-003

### 风险等级

Medium

### 问题类型

DoS / Length field trust issue

### 位置

`src/formats/flac/FlacParser.cpp:68`，`src/formats/ogg-vorbis/OggVorbisParser.cpp:65`，函数 `ForEachFlacVorbisCommentEntry()`、`ForEachVorbisCommentEntry()`。

### 触发条件

Vorbis Comment block/packet 声明极大的 32-bit `commentCount`，但实际 payload 很短或只包含少量空 comment length 字段。

### 根本原因

parser 直接按 `commentCount` 循环，只有在 `size - cursor < 4` 或 `commentLength > size - cursor` 时才失败。没有根据剩余字节推导最大可能 entry 数，也没有设置 comment 数量上限。

### 实际风险

恶意 FLAC/Ogg 文件可以制造大量循环检查。每次循环本身只做少量操作，但在 fuzzing 和批量扫描场景中会形成 CPU 放大；在 FLAC metadata 分支还会最终抛出 malformed block，被 `ReadMetadata()` 吞掉，调用方只能看到字段缺失，难以定位慢输入。

### 是否可被 fuzzing 命中

是。把 `commentCount` 置为接近 `UINT32_MAX`，同时控制 block size 让循环尽量多次读取长度即可。

### 修复建议

增加 `kMaxVorbisComments`，例如 4096 或更低；循环前用 `remaining / 4` 作为理论上限快速拒绝明显不可能的 count；对空 comment 或超多 comment 的情况提前返回 false。

## TR-AUDIT-004

### 风险等级

Medium

### 问题类型

Resource exhaustion / Redundant large read

### 位置

`src/formats/mp4/Mp4Parser.cpp:253`，`src/formats/mp4/Mp4Parser.cpp:261`，`src/formats/mp4/Mp4Parser.cpp:233`，函数 `ReadMp4ItemAtom()`、`ReadMp4DataAtom()`。

### 触发条件

MP4 `ilst` 中包含多个 `covr` item，每个 `data` atom payload 接近 `kMaxCoverInputBytes`。第一张封面已经成功设置 `metadata.coverPath`，后续 `covr` 仍会进入 item child traversal。

### 根本原因

`ReadMp4ItemAtom()` 总是先按 `Mp4MetadataPayloadLimit(atomType)` 读取整个 `data` atom payload；`ReadMp4DataAtom()` 到 `atomType == "covr"` 分支后才检查 `!metadata.coverPath.empty()` 并返回。因此已拥有封面时，后续大 `covr` payload 仍会被读入内存。

### 实际风险

单个文件可以导致重复 64MiB 级别读取和分配。由于 `WalkMp4IlstItems()` 还有 atom 数上限，这不是无限 OOM，但对批量扫描器是明显的内存带宽和延迟放大。

### 是否可被 fuzzing 命中

是。构造多个 `covr/data` atom 并让第一个成功解码即可观察后续 payload 仍被读取。

### 修复建议

在 `ReadMp4ItemAtom()` 进入 child atom 前判断 `atomType == "covr" && !metadata.coverPath.empty()`，直接跳过整个 item；或者在 lambda 看到 `data` 前也先判断 cover 已存在，不调用 `ReadMp4AtomPayload()`。

## TR-AUDIT-005

### 风险等级

Medium

### 问题类型

Resource exhaustion / Redundant cover decode

### 位置

`src/formats/flac/FlacParser.cpp:212`，`src/formats/flac/FlacParser.cpp:224`，`src/formats/flac/FlacParser.cpp:147`，函数 `ReadFlacMetadataBlocks()`、`ReadFlacPictureEntry()`。

### 触发条件

FLAC 文件包含多个 type 6 PICTURE metadata block，每个 block 都是 front cover type 3 且图片数据接近 64MiB。

### 根本原因

FLAC picture path 没有在读取 block 或调用 `WriteCoverAsPng()` 前判断 `metadata.coverPath` 是否已设置。与 MP4 类似，多张封面会重复读取、解析和尝试解码，即使第一张封面已经足够填充 public result。

### 实际风险

恶意 FLAC 可以造成重复大 block 读取和 FFmpeg 图片解码/转码尝试。输入、像素、输出都有限制，因此主要是 CPU/内存带宽 DoS，而非越界读写。

### 是否可被 fuzzing 命中

是。生成多个合法 PICTURE block，第一张设置 coverPath，后续仍可触发重复工作。

### 修复建议

当 `metadata.coverPath` 已非空时跳过后续 PICTURE block 的 payload 读取和 `ReadFlacPictureEntry()`；在 `ReadFlacPictureEntry()` 开头也增加同样的快速返回。

## TR-AUDIT-006

### 风险等级

Medium

### 问题类型

Logic bug / Parser recovery issue / Metadata hiding

### 位置

`src/formats/id3/Id3Frames.cpp:820`，`src/formats/id3/Id3Frames.cpp:854`，`src/formats/id3/Id3Frames.cpp:1171`，`src/formats/id3/Id3Frames.cpp:1283`，函数 `ReadID3v22Frames()`、`ReadID3v23Or24Frames()`、`ReadID3v22LyricsFrames()`、`ReadID3v23Or24LyricsFrames()`。

### 触发条件

ID3v2 tag 在前部放置一个畸形 frame header，例如非法 frame id、`frameSize == 0`、frame size 超过剩余 tag、非法 v2.4 syncsafe frame size，后面再放置合法 `TIT2`/`TPE1`/`USLT`/`APIC` 等 frame。

### 根本原因

frame walker 遇到第一个异常 frame 时普遍使用 `break` 终止整个 tag 扫描，而不是把该区域标记为 padding/malformed 后尝试继续扫描。该策略对真正 padding 是安全的，但对恶意插入的畸形 frame 过于脆弱。

### 实际风险

攻击者可以稳定隐藏后续 metadata、歌词或封面，使 `Read()` 返回缺字段的 `MusicTag`。这不是越界读写，但属于 parser recovery 和数据完整性问题；在批量媒体库扫描场景中会造成可控的元数据截断。

### 是否可被 fuzzing 命中

是。构造合法 ID3 header，在第一个 frame 处变异 frame id 或 frame size，再在后面放合法 frame 即可观察字段丢失。

### 修复建议

区分明确 padding 与 malformed frame。对 padding 仍可停止；对 malformed frame 建议记录诊断并尝试同步到下一个可信 frame header，或至少只丢弃当前 tag 的问题区域而非无条件吞掉后续所有 frame。新增 characterization 测试覆盖“坏 frame 后跟合法 frame”。

## TR-AUDIT-007

### 风险等级

Low

### 问题类型

Integer parsing bug / Logic bug

### 位置

`src/formats/id3/Id3Frames.cpp:44`，`src/formats/id3/Id3Frames.cpp:113`，`src/formats/id3/Id3Frames.cpp:985`，`src/formats/id3/Id3Frames.cpp:1088`，函数 `ParseUInt16()`、`ParseSlashNumber()`、`ReadID3v22Frame()`、`ReadID3v2Frame()`。

### 触发条件

ID3 `TRK`/`TPA`/`TRCK`/`TPOS` 文本帧包含数字前缀加垃圾后缀，例如 `12abc/7`、`003x/01`。

### 根本原因

`ParseUInt16()` 使用 `std::stoul()` 后只检查 `consumed == 0`，没有要求 `consumed == value.size()`；因此 C++ 标准库会接受数字前缀并忽略后续非法字符。

### 实际风险

损坏或恶意 tag 可以把非规范 track/disc 字段解析成合法数字，影响排序和展示。该问题不会导致内存安全风险，但会污染最终 `MusicTag` 的数值字段。

### 是否可被 fuzzing 命中

是。对 track/disc 文本字段追加任意非数字字符即可触发。

### 修复建议

`ParseUInt16()` 应要求完整消费输入：`consumed == value.size()`。同时在 `ParseSlashNumber()` 中对 slash 两侧分别 trim 后严格解析，拒绝空侧或含垃圾后缀的数值。

## TR-AUDIT-008

### 风险等级

Medium

### 问题类型

Integer overflow / Invalid timestamp handling

### 位置

`src/text/TextNormalize.cpp:39`，`src/text/TextNormalize.cpp:98`，`src/text/TextNormalize.cpp:230`，函数 `ParseDecimalU16Strict()`、`ParseLrcTimestamp()`、`ReadLyricsFromPlainText()`。

### 触发条件

ID3 `USLT`/`TXXX`/`ULT` 或 Vorbis/MP4 plain lyrics 中包含超长 LRC 时间戳数字，例如 `[999999999999999999999999:01.00]text`。

### 根本原因

`ParseDecimalU16Strict()` 用 `uint32_t result = result * 10 + digit` 累乘，但没有在乘法和加法前检查 `uint32_t` 溢出；溢出后再检查 `result > uint16_t::max()` 已经太晚。

### 实际风险

长数字 token 可在 `uint32_t` 中回绕，导致非法时间戳被错误接受或误判，污染 timed lyrics 的时间轴。由于歌词文本来自音频文件内嵌 tag，该问题可由 malformed 媒体文件触发。

### 是否可被 fuzzing 命中

是。LRC parser 对 `[` 开头的行会逐个解析 timestamp，长数字串很容易覆盖该路径。

### 修复建议

在乘加前检查：`result > (max - digit) / 10` 时立即失败；也可以在进入解析前限制分钟/秒/毫秒字段长度，例如 minutes 最多 5 位，seconds 固定 1-2 位，fraction 最多 3 位。

## TR-AUDIT-009

### 风险等级

Low

### 问题类型

Spec violation / Parser state-machine bug

### 位置

`src/formats/ogg-vorbis/OggVorbisParser.cpp:100`，`src/formats/ogg-vorbis/OggVorbisParser.cpp:236`，函数 `HasVorbisPrefix()`、`ReadOggVorbisCommentEntries()`。

### 触发条件

Ogg packet 只伪造 7 字节 `0x01 "vorbis"` identification prefix 或 `0x03 "vorbis"` comment prefix，但后续 identification/comment packet 必需字段缺失或结构不完整。

### 根本原因

状态机仅凭 7 字节前缀推进 `LookingForIdentification` -> `LookingForComment` -> parse comment，没有验证 Vorbis identification packet 的最小长度和必要字段，也没有在 comment packet 前确认该 logical stream 真的是完整 Vorbis stream。

### 实际风险

畸形 Ogg 数据可被误判为 Vorbis stream，导致错误 metadata 抽取或消耗 comment parser 资源。该问题主要是规范兼容和状态机鲁棒性问题，不是直接越界。

### 是否可被 fuzzing 命中

是。最小 packet 只需前缀匹配即可推进状态机。

### 修复建议

验证 identification packet 的最小长度和关键字段，例如 Vorbis version、channels、sample rate、framing flag 等；只有完整 identification packet 通过后才接受后续 comment packet。对 comment packet 也应做最小结构校验后再交给 Vorbis Comment walker。

## TR-AUDIT-010

### 风险等级

Low

### 问题类型

Invalid UTF handling / Compatibility bug

### 位置

`src/text/TextCodec.cpp:515`，`src/text/TextCodec.cpp:243`，函数 `ReadUtf16TextWithBom()`、`TryReadUtf16Text()`。

### 触发条件

ID3 encoding byte `0x01` 表示 “UTF-16 with BOM”，但 payload 实际缺少 BOM。

### 根本原因

`ReadUtf16TextWithBom()` 在未发现 BOM 时默认调用 `ReadUtf16Text(data, size, false)`，也就是静默按 little-endian 解析。对需要 BOM 的格式来说，这会把本应拒绝的 BOM-less 字节流纳入规范化路径。

### 实际风险

恶意或损坏 tag 可造成文本误解码、字段污染或跨平台结果不一致。该问题主要是编码严格性和规范兼容问题。

### 是否可被 fuzzing 命中

是。构造 encoding byte 为 1、payload 为偶数字节但无 BOM 的文本帧即可触发。

### 修复建议

对 `encoding == 1` 的 ID3 路径要求 BOM 存在；若为了兼容历史坏样本保留默认 little-endian，应作为显式 legacy 模式并在报告/诊断中标记为非规范输入。

## TR-AUDIT-011

### 风险等级

Low

### 问题类型

Malformed image handling / Unchecked library return

### 位置

`src/cover/CoverDecoder.cpp:273`，函数 `ConvertImageToPng()`。

### 触发条件

FFmpeg 解码出尺寸合法但像素格式/linesize/sws 转换异常的图片帧，`sws_scale()` 返回值小于目标高度或负数。

### 根本原因

代码调用 `sws_scale()` 后未检查返回的输出行数，随后直接把 `rgbFrame` 交给 PNG encoder。

### 实际风险

通常会表现为封面转码失败、输出空或生成错误 PNG。在异常路径上可能编码未完整转换的 frame 内容，造成 nondeterministic cache bytes。由于 `av_frame_get_buffer()` 分配的 frame buffer 通常由 FFmpeg 管理，且最终还要通过 PNG 格式校验和 64MiB 输出限制，本项不应夸大为直接内存越界。

### 是否可被 fuzzing 命中

可能。图片 fuzz corpus 可通过畸形 BMP/TIFF/WebP/GIF 帧覆盖 swscale 异常返回。

### 修复建议

检查 `const int scaledRows = sws_scale(...)`，要求 `scaledRows == decodedFrame->height`；否则返回空封面。必要时在 `av_frame_get_buffer()` 后清零目标 buffer，避免部分转换时产生不稳定输出。

## TR-AUDIT-012

### 风险等级

Low

### 问题类型

Spec violation / Compatibility bug

### 位置

`src/formats/id3/Id3Frames.cpp:390`，`src/formats/id3/Id3Frames.cpp:393`，`src/formats/id3/Id3Frames.cpp:405`，函数 `PrepareId3v24FrameRegion()`。

### 触发条件

ID3v2.4 tag 设置 extended header flag，并使用规范常见的 extended header size 语义，即 extended header size 字段描述整个 extended header 大小。

### 根本原因

当前代码注释和实现把 v2.4 extended header size 当作“不含 4 字节 size 字段”，使用 `TryAddSize(4, extSize, extendedEnd)` 计算 frame 起点。若实际文件按“size 包含自身”编码，则 cursor 会多跳 4 字节，导致第一个 frame 被错过或 frame header 错位。

### 实际风险

主要是合法 ID3v2.4 文件兼容性问题，可能导致 metadata/lyrics/cover 缺失。因为 tag bytes 已整体读入且 cursor/limit 仍受 vector 边界约束，当前未看到越界读取证据。

### 是否可被 fuzzing 命中

是。构造 v2.4 extended header 的最小 tag 并在 extended header 后立即放置 `TIT2`/`APIC` 可触发解析结果差异。

### 修复建议

用官方 ID3v2.4.0 extended header 规范重新确认 size 字段语义，并加入包含最小 extended header 的 characterization corpus。实现应按规范计算 cursor，并对非规范变体只做显式容错，不应混入主路径。

## TR-AUDIT-013

### 风险等级

Low

### 问题类型

File trust issue / Cache DoS / Hardening gap

### 位置

`src/cover/CoverCache.cpp:50`，`src/cover/CoverCache.cpp:72`，`src/cover/CoverCache.cpp:210`，`src/cover/CoverCache.cpp:314`，函数 `HashEmbeddedImageBytes()`、`BuildCoverCachePath()`、`ValidateExistingCoverCacheFile()`、`AtomicWriteFileIfAbsent()`。

### 触发条件

调用方把多个互不信任来源的音频文件导出到同一个可写 `coverExportDir`；攻击者能构造内嵌图片字节并尝试制造 FNV hash collision，或能控制/替换导出目录下的 shard 路径。

### 根本原因

缓存 key 是自定义双 FNV 风格非加密 hash，不适合作为安全身份。代码对最终 PNG 文件做了 `symlink_status`、`O_NOFOLLOW`、`fstat` 和逐字节验证，因此 collision 不会静默复用错误封面；但 collision 或预置不匹配文件会触发 `cover cache` 错误并中断 `Read()`。目录层级本身没有逐级 `openat`/`O_NOFOLLOW` 约束，仍依赖调用方提供可信导出目录。

### 实际风险

主要是共享 cache 下的错误注入/拒绝服务，而不是错误封面静默返回。攻击者若能写 cache 目录，可以预置冲突路径或替换目录结构，让受害者读取封面时抛错。

### 是否可被 fuzzing 命中

普通文件内容 fuzz 不容易命中 hash collision；带文件系统模型的集成测试可以覆盖预置污染 cache、symlink、目录替换等场景。

### 修复建议

使用 SHA-256/BLAKE3 等加密 hash 作为 content address；把 cache 错误策略区分为“安全污染”与“一般封面失败”，必要时只跳过封面而不让整个 `Read()` 失败；对目录创建/打开使用 `openat` 风格逐级校验，明确文档化 `coverExportDir` 必须是调用方可信目录。

## TR-AUDIT-014

### 风险等级

Low

### 问题类型

Resource amplification / Performance issue

### 位置

`src/cover/CoverDecoder.cpp:110`，`src/cover/CoverDecoder.cpp:123`，`src/cover/CoverDecoder.cpp:128`，`src/cover/CoverDecoder.cpp:218`，`src/cover/CoverDecoder.cpp:229`，`src/cover/CoverDecoder.cpp:233`，函数 `ReadImageBytes()`、`ConvertImageToPng()`。

### 触发条件

内嵌封面接近 `maxInputBytes`，且格式识别失败时走 fallback codec 列表，多次尝试解码。

### 根本原因

`ReadImageBytes()` 先 `av_new_packet()` 复制输入，再从 packet 复制到 vector；`ConvertImageToPng()` 又创建新的 packet 并再次复制。fallback 分支会对多个 codec 重复该过程。

### 实际风险

攻击者可用单张 64MiB 封面制造多次大内存复制和解码尝试。限制存在，因此不是无限 OOM，但在批量扫描和 fuzzing 中会显著拖慢执行。

### 是否可被 fuzzing 命中

是，特别是封面头部未知或畸形但尺寸接近上限的样本。

### 修复建议

移除 `ReadImageBytes()` 的中间 vector/packet 双重复制，直接为 decoder packet 分配一次；fallback 前先做更严格 magic/type 筛选，或限制 fallback 总尝试次数和总解码预算。

## TR-AUDIT-015

### 风险等级

Low

### 问题类型

API design defect / Hidden global state risk

### 位置

`include/Tag.hpp:15`，`include/Tag.hpp:18` 至 `include/Tag.hpp:40`，类 `MusicTag`。

### 触发条件

多线程同时调用 `TagReader::Read()`，并频繁写入不同 metadata 字符串到 `MusicTag` 的 `boost::flyweight<std::string>` 字段。

### 根本原因

公开返回对象使用 `boost::flyweight`，这通常意味着进程级 intern pool 和锁策略。当前代码没有在 API 文档中说明 `MusicTag` flyweight 的线程安全假设，也没有把字符串字段保持为普通 `std::string`。

### 实际风险

如果 Boost flyweight 配置或平台锁策略不满足并发写入假设，`Read()` 的内部 parser 虽然基本使用局部状态，最终写入 `MusicTag` 时仍可能进入隐藏共享状态。现有 `TagReaderSecuritySmoke` 会并发读取封面路径，但不是专门验证 flyweight 大量不同字符串并发插入。

### 是否可被 fuzzing 命中

单线程 libFuzzer 不容易命中；ThreadSanitizer 或并发压力测试更适合。

### 修复建议

确认并文档化 Boost flyweight 默认 locking policy；若库目标是线程安全读取，建议 public data holder 改为普通 `std::string`，或显式选择带锁 factory/locking policy 并加入 TSan 并发测试。

# Structural Weakness

架构上最大的优点是 God Translation Unit 已拆成 core/media/io/text/cover/formats，各 parser 边界清晰，`ReadContext` 集中保存共享状态，`ReadRange()` 统一处理 seek/read 失败。多数长度字段有显式上限，MP4 traversal 也避免了递归调用栈风险。

主要结构弱点是 `ReadContext` 仍是共享可变对象，所有 parser 复用同一个 `std::ifstream`。`ReadRange()` 会反复 `clear()` 并移动 stream 位置，当前 parser 大多用绝对 offset，状态污染风险可控；但未来若加入相对读取 parser，很容易被前一个 parser 的 seek 状态影响。建议把二进制读取进一步封装为无状态 random-access reader，禁止 parser 直接持有/操作 stream。

第二个弱点是 parser 错误被大量吞掉。`ReadMetadata()` 和 `ReadLyrics()` 的局部失败策略符合设计，但会让安全测试难以区分“格式合法但字段缺失”和“parser 遇到 malformed 后放弃”。建议内部增加可选 diagnostics 或 debug trace，不改变 public API，但能在 fuzz/minimization 中暴露失败原因。

第三个弱点是重复逻辑较多。ID3、Vorbis、MP4 各自有 `ParseYearOnly()` / `ParseUInt16()` / `ParseSlashNumber()`；FLAC 和 Ogg 各自实现 Vorbis Comment entry walker。重复实现增加规范分歧风险，例如 comment count 上限和 malformed recovery 应该共享同一 helper。

第四个弱点是资源预算分散。ID3 tag、MP4 payload、Ogg packet、lyrics、cover input/output 都有上限，但没有全局 per-file CPU/I/O/decoded bytes budget。攻击者可以组合多个 bounded 路径形成整体慢输入，例如 MP4 双 pass traversal + 多 covr + cover fallback decode。

API 层面，`TagReader::Read()` 会先要求 FFmpeg 成功打开并找到音频流。它不是“任意 tag 文件解析器”，而是“音频文件标签读取器”。这符合项目定位，但需要文档明确：无有效音频 stream 的 tag-only/fuzz 样本会在 parser 前失败。

# Spec Compliance Issues

ID3v2.2/2.3/2.4：当前实现校验 ID3 tag size 的 syncsafe 字节，并跳过压缩/加密帧；v2.4 frame size 使用 syncsafe，v2.3 frame size 使用普通 BE32。兼容风险在 v2.4 extended header size 语义：代码按“不含 size 字段”处理，可能与常见规范解释不一致。ID3v2.3/v2.4 的更多 frame flags、CRC、restrictions、data length indicator 的完整语义也只做跳过/简化支持，不是完整规范实现。文本编码上，`encoding == 1` 代表 UTF-16 with BOM，当前缺 BOM 时默认小端属于宽松兼容路径。

FLAC metadata spec：block header 的 24-bit length 和 last-block bit 基本按规范处理；PICTURE block 使用 bounded cursor 读取 picture type、MIME、description、尺寸字段和 data length。兼容缺口是没有校验 STREAMINFO 必须是第一个 metadata block，也没有验证 PICTURE 的 MIME/type 与实际图片 magic 一致；这些目前主要影响严格规范合规，不直接导致越界。

Vorbis Comment spec：vendor length、comment count、comment length 都按 little-endian 读取，并将 entry 解析为 `key=value`。缺口是没有 comment 数量上限，也没有用剩余字节推导最大 entry 数；key 只通过 UTF-8 normalize 后 lower-case，没有严格限制 Vorbis field name 的 ASCII 可打印字符集合。

Ogg Vorbis spec：page capture pattern、version、segment table、continuation 和 packet lacing 有基础状态机；只在 packet 闭合后识别 Vorbis identification/comment packet。缺口是 sequence number 使用 `expectedSequence + 1`，对 32-bit wrap 没有特殊处理；多 logical stream serial 使用线性状态表，不适合恶意大量 stream；Vorbis stream 判定只看 7 字节 `vorbis` 前缀，未验证 identification packet 的完整字段。

ISO BMFF / MP4 atom spec：basic size、extended 64-bit size、parent range、payload offset 有显式检查，atom traversal 用 stack 防递归。主要 spec violation 是 size 0 recovery：size 0 不应被解释为“扫描 payload 寻找下一个 sibling”。`meta` full box 只接受 version 0，这可能拒绝未来或非典型文件，但属于保守兼容策略。

MP4 metadata conventions：`data` atom payload 前 8 字节被解释为 type/locale；文本 type 支持 UTF-8、UTF-16BE、UTF-16LE，track/disk 支持整数布局，`covr` 直接送封面解码。缺口是没有根据 `covr` data type 区分 JPEG/PNG/BMP 等，而是依赖图片 magic/fallback decoder；这提高容错性，但不是严格按 iTunes data type 做验证。

# Fuzzing Targets

最适合 fuzz 的入口仍是 `TagReader::Read(inputPath, coverDir)`，因为它覆盖 FFmpeg probe、raw parser、歌词、封面导出和 cache。已有 `test/fuzz/tagreader_fuzz.cpp` 可作为 libFuzzer 入口，但建议补充 parser-local fuzz harness，以避免 FFmpeg 在无音频流时提前拒绝导致内部 parser 覆盖不足。

推荐 parser-local fuzz 目标：

- `ReadId3TagBytes()` + `ReadID3v23Or24Frames()`：输入形状为完整 ID3 header + tag bytes，重点变异 syncsafe tag size、extended header、footer、frame flags、frame size、坏 frame 后接合法 frame、unsync、APIC/USLT/SYLT/TXXX、TRCK/TPOS 数字后缀。
- `ReadFlacMetadataBlocks()` / Vorbis Comment walker：输入形状为 `fLaC` + metadata block 链，重点变异 block length、last flag、comment count、PICTURE length。
- `ReadOggVorbisCommentEntries()`：输入形状为 Ogg page 序列，重点变异 serial、sequence、continuation、segment table、255 lacing、packet size。
- `WalkMp4IlstItems()`：输入形状为 MP4 atom tree，重点变异 atom size 0/1、extended size、parent limit、`meta` full box、`ilst/data/covr/©lyr/----`。
- `DecodeTextToUtf8()` / `ReadLyricsFromPlainText()`：输入形状为随机 bytes + encoding selector、LRC/plain text，重点变异 UTF-16 surrogate、odd length、BOM、invalid UTF-8、超多 timestamp。
- `DecodeAndEncodeCoverPng()`：输入形状为图片 bytes，重点变异 PNG/JPEG/BMP/WebP/GIF/TIFF header 和截断帧。

推荐 sanitizer：ASAN、UBSAN、TSAN。ASAN/UBSAN 用于 parser 和 cover decoder；TSAN 用于并发 `TagReader::Read()` 与 `MusicTag` flyweight 字段。建议 Clang libFuzzer 参数包括 `-fsanitize=fuzzer,address,undefined`、`-fno-omit-frame-pointer`、`-fno-sanitize-recover=all`。CMake 已有 `TAGREADER_ENABLE_SANITIZERS=ON` 和 `TAGREADER_ENABLE_FUZZING=ON` 开关，可在 Clang 下组合使用。

推荐 corpus：

- ID3v2.2/2.3/2.4 最小 tag、extended header、footer、unsync、压缩/加密 flag、APIC/PIC 截断、USLT/SYLT/TXXX。
- FLAC STREAMINFO + Vorbis Comment + PICTURE，包含 block length 截断、超大 comment count、多 PICTURE、大 MIME/description。
- Ogg Vorbis identification/comment packet 正常样本、跨 page lacing、continuation mismatch、sequence wrap、大量 serial、小 payload 多 page。
- MP4 `ftyp/moov/udta/meta/ilst` 正常样本，size 0、extended size、嵌套深度、大量 atom、多 covr、多 freeform lyrics。
- 文本样本包含 Latin-1、UTF-8、UTF-16LE/BE、BOM 错配、孤立 surrogate、超长 LRC、多 timestamp、重复 timestamp。
- 封面样本包含小 PNG/JPEG、畸形 header、截断图片、超大维度声明、fallback codec 可识别但解码失败的文件。

# Overall Security Assessment

整体安全等级：中等偏上。当前代码已经做了大量必要边界控制：统一 `ReadRange()`、ID3 tag 上限、MP4 atom/payload 上限、Ogg packet/page/scan 上限、歌词行数上限、封面输入/像素/输出上限、UTF-8 校验、封面缓存污染检测。这些措施显著降低了越界读写和直接 UB 风险。

crash 风险：中低。静态审计未发现明确可达的越界写入、裸指针越界读、无界递归或明显生命周期悬挂。多数 malformed 输入会被短读、边界检查或异常吞掉。剩余风险集中在第三方 FFmpeg 图片解码路径和复杂 parser 状态组合。

malformed 文件鲁棒性：中等。ID3、FLAC、Ogg、MP4 都有本地失败策略，但 MP4 size-zero recovery、Vorbis comment count、Ogg 多 serial、多封面重复读取说明攻击者仍可构造慢输入。局部失败被吞掉也会降低定位能力。

fuzzing 风险等级：Medium。最可能被 fuzzing 命中的不是内存破坏，而是 CPU/I/O 放大、兼容性错误、封面解码异常返回、以及 parser 状态错位。建议优先用 ASAN/UBSAN 跑现有入口 fuzz，再补 parser-local harness 提升内部格式覆盖。

当前代码质量评估：重构后的模块边界清楚，手写二进制解析总体谨慎，核心安全边界比典型媒体 tag parser 更强。下一轮修复应优先处理 MP4 size 0 语义、Ogg serial 状态表、Vorbis comment count 上限、ID3 malformed frame recovery、LRC 数字溢出、多封面重复读取/解码、`sws_scale()` 返回值检查，并为 ID3v2.4 extended header 增加规范样本确认。
