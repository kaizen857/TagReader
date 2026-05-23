# Architecture Overview

> 2026-05-23 稳定化更新：本文件保留初始静态审计结论。Phase 0-14 已完成后，实际实现已加入资源预算、content-addressed cover cache、MP4 显式栈 walker、Ogg packet/page 限制、统一 ID3 tag 读取、严格文本/LRC 解析、`DetectedContainer` dispatch、MP4 text/numeric 校验、媒体信息 clamp、malformed metadata/lyrics recovery、sanitizer/fuzzer target 和 fuzz corpus。具体执行命令、样本路径和输出摘要记录在 `TASKS.md` 的“稳定化实施验收记录”。

## 稳定化完成摘要

- 公共入口仍保持 `TagReader::Read(path)`，并新增兼容重载 `TagReader::Read(path, coverExportDir)` 用于调用方控制封面导出目录。
- metadata、lyrics、cover 继续从原始字节解析；FFmpeg 仍只用于 probe、媒体信息、封面图像 decode/PNG encode。
- P0/P1 风险已通过代码预算和样本回归覆盖：大字段读取、Ogg packet 聚合、MP4 深嵌套递归、封面 decoder 资源、临时文件/cover cache、文本输出扩张和 malformed recovery。
- Dispatch 已收敛到 `ReadContext::detectedContainer`，避免 signature 与 FFmpeg `containerName` 二次判断不一致。
- Fuzz/sanitizer 支持默认关闭；`TAGREADER_ENABLE_SANITIZERS` 和 `TAGREADER_ENABLE_FUZZING` 可用于安全回归。
- `src/TagReaderInternal.hpp` 只包含私有小类型，公共 `include/` 未暴露内部实现。

本库是单入口 facade API：用户只调用 `MusicTag TagReader::Read(const std::filesystem::path&)`，内部完成路径检查、FFmpeg probe、主音频流选择、媒体信息读取、标签读取、歌词读取、文本归一化和最终 `MusicTag` 组装。

核心调用图如下：

```text
TagReader::Read(path)
  -> ValidatePath(path)
  -> OpenContext(path)
       -> std::ifstream input.open(path)
       -> avformat_open_input / avformat_find_stream_info
  -> DetectStream(context)
       -> av_find_best_stream(... AVMEDIA_TYPE_AUDIO ...)
       -> fallback 顺序扫描音频流
  -> ReadMediaInfo(context)
       -> 消费 AVFormatContext / AVStream / AVCodecParameters
  -> ReadMetadata(context)
       -> DetectContainerFromSignature(context)
       -> MP4: ReadMP4Metadata -> ReadMP4AtomTree -> ReadMP4ItemAtom -> ReadMP4DataAtom
       -> FLAC/Ogg: ReadVorbisCommentMetadata -> ReadFlacMetadataBlocks / ReadOggVorbisComments
       -> MP3: ReadID3v2Metadata -> ReadID3v22Frames / ReadID3v23Or24Frames -> frame readers
       -> MP3: ReadID3v1Metadata
       -> NormalizeMetadata
  -> ReadLyrics(context)
       -> DetectContainerFromSignature(context)
       -> MP3: ReadID3Lyrics -> ReadID3v22LyricsFrames / ReadID3v23Or24LyricsFrames
       -> FLAC/Ogg: ReadVorbisLyrics -> ReadVorbisLyricsEntry -> ReadLyricsFromPlainText
       -> MP4: ReadMP4Lyrics -> ReadMP4LyricsAtomTree -> ReadMP4LyricsItem / ReadMP4FreeformLyricsItem
       -> NormalizeLyrics
  -> BuildMusicTag(context, mediaInfo, metadata, lyrics)
```

数据流：`ReadContext` 同时保存 `std::ifstream input`、文件大小、修改时间、FFmpeg `AVFormatContext`、容器名和音频流索引。媒体信息只走 FFmpeg；metadata、lyrics、cover 从 `input` 原始字节直接解析，最终进入 `RawMetadata` / `RawLyrics`，再归一化到 UTF-8，最后写入 `MusicTag`。

状态流：所有 parser 共享同一个 `ReadContext` 和同一个 `std::ifstream`。底层读取主要通过 `ReadRange()` 执行，每次会 `input.clear()`、`seekg()`、`read()`。少数路径直接操作 stream，例如 `ReadID3v1Metadata()` 使用 `seekg(-128, std::ios::end)`。

parser dispatch 逻辑：`ReadMetadata()` / `ReadLyrics()` 同时依赖 FFmpeg `containerName` 和 `DetectContainerFromSignature()`。但具体分支函数中仍有二次 `containerName` 检查，导致 dispatch 与实际执行条件存在耦合风险。

normalization 流程：ID3 文本帧会先按声明编码读取；Vorbis、MP4、ID3v1 等路径会调用 `DecodeRawText()` 或 `DecodeTextToUtf8()`。`NormalizeMetadata()` 和 `NormalizeLyrics()` 又会对已解析字符串再次调用 `NormalizeText()`。这保证最终字段大多是 UTF-8，但也引入重复嗅探和“无效输入被兼容回退”的语义风险。

IO 模型：所有二进制 parser 都是手写 offset/cursor parser。关键工具包括 `ReadRange()`、`TryAddUintmax()`、`ReadBE16/24/32()`、`ReadLE32()`、`ReadSyncSafe32()`、`ReadMp4AtomHeader()`。整体没有 mmap，越界写风险较低；主要风险是大分配、递归、格式长度信任、stream 状态和 malformed 输入恢复语义。

# Supported Formats Matrix

| 类别 | 实际支持情况 |
|---|---|
| 容器 | MP3/MPEG、FLAC、Ogg Vorbis、MP4/M4A/MOV 系容器；识别来自 FFmpeg `containerName` 和文件签名。 |
| Metadata | ID3v1/ID3v1.1、ID3v2.2、ID3v2.3、ID3v2.4、FLAC Vorbis Comment、Ogg Vorbis Comment、MP4 `moov/udta/meta/ilst` 常见 atom。 |
| 图片 | ID3v2.2 `PIC`、ID3v2.3/2.4 `APIC`、FLAC `PICTURE`、MP4 `covr`。只接受 front cover 类型；MP4 `covr` 无 picture type。 |
| 图片编码 | PNG 直接写出；JPEG/BMP/WEBP/GIF/TIFF 通过 FFmpeg decoder 转 PNG；未知图像会试探 WEBP/GIF/TIFF/BMP/MJPEG。 |
| 歌词 | ID3v2.2 `ULT`/`SLT`、ID3v2.3/2.4 `USLT`/`SYLT`/部分 `TXXX`、Vorbis `LYRICS` 等键、MP4 `©lyr` 和 `----:com.apple.iTunes:LYRICS`。 |
| LRC | 纯文本歌词中解析 `[mm:ss.xxx]` 风格时间戳。 |
| 编码 | ID3 声明编码 0/1/2/3、Latin-1、UTF-8、UTF-16LE/BE、BOM 嗅探、可选 iconv 本地编码候选。 |

# Critical Risk Areas

- `ReadMP4LyricsAtomTree()`：无深度上限递归，且不是 metadata 路径那种固定 depth 状态机，恶意嵌套 atom 可触发栈耗尽。
- `ReadRange()` 的调用点：MP4 `data` atom、封面 payload、ID3 tag body、FLAC block、Ogg packet 都可能由文件长度字段驱动分配。
- `ReadOggVorbisCommentEntries()`：跨页 packet 使用 `packet.insert()` 累积，缺少 packet 总大小上限，连续 `255` lacing 可制造内存 DoS。
- `ReadID3v2Metadata()` / `ReadID3Lyrics()`：最大 syncsafe tag body 可接近 256 MiB，直接读入 vector，适合 fuzz / DoS。
- `ConvertTextWithIconv()` / `ReadLatin1Text()`：输出缓冲按输入成倍扩张，缺少全局文本长度上限和 overflow guard。
- `WriteCoverAsPng()` / `ConvertImageToPng()`：会把嵌入图片交给 FFmpeg 图像 decoder，缺少自定义像素数、输出 PNG 大小、CPU 时间上限。
- `MakeCoverPathForAudioFile()` / `WriteBinaryFile()`：在系统临时目录用可预测文件名并普通 `ofstream` 创建，存在跨进程碰撞/符号链接风险。
- `ParseLrcTimestamp()`：使用 `ParseUInt16()` 作为宽松解析器，非法时间字段可能被解析为 0。

# Bug Report

## 1. MP4 歌词 atom 递归无深度限制

## 风险等级

High

## 问题类型

Parser recursion risk / DoS / Stack exhaustion

## 位置

`src/TagReader.cpp:3506-3566`，`TagReader::ReadMP4Lyrics()` 和 `TagReader::ReadMP4LyricsAtomTree()`

## 触发条件

恶意 MP4 构造大量嵌套 `moov`、`udta`、`meta`、`ilst` atom，每层 payload 内继续放同名容器 atom。

## 根本原因

`ReadMP4LyricsAtomTree()` 对容器 atom 递归调用自身，但没有 depth 参数、深度上限或显式栈。metadata 路径 `ReadMP4AtomTree()` 至少通过 `depth` 固定在 root -> moov -> udta -> meta -> ilst；lyrics 路径没有同等状态机。

## 实际风险

可通过单个畸形 MP4 触发进程栈耗尽崩溃。因为每一层 atom header 都能保持合法边界，普通越界检查不会阻止递归深度攻击。

## 是否可被 fuzzing 命中

是。结构感知 MP4 fuzzing 很容易生成深层嵌套容器。

## 修复建议

给 `ReadMP4LyricsAtomTree()` 增加 `depth` 参数和最大深度，例如 8 或与 metadata 路径一致的严格状态机。更稳妥的做法是改为显式栈遍历，并限制待扫描 atom 数量。

## 2. Ogg Vorbis packet 聚合缺少总大小上限

## 风险等级

High

## 问题类型

Memory DoS / Parser resource exhaustion

## 位置

`src/TagReader.cpp:2570-2664`，`TagReader::ReadOggVorbisCommentEntries()`

## 触发条件

Ogg 页面 segment table 长期使用 `255` lacing value，使 packet 持续跨页延续而不结束。

## 根本原因

函数将每页 payload 追加到 `packet`，只有遇到 `segmentSize < 255` 才清空或解析。没有限制累计 `packet.size()`、页数量或扫描字节数。

## 实际风险

恶意 Ogg 文件可导致内存持续增长直到 OOM。即使最终没有 comment header，也会消耗大量内存和 CPU。

## 是否可被 fuzzing 命中

是。lacing table 是 fuzzer 容易变异的字段。

## 修复建议

为 Ogg packet、总扫描页数、总扫描字节数设置上限。解析 comment header 前也应验证 page continuation、header type、bitstream serial、sequence number，并在异常延续时停止。

## 3. MP4 `data` atom 可触发超大一次性分配

## 风险等级

High

## 问题类型

Memory DoS / File trust issue

## 位置

`src/TagReader.cpp:2910-2963`，`TagReader::ReadMP4ItemAtom()`；`src/TagReader.cpp:3865-3906`，`TagReader::ReadMP4LyricsItem()`；`src/TagReader.cpp:3908-3980`，`TagReader::ReadMP4FreeformLyricsItem()`

## 触发条件

MP4 `ilst` item 内构造巨大 `data`、`mean`、`name` atom，size 合法且位于文件范围内。

## 根本原因

读取 payload 时使用 `ReadRange(..., atom.atomEnd - atom.payloadOffset)`，直接把整个 atom payload 放入 `std::vector<uint8_t>`。没有字段级大小上限，也没有对文本、歌词、封面分别设定最大长度。

## 实际风险

单个 metadata atom 可造成大内存分配、长时间磁盘读取、后续文本解码或图像解码放大。MP4 largesize 使攻击输入更自然。

## 是否可被 fuzzing 命中

是。MP4 atom size 是典型 fuzzing 高价值字段。

## 修复建议

对文本字段、歌词字段和封面字段分别设置上限，例如文本 1 MiB、歌词 8 MiB、封面 64 MiB，并在 `ReadRange()` 调用前统一验证。封面应流式读取或至少先判断 MIME/签名和上限。

## 4. ID3v2 tag body 直接按声明大小整体读入

## 风险等级

Medium

## 问题类型

Memory DoS / File trust issue

## 位置

`src/TagReader.cpp:2080-2163`，`TagReader::ReadID3v2Metadata()`；`src/TagReader.cpp:3072-3147`，`TagReader::ReadID3Lyrics()`

## 触发条件

ID3v2 header 中 syncsafe size 声明接近协议上限，且文件实际包含足够字节。

## 根本原因

`tagSize = ReadSyncSafe32(...)` 后直接 `ReadRange(context.input, 10, tagSize)`。syncsafe 28-bit 最大接近 256 MiB，对音乐标签库来说过大。

## 实际风险

畸形 MP3 可造成大内存分配和长时间解析；metadata 和 lyrics 路径还会分别读取同一 tag body，放大开销。

## 是否可被 fuzzing 命中

是。ID3 size 是 fuzzer 常改字段。

## 修复建议

设置 ID3 tag 最大可接受大小。metadata 和 lyrics 可以共享一次 tag 扫描结果，避免重复读入。对超限 tag 返回错误或跳过标签。

## 5. 临时封面文件名可预测且普通覆盖创建

## 风险等级

High

## 问题类型

Unsafe temp file / File overwrite / Symlink risk

## 位置

`src/TagReader.cpp:237-267`，`MakeCoverPathForAudioFile()` 和 `WriteBinaryFile()`

## 触发条件

本地攻击者可观察或猜测 temp 目录文件名，提前创建同名文件或符号链接；或多个进程同时处理同名音频。

## 根本原因

文件名由音频 stem、`file_time_type::clock::now()` 和进程内 atomic counter 组成，不是原子安全创建。`std::ofstream out(path, std::ios::binary)` 会跟随符号链接并截断已有文件。

## 实际风险

在共享 `/tmp` 场景下可能覆盖攻击者指定路径，或被跨进程碰撞覆盖。虽然路径带时间戳降低概率，但不是安全临时文件策略。

## 是否可被 fuzzing 命中

普通文件 fuzzing 不易命中；并发/本地攻击测试可命中。

## 修复建议

使用 `mkstemp`、`std::filesystem` 配合平台安全 open flags，或在私有临时目录中用 `O_CREAT | O_EXCL | O_NOFOLLOW` 创建。返回路径前确保权限和所有权符合预期。

## 6. PNG 封面只按签名直写，不验证完整图像

## 风险等级

Medium

## 问题类型

Malformed file handling / Unsafe export semantics

## 位置

`src/TagReader.cpp:499-564`，`WriteCoverAsPng()`

## 触发条件

嵌入封面 payload 以 PNG 签名开头，但后续内容损坏、截断或包含超大垃圾数据。

## 根本原因

`format == ImageFormat::Png` 时直接 `WriteBinaryFile()`，不经过 PNG decoder 验证，也不限制 payload 最大大小。

## 实际风险

库会导出损坏 PNG 或超大垃圾文件，并把路径返回给调用方。下游 UI 或图像库再打开该文件时承担风险。

## 是否可被 fuzzing 命中

是。构造 APIC/PICTURE/covr 中的伪 PNG 很容易。

## 修复建议

即使源是 PNG，也至少用 FFmpeg decoder 验证图像可解码；或提供“快速直写但校验 chunk 长度/CRC/尺寸”的路径。增加封面 payload 和输出 PNG 大小上限。

## 7. 图像转码缺少自定义资源上限

## 风险等级

High

## 问题类型

CPU/Memory DoS / Decompression bomb

## 位置

`src/TagReader.cpp:415-492`，`ConvertImageToPng()`

## 触发条件

嵌入高压缩比、大尺寸、复杂或畸形图片，例如巨大 WEBP/GIF/TIFF/BMP。

## 根本原因

代码只检查 `decodedFrame->width/height > 0` 和 `av_image_check_size()`，没有业务级像素总量、解码时间、帧数、输出大小上限。

## 实际风险

恶意封面可消耗大量 CPU 和内存。GIF/WEBP 虽然只取首帧，但 decoder 仍可能做复杂工作。

## 是否可被 fuzzing 命中

是。封面 payload 是高价值 fuzz target。

## 修复建议

增加最大封面输入字节数、最大宽高、最大像素数、最大输出 PNG 字节数。对 animated 格式明确只解码首帧并限制 decoder 行为。必要时在独立进程/沙箱中转码。

## 8. MP4 lyrics parser 接受任意层级的 `©lyr` / `----`

## 风险等级

Medium

## 问题类型

Spec violation / Parser confusion

## 位置

`src/TagReader.cpp:3522-3566`，`TagReader::ReadMP4LyricsAtomTree()`

## 触发条件

MP4 文件在非 `moov/udta/meta/ilst` 路径中放置 `©lyr` 或 `----` atom。

## 根本原因

lyrics atom tree 递归没有像 metadata 路径一样跟踪 strict path depth，而是在扫描任何层级时看到 `©lyr` 或 `----` 就解析。

## 实际风险

可能从错误位置提取歌词，导致 parser 状态污染或误读二进制数据为文本。fuzzer 可用它触发异常解码路径。

## 是否可被 fuzzing 命中

是。

## 修复建议

与 metadata 路径统一，严格只在 `moov/udta/meta/ilst` 下识别歌词 item。复用同一个 MP4 atom walker 和路径状态机。

## 9. MP4 atom `size == 0` 在子 atom 中被接受

## 风险等级

Medium

## 问题类型

Spec violation / Parser recovery bug

## 位置

`src/TagReader.cpp:1535-1603`，`ReadMp4AtomHeader()`；调用点 `ReadMP4AtomTree()`、`ReadMP4ItemAtom()`、`ReadMP4LyricsAtomTree()`

## 触发条件

在非顶层或 bounded parent 内构造 `size == 0` atom。

## 根本原因

`ReadMp4AtomHeader()` 将 `atomSize == 0` 统一解释为延伸到当前 `limit`，没有区分 top-level 和 child-level。

## 实际风险

恶意子 atom 可吞掉 parent 内后续 siblings，使 parser 错过合法字段或从错误边界恢复。虽然代码会在处理后 return，避免死循环，但行为不符合 ISO BMFF 常见约束。

## 是否可被 fuzzing 命中

是。

## 修复建议

给 `ReadMp4AtomHeader()` 增加参数标识是否允许 `size == 0`。通常只允许 top-level atom 使用；在 child atom 中遇到应视为 malformed 并停止当前分支。

## 10. MP4/FLAC/Ogg dispatch 存在签名与 containerName 二次检查不一致

## 风险等级

Medium

## 问题类型

Logic bug / Parser dispatch coupling

## 位置

`src/TagReader.cpp:1942-1984`，`ReadMetadata()`；`src/TagReader.cpp:2473-2496`，`ReadVorbisCommentMetadata()`；`src/TagReader.cpp:2829-2845`，`ReadMP4Metadata()`；`src/TagReader.cpp:3434-3504`，`ReadVorbisLyrics()`；`src/TagReader.cpp:3506-3520`，`ReadMP4Lyrics()`

## 触发条件

`ReadMetadata()` 通过 signatureContainer 判定格式，但具体 parser 入口再次只检查 FFmpeg `containerName`，且该名称为空、别名不同或不含预期字符串。

## 根本原因

格式识别逻辑分散在分发层和具体 parser 层，条件不完全一致。

## 实际风险

某些有效文件会进入分发但实际 parser 立即返回，导致标签/歌词丢失。恶意文件也可利用命名差异诱导错误 parser 路径。

## 是否可被 fuzzing 命中

中等。需要 FFmpeg probe 与签名结果不一致。

## 修复建议

在 `ReadContext` 中保存一个枚举型 `DetectedContainer`，所有 metadata/lyrics parser 只消费这个稳定结果。具体 parser 不再重复字符串匹配。

## 11. Vorbis Comment 对非法 UTF-8 做本地编码回退

## 风险等级

Medium

## 问题类型

Invalid UTF handling / Spec violation

## 位置

`src/TagReader.cpp:2666-2757`，`ReadVorbisCommentEntry()`；`src/TagReader.cpp:3568-3586`，`ReadVorbisLyricsEntry()`

## 触发条件

FLAC/Ogg Vorbis Comment 中包含非法 UTF-8 字节。

## 根本原因

Vorbis Comment 规范要求 UTF-8，但代码调用 `DecodeRawText()`，会在 UTF-8 无效时尝试 UTF-16 嗅探或 legacy local encoding。

## 实际风险

损坏或恶意 comment 可能被错误接受并转成看似合法文本，造成字段污染。不同平台 iconv 可用性不同，结果不稳定。

## 是否可被 fuzzing 命中

是。

## 修复建议

Vorbis Comment 应严格 `DecodeTextToUtf8(..., "utf-8")`，非法 UTF-8 直接丢弃或报错。兼容回退只能作为显式选项。

## 12. ID3 UTF-8 文本非法时回退 Latin-1

## 风险等级

Medium

## 问题类型

Invalid UTF handling / Spec violation

## 位置

`src/TagReader.cpp:1482-1504`，`ReadId3ByteString()`

## 触发条件

ID3v2 文本帧 encoding byte 为 `3`，但 payload 不是合法 UTF-8。

## 根本原因

`case 3` 中 `ReadUtf8Text()` 后如果不是合法 UTF-8，会返回 `ReadLatin1Text()`。

## 实际风险

违反 ID3v2.4 UTF-8 声明语义，损坏字段被静默接受，可能污染 metadata 或隐藏 malformed 输入。

## 是否可被 fuzzing 命中

是。

## 修复建议

encoding byte 为 `3` 时只接受合法 UTF-8。兼容 Latin-1 回退应限制到 encoding byte 为 `0` 或用户明确开启的容错模式。

## 13. LRC 时间戳非法字段会被解析为 0

## 风险等级

Medium

## 问题类型

Logic bug / Malformed lyrics handling

## 位置

`src/TagReader.cpp:3982-4014`，`ParseLrcTimestamp()`

## 触发条件

歌词行包含 `[abc:def]`、`[:.]`、`[00:xx]` 等非数字时间戳。

## 根本原因

函数用 `ParseUInt16()` 解析 minutes/seconds/millis，而 `ParseUInt16()` 在无数字消费或异常时返回 0；调用方无法区分真实 0 和解析失败。

## 实际风险

非法 LRC 标签会被当作 `00:00.000` 时间戳，生成错误同步歌词。

## 是否可被 fuzzing 命中

是。

## 修复建议

实现严格数字解析函数，返回 `std::optional<uint16_t>` 或 bool。要求 minutes、seconds 至少一位数字，seconds < 60，毫秒部分长度和字符合法。

## 14. 多时间戳 LRC 行解析错误

## 风险等级

Low

## 问题类型

Lyrics parser logic bug / Spec compatibility

## 位置

`src/TagReader.cpp:3588-3654`，`ReadLyricsFromPlainText()`

## 触发条件

LRC 行形如 `[00:01.00][00:02.00]same lyric`。

## 根本原因

每匹配一个 timestamp 后立即用 `line.substr(close + 1)` 作为歌词文本。第一个 timestamp 的文本会包含后续 timestamp 字符串。

## 实际风险

输出错误歌词文本和重复/污染的 timed line。

## 是否可被 fuzzing 命中

是。

## 修复建议

先收集行首所有 timestamp，再在最后一个 `]` 后截取歌词文本，然后为每个 timestamp 添加同一文本。

## 15. MP4 文本 data type 支持不完整

## 风险等级

Low

## 问题类型

Spec compatibility issue

## 位置

`src/TagReader.cpp:2974-3070`，`ReadMP4DataAtom()`；`src/TagReader.cpp:3865-3980`，MP4 lyrics item 读取

## 触发条件

MP4 metadata `data` atom 使用 UTF-16 或其他合法 text data type。

## 根本原因

当前只处理 `dataType == 1` UTF-8 和 `dataType == 0` 隐式原始文本；注释中明确跳过 UTF-16 和其他类型。

## 实际风险

兼容性缺失，合法 MP4/M4A 标签被漏读。

## 是否可被 fuzzing 命中

是，但属于兼容性而非内存安全。

## 修复建议

补充 MP4 data type 到编码的映射，至少支持 UTF-16BE/LE 的常见类型，并严格验证 payload。

## 16. `ReadRange()` 没有业务级读取上限

## 风险等级

Medium

## 问题类型

Resource exhaustion / API design weakness

## 位置

`src/TagReader.cpp:963-993`，`ReadRange()`

## 触发条件

任意 parser 调用 `ReadRange()` 时传入来自文件长度字段的大 size。

## 根本原因

`ReadRange()` 只检查 `streamoff`、`streamsize` 和加法溢出，不知道调用场景，也不限制最大 vector 分配。

## 实际风险

多个 parser 都可能把 malformed 长度字段转化为大内存分配。异常路径可能抛出 `std::bad_alloc`，而不是清晰的 parse error。

## 是否可被 fuzzing 命中

是。

## 修复建议

增加带上限参数的读取函数，例如 `ReadRange(input, offset, size, maxSize)`。不同字段使用不同上限，并把超限作为 malformed 输入处理。

## 17. `ReadRange()` partial read 后保留 fail/eof 状态直到下一次 clear

## 风险等级

Low

## 问题类型

Stream state coupling

## 位置

`src/TagReader.cpp:963-993`，`ReadRange()`

## 触发条件

读取不足，函数返回空 vector，但没有在返回前清理 stream 状态。

## 根本原因

函数入口会 `input.clear()`，但 partial read 失败返回前不恢复状态。后续使用 `ReadRange()` 可自愈，直接使用 `context.input` 的代码需自己 clear。

## 实际风险

当前多数直接 stream 使用点会自行 `clear()`，但共享 mutable stream 的耦合很脆弱，后续新增 parser 容易踩坑。

## 是否可被 fuzzing 命中

间接可命中。

## 修复建议

`ReadRange()` 在任何失败返回前恢复 stream 状态，或封装所有 raw stream 操作为不可泄漏状态的 reader 对象。

## 18. `NormalizeMetadata()` 对已解码字段再次嗅探解码

## 风险等级

Low

## 问题类型

Parser state pollution / Encoding design weakness

## 位置

`src/TagReader.cpp:3803-3824`，`NormalizeMetadata()`；`src/TagReader.cpp:3826-3843`，`NormalizeLyrics()`

## 触发条件

字段已经由 ID3 声明编码、Vorbis UTF-8 或 MP4 UTF-8 解码为 UTF-8 后，再进入 Normalize 阶段。

## 根本原因

`RawMetadata` 只保存 `std::string`，没有区分“原始字节”和“已解码 UTF-8”。Normalize 阶段只能重新嗅探。

## 实际风险

短文本或边界文本可能被错误检测为 UTF-16/legacy 编码，或非法字段被兼容回退。当前 `IsValidUtf8()` 优先降低了概率，但架构仍不清晰。

## 是否可被 fuzzing 命中

可命中字段污染类问题。

## 修复建议

拆分 `RawBytesField` 与 `DecodedTextField`，记录 encoding provenance。已声明解码成功的字段不再二次嗅探。

## 19. `ConvertTextWithIconv()` 输出扩容缺少 overflow guard

## 风险等级

Medium

## 问题类型

Integer overflow / Memory DoS

## 位置

`src/TagReader.cpp:1272-1319`，`ConvertTextWithIconv()`

## 触发条件

超大输入文本反复触发 `E2BIG`。

## 根本原因

`output.resize(output.size() * 2, '\0')` 没有检查乘法溢出或最大输出大小。

## 实际风险

可能抛出异常或持续扩容导致 OOM。虽然实际 tag 大小通常有限，但 MP4/Vorbis 恶意文本可放大。

## 是否可被 fuzzing 命中

是，尤其是带 iconv 的构建。

## 修复建议

在扩容前检查 `output.size() <= max / 2`，并设置文本输出最大字节数。

## 20. `ReadLatin1Text()` reserve 可能按输入大小成倍分配

## 风险等级

Low

## 问题类型

Memory DoS / Integer overflow edge

## 位置

`src/TagReader.cpp:1247-1270`，`ReadLatin1Text()`

## 触发条件

恶意字段声明为 Latin-1 且长度巨大。

## 根本原因

`value.reserve(size * 2)` 没有检查 `size * 2` 溢出，也没有字段大小上限。

## 实际风险

在现实 64-bit 上主要表现为内存 DoS 或 `length_error`；理论上存在乘法溢出风险。

## 是否可被 fuzzing 命中

是。

## 修复建议

增加最大文本字段长度，使用安全乘法检查，或按需增长而非直接按最大扩容。

## 21. 音频声道数和比特率存在窄化截断

## 风险等级

Low

## 问题类型

Integer truncation / Logic bug

## 位置

`src/TagReader.cpp:1877-1940`，`ReadMediaInfo()`

## 触发条件

FFmpeg 返回极端 `channels > 255` 或 `bit_rate > UINT32_MAX`。

## 根本原因

声道数直接 cast 到 `uint8_t`，比特率直接 cast 到 `uint32_t`，无范围检查。

## 实际风险

输出错误媒体信息。恶意文件可通过 FFmpeg probe 产生极端参数时触发。

## 是否可被 fuzzing 命中

可能。

## 修复建议

超出目标类型范围时返回 0、饱和值或扩大 `MusicTag` 字段类型。

## 22. ID3v2.3 extended header 边界校验不精确

## 风险等级

Low

## 问题类型

Spec violation / Parser recovery bug

## 位置

`src/TagReader.cpp:2129-2154`，`ReadID3v2Metadata()`；`src/TagReader.cpp:3115-3138`，`ReadID3Lyrics()`

## 触发条件

ID3v2.3 extended header size 接近 tag body 尾部，例如 `extSize == tagBytes.size()`。

## 根本原因

v2.3 extended header size 不包含 size 字段自身，跳过长度是 `4 + extSize`。代码检查 `extSize > tagBytes.size()`，但更准确应检查 `4 + extSize <= frameLimit`。

## 实际风险

畸形 tag 可能使 cursor 超过 frameLimit，当前通常表现为静默不读帧，而非 OOB。属于 malformed recovery 和兼容性问题。

## 是否可被 fuzzing 命中

是。

## 修复建议

使用无溢出加法计算 `extendedHeaderEnd`，要求其不超过 `frameLimit`。

## 23. ID3v2.2 `ReadID3v22LyricsFrames()` 忽略传入 cursor 的 extended header 语义

## 风险等级

Low

## 问题类型

Spec compatibility / Parser dispatch bug

## 位置

`src/TagReader.cpp:3140-3143`，`ReadID3Lyrics()`；`src/TagReader.cpp:3155-3240`，`ReadID3v22LyricsFrames()`

## 触发条件

ID3v2.2 header 带不支持的 extended header flag 或异常 flag。

## 根本原因

v2.2 不支持当前 v2.3/v2.4 extended header 处理，但入口仍复用 `flags & 0x40` 分支并在 v2.2 时 return。metadata 路径 v2.2 直接从 0 扫描。两条路径行为不完全一致。

## 实际风险

异常 ID3v2.2 文件 metadata 和 lyrics 行为不一致。不是内存安全问题。

## 是否可被 fuzzing 命中

是。

## 修复建议

明确按版本解释 flags。v2.2 遇到未知 flag 时统一跳过或报 malformed，不复用 v2.3/v2.4 分支。

## 24. `ReadMP4DataAtom()` 对 MP4 `trkn`/`disk` payload 结构校验过弱

## 风险等级

Low

## 问题类型

Spec compatibility / Logic bug

## 位置

`src/TagReader.cpp:3033-3056`，`ReadMP4DataAtom()`

## 触发条件

`trkn` 或 `disk` atom payload 长度为 6 但保留字段、总数、locale/type 语义异常。

## 根本原因

代码只读取 `payload[2..3]` 作为编号，没有验证完整结构、reserved 字段和 total 字段。

## 实际风险

可能接受 malformed numeric atom 并输出错误编号。

## 是否可被 fuzzing 命中

是。

## 修复建议

按 Apple metadata atom 结构完整解析 `reserved/index/total`，并对异常 reserved 或超长 payload 做一致策略。

## 25. API 返回临时封面路径但没有生命周期契约

## 风险等级

Medium

## 问题类型

API design defect / Ownership issue

## 位置

`include/Tag.hpp:31-33`、`src/TagReader.cpp:499-564`、`src/TagReader.cpp:4016-4073`

## 触发条件

调用方读取 `MusicTag::coverPath()` 后，临时目录清理、进程重启、文件被其他进程删除或覆盖。

## 根本原因

API 只返回 `coverPath`，没有说明文件归属、清理责任、有效期，也没有 RAII 对象管理导出文件。

## 实际风险

调用方可能持有失效路径，或长期泄漏临时封面文件。并发读取会产生大量临时文件。

## 是否可被 fuzzing 命中

否，属于 API 设计风险。

## 修复建议

明确封面导出策略：返回内存字节、由调用方传入输出目录、或提供 `CoverHandle` 管理生命周期。至少在文档中声明清理责任。

# Structural Weakness

- `src/TagReader.cpp` 是明显 God Object，超过 4000 行，媒体信息、ID3、FLAC、Ogg、MP4、歌词、编码、图像转码、文件导出全部混在一个翻译单元。审计和 fuzz triage 成本高。
- `ReadContext` 是共享 mutable 状态，所有 parser 共享同一个 `std::ifstream`。虽然 `ReadRange()` 会在入口 clear/seek，但 parser 之间仍存在 stream 状态耦合。
- 格式识别使用字符串匹配和签名检测混合，且 parser 入口重复判断，容易产生“分发判定是 A，具体 parser 又拒绝 A”的状态污染。
- `RawMetadata` / `RawLyrics` 使用 `std::string` 同时表示原始字节、已解码文本和已归一化 UTF-8，缺少 provenance。导致重复 normalization 和错误回退难以避免。
- 多格式 parser 重复实现 frame/header/atom 遍历逻辑，metadata 和 lyrics 各自扫描 ID3 tag、MP4 atom、Vorbis comments，增加不一致风险和性能开销。
- 没有统一 malformed 输入策略。有些路径 throw，例如 ID3 metadata、FLAC metadata；有些路径 return false 或静默跳过，例如 Ogg、MP4 lyrics、ID3 lyrics。调用方难以判断“无标签”和“标签损坏”。
- 缺少统一资源预算。没有最大 tag 大小、最大 atom payload、最大歌词长度、最大封面大小、最大递归深度、最大 Ogg packet、最大解码图像像素等限制。
- 封面导出是副作用 API。`Read()` 不只是读取，还会写临时文件；异常路径和生命周期语义不清晰。
- 文本编码策略偏兼容，倾向于把非法 UTF-8 当 legacy encoding 读取。这对播放器兼容有利，但对安全审计和协议一致性不利。
- Boost flyweight 在 `MusicTag` 中引入全局字符串池语义。并发构造大量 `MusicTag` 时需要确认 boost flyweight factory 的线程安全和内存增长特性。

# Spec Compliance Issues

## ID3v2.2

- `PIC` front cover 支持存在，但只导出 picture type 3，其他 picture type 不作为候选；这是产品策略，不是完整 ID3 图片语义。
- `SLT` 只支持 timestamp format 2 milliseconds，不支持 MPEG frames timestamp。当前是安全跳过，但兼容性不完整。
- v2.2 flag 处理与 v2.3/v2.4 逻辑混用，异常 flags 的行为不够清晰。

## ID3v2.3

- compressed/encrypted frames 直接跳过，符合安全保守策略，但不完整支持。
- extended header size 边界检查应以 `4 + extSize <= frameLimit` 表达，目前校验不精确。
- text encoding byte 为 UTF-8 的非法输入会回退 Latin-1，这违反声明编码语义。

## ID3v2.4

- UTF-8 声明非法时回退 Latin-1，不符合严格 ID3v2.4。
- data length indicator 被用于 resize frameData，但缺少对异常大 declared size 的业务上限；协议上合法但资源上危险。
- footer 和 extended header 已有基本处理，但 metadata 和 lyrics 路径重复实现，存在未来不一致风险。

## FLAC Metadata

- 没有验证第一个 metadata block 必须是 STREAMINFO 且长度为 34。
- `PICTURE` 中 MIME、description、尺寸字段基本跳过，不完整验证图片元信息与实际图片 payload 一致性。
- Vorbis Comment 被允许 legacy encoding 回退，违反 Vorbis Comment UTF-8 要求。

## Vorbis Comment / Ogg

- Ogg page parser 不校验 checksum、version、header type continuation、bitstream serial、page sequence number。
- packet 组装只依赖 lacing value，不验证跨页 continuation 标志。
- Vorbis Comment 字段按 UTF-8 规范应严格验证，当前走 `DecodeRawText()` 兼容回退。

## ISO MP4 / M4A Atom

- metadata 路径只识别 `moov/udta/meta/ilst`，不处理更多合法位置或 freeform metadata 字段，兼容性有限。
- lyrics 路径没有 strict path depth，可能从非标准位置提取 `©lyr` / `----`。
- child atom 中接受 `size == 0` 并扩展到 parent limit，不符合常见 BMFF 约束。
- MP4 text `dataType` 支持不完整，UTF-16 等合法文本类型会被跳过。
- `mean`/`name` freeform 只接受 `com.apple.iTunes` 和 `lyrics`，合理但不是完整 freeform parser。

# Fuzzing Targets

最适合 fuzz 的函数：

- `ReadID3v2Metadata()`：目标是 ID3 header size、flags、extended header、frame header、frame flags、APIC payload。
- `ReadID3Lyrics()`：目标是 USLT/SYLT/ULT/SLT/TXXX 的 encoding、terminator、timestamp、frame truncation。
- `ReadFlacMetadataBlocks()`：目标是 block header length、last flag、Vorbis comment block、PICTURE block。
- `ReadOggVorbisCommentEntries()`：目标是 Ogg page header、segment table、continuation packet、comment length/count。
- `ReadMP4AtomTree()`：目标是 atom size 0/1、largesize、nested atom、meta full box offset、data atom。
- `ReadMP4LyricsAtomTree()`：目标是 deep nested atom、non-standard `©lyr`、freeform `----`、largesize truncation。
- `DecodeTextToUtf8()` / `DetectTextEncoding()`：目标是 invalid UTF-8、odd UTF-16、surrogate、BOM mismatch、legacy byte soup。
- `WriteCoverAsPng()` / `ConvertImageToPng()`：目标是 APIC/PICTURE/covr 内图片 payload，包括 malformed PNG/JPEG/BMP/WEBP/GIF/TIFF。

推荐 fuzz 输入：

- 最小 MP3 + 可变 ID3v2.2/2.3/2.4 tag。
- 最小 FLAC + 可变 metadata block chain。
- 最小 Ogg Vorbis page 序列，重点生成跨页 comment packet。
- 最小 M4A/MP4 atom tree，重点变异 size、largesize、嵌套深度、`data` payload。
- 独立封面 payload corpus：PNG/JPEG/BMP/WEBP/GIF/TIFF 的合法样本和截断样本。
- 文本 corpus：UTF-8 boundary、UTF-16LE/BE odd length、surrogate、overlong UTF-8、GBK/Shift-JIS/Latin-1 混合。

推荐 sanitizer：

- ASAN：捕获越界、use-after-free、部分栈/堆问题。
- UBSAN：捕获整数 UB、非法 shift、窄化相关未定义行为。
- MSAN：如果环境允许，用于未初始化数据传播。
- LSAN：检查图像转码和异常路径资源泄漏。

推荐编译参数：

```text
-O1 -g -fno-omit-frame-pointer
-fsanitize=address,undefined
-fno-sanitize-recover=all
```

推荐 libFuzzer harness：

- 将 fuzz bytes 写入临时文件，然后调用 `TagReader::Read(tempPath)`，捕获所有 C++ 异常但不捕获 sanitizer 崩溃。
- 使用固定私有临时目录，并在每次迭代后清理导出的 `coverPath`，避免磁盘填满。
- 增加超时和 RSS 限制，因为图像 decoder 与大 tag 容易 DoS。
- 为 MP4/ID3/FLAC/Ogg 分别建 corpus，而不是只做纯随机输入。

# Overall Security Assessment

parser 安全等级：中等偏低。当前代码有较多边界检查和无溢出加法，直接越界读写风险不算最高；但资源上限、递归控制、格式状态机一致性和临时文件安全仍明显不足。

crash 风险：中到高。MP4 lyrics 深递归、Ogg packet 聚合、超大 tag/atom 分配、图像解码 bomb 都可能造成崩溃或 OOM。

malformed 文件鲁棒性：中等。ID3/FLAC 部分路径对截断输入会 throw 或安全 return，MP4 atom header 做了较好的 offset 边界检查；但 Ogg continuation、MP4 lyrics path、文本回退和封面导出对恶意结构仍脆弱。

fuzzing 风险等级：高。该项目非常适合 fuzz，尤其是 MP4 atom、ID3 frame、Ogg packet、图片 payload 和编码路径。预计 fuzzing 会较快发现 DoS、栈耗尽、异常语义不一致、错误字段接受等问题。

当前代码质量评估：实现比原始“直接信任 FFmpeg metadata”更接近项目设计目标，且很多二进制边界已有基础检查；但 parser 架构仍过于集中，缺少统一 reader、统一资源预算、统一格式状态机和统一 malformed 策略。建议优先修复高风险 DoS/递归/临时文件问题，再拆分 parser 并补 libFuzzer 回归。
