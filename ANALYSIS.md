# Architecture Overview

## parser 架构

TagReader 是单入口 facade：`TagReader::Read(path)` 转到 `TagReader::Read(path, coverExportDir)`，内部构造一个 `ReadContext`，再按容器分发到各格式 parser。公共数据类型 `MusicTag`、`Lyrics`、`Lyric` 只做存储，真正的二进制解析集中在 `src/TagReader.cpp`。

主调用链是：`Read()` -> `ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。

`ReadMetadata()` 是容器分发入口：MP4 走 `ReadMP4Metadata()`，FLAC/Ogg 走 `ReadVorbisCommentMetadata()`，MP3 先走 `ReadID3v2Metadata()` 再用 `ReadID3v1Metadata()` 补缺失字段，Unknown 只尝试 ID3v1。该函数吞掉 `std::filesystem::filesystem_error` 和 `std::runtime_error`，所以 malformed metadata 通常退化为空字段而不是让整个 `Read()` 失败。

`ReadLyrics()` 是歌词分发入口：MP3 走 `ReadID3Lyrics()`，FLAC/Ogg 走 `ReadVorbisLyrics()`，MP4 走 `ReadMP4Lyrics()`。它同样吞掉 filesystem/runtime 异常，失败后清空歌词。

## 调用关系

ID3 metadata 调用链：`ReadID3v2Metadata()` -> `ReadId3TagBytes()` -> `ReadID3v22Frames()` 或 `ReadID3v23Or24Frames()` -> `ReadID3v22Frame()` / `ReadID3v2Frame()` -> 文本帧 `ReadId3TextFrame()` 或图片帧 `ReadID3v22PictureFrame()` / `ReadID3v2PictureFrame()` -> `WriteCoverAsPng()`。

ID3 lyrics 调用链：`ReadID3Lyrics()` -> `ReadId3TagBytes()` -> `ReadID3v22LyricsFrames()` 或 `ReadID3v23Or24LyricsFrames()` -> `AppendPlainLyrics()` / timed line vector -> `NormalizeLyrics()`。

FLAC metadata 调用链：`ReadVorbisCommentMetadata()` -> `ReadFlacMetadataBlocks()` -> Vorbis comment block 使用 `ForEachVorbisCommentEntry()`，picture block 使用 `ReadFlacPictureEntry()` -> `WriteCoverAsPng()`。

Ogg Vorbis 调用链：`ReadVorbisCommentMetadata()` -> `ReadOggVorbisComments()` -> `ReadOggVorbisCommentEntries()` -> `ForEachVorbisCommentEntry()` -> `ReadVorbisCommentEntry()`。

MP4 metadata 调用链：`ReadMP4Metadata()` -> `ReadMP4AtomTree()` -> `ReadMP4ItemAtom()` -> `ReadMP4DataAtom()` -> `DecodeMp4TextData()` 或 `WriteCoverAsPng()`。

MP4 lyrics 调用链：`ReadMP4Lyrics()` -> `ReadMP4LyricsAtomTree()` -> `ReadMP4LyricsItem()` / `ReadMP4FreeformLyricsItem()` -> `ReadLyricsFromPlainText()`。

文本归一化链：ID3 专用 encoding byte 走 `ReadId3ByteString()`，Vorbis/MP4 data atom 走 `DecodeTextToUtf8()`，ID3v1 和 MP4 data type 0 走 `DecodeRawText()` -> `DetectTextEncoding()` -> `DecodeTextToUtf8()`。最终 `NormalizeMetadata()` 和 `NormalizeLyrics()` 只再次验证已经是 UTF-8 的字符串。

封面导出链：各格式图片块传原始图片字节给 `WriteCoverAsPng()`，该函数按 FNV 风格哈希构造 `<coverExportDir>/<2hex>/<rest>.png`，用 FFmpeg 解码/转码为 PNG，再通过 sibling tmp 文件加 `rename()` 发布。

## 数据流

文件路径和导出目录进入 `ReadContext`。FFmpeg 的 `AVFormatContext` 只用于 probe、音频流和基础媒体信息；标签内容主要从 `ReadContext::input` 原始字节读取。中间结果保存在 `RawMediaInfo`、`RawMetadata`、`RawLyrics`，最后 `BuildMusicTag()` 组装为 `MusicTag`。

文本字段先落到普通 `std::string`，最终 `MusicTag` setter 写入 `boost::flyweight<std::string>`。歌词 plain text 会在 `BuildMusicTag()` 中按换行拆成 timestamp 为 0 的 `Lyric`。

## 状态流

`ReadContext` 是共享可变状态，包含 `std::ifstream input`、文件大小、FFmpeg context、检测容器和 cover 目录。所有格式 parser 共享同一个 stream，但 `ReadRange()` 每次会 `clear()`、`seekg()`、`read()`，多数失败路径也会 `clear()`，因此 stream failbit 通常不会污染后续 parser。直接使用 `context.input.seekg/read` 的 ID3v1 路径在短读时不 clear，但后续主要 `ReadRange()` 会清理状态。

metadata 与 lyrics 两次独立解析 ID3 tag、Vorbis comments 或 MP4 atom，状态通过 `ReadContext` 和输出结构传递。metadata parser 对 malformed block 的异常被上层吞掉，lyrics parser 也会吞掉并清空歌词；这增强了容错，但会隐藏真实解析错误。

## parser dispatch 逻辑

容器识别优先看文件头签名：`ID3`、`fLaC`、`OggS`、`ftyp`；否则回退到 FFmpeg container name。MP3 无 ID3 头但 FFmpeg 识别为 mp3 时仍可读取 ID3v1；ID3v2 只在 offset 0 读取，不支持文件前有其它前缀再出现 ID3v2 的情况。

## normalization 流程

ID3 文本根据 frame encoding byte 直接解码；ID3v1 没有 encoding marker，走 sniff。Vorbis comment 按 spec 作为 UTF-8 验证，不做 legacy 回退。MP4 data type 1/0 当 UTF-8，type 2 当 UTF-16BE，type 3 当 UTF-16LE。

`NormalizeMetadata()` 和 `NormalizeLyrics()` 不做二次编码探测，只 trim 并校验 UTF-8；任何已经被错误解码但仍合法 UTF-8 的内容会被保留。

## IO 模型

通用二进制读取使用 `ReadRange(std::ifstream&, offset, size, maxSize)`，它检查单次读取最大值、`streamoff`/`streamsize` 可表示性、`offset + size` 溢出，之后清状态、seek、read，并要求 `gcount() == size`。

FFmpeg open/probe 在 `OpenContext()` 中完成。封面导出使用 POSIX `open/write/fsync/close/rename`，而不是 C++ stream。

# Supported Formats Matrix

| 类别 | 实际支持 |
| --- | --- |
| 容器 | MP3、FLAC、Ogg Vorbis、MP4/M4A，Unknown 只尝试 ID3v1 |
| Metadata | ID3v1/ID3v1.1、ID3v2.2、ID3v2.3、ID3v2.4、FLAC Vorbis Comment、Ogg Vorbis Comment、MP4 `ilst` data atom |
| 图片 | ID3v2.2 `PIC`、ID3v2.3/2.4 `APIC`、FLAC `PICTURE` block、MP4 `covr` atom；只导出 picture type 3/front cover |
| 封面源格式 | PNG、JPEG、BMP、WEBP、GIF、TIFF，未知格式会尝试若干 FFmpeg decoder fallback |
| 歌词 | ID3v2.2 `ULT`/`SLT`、ID3v2.3/2.4 `USLT`/`SYLT`/部分 `TXXX`、Vorbis `LYRICS`/`UNSYNCEDLYRICS`/`LYRIC`/`SYLT`/`SYNCEDLYRICS`、MP4 `©lyr` 和 iTunes freeform lyrics |
| 编码 | ID3 encoding 0 Latin-1、1 UTF-16 with BOM fallback LE、2 UTF-16BE、3 UTF-8；sniff 支持 UTF-8 BOM、UTF-16 BOM、无 BOM UTF-16 猜测、iconv legacy 候选和 latin-1 fallback |
| 构建安全工具 | CMake 支持 `TAGREADER_ENABLE_SANITIZERS=ON` 的 ASAN/UBSAN，Clang 下支持 `TAGREADER_ENABLE_FUZZING=ON` 的 libFuzzer target |

# Critical Risk Areas

- `ReadID3v22Frame()` 和 `ReadID3v2Frame()`：所有非图片帧都先按文本帧解码，再判断 frame id。大 `GEOB`、`PRIV`、`MCDI`、`COMR` 等二进制帧可触发高成本文本解码或额外分配。
- `ConvertTextWithIconv()`：`iconv_open()` 成功后，在输入大小超过阈值时直接返回，泄漏 `iconv_t`。
- `ReadMP4ItemAtom()`：如果 item payload 第一个 child 不是 `data`，会扫描 sibling child；但如果第一个 child 是 `data` 后立即返回，导致同一 item 里的后续 `data` 被忽略。
- `ReadMP4AtomTree()`：metadata atom 使用递归，虽然路径深度有限且有 atom count 限制，但 metadata 和 lyrics 两套 MP4 walker 行为不一致，容易出现覆盖缺陷。
- `AtomicWriteFileIfAbsent()`：先 `exists()` 再 tmp `rename()`，在 POSIX 上 `rename()` 会覆盖已存在目标，`EEXIST` 分支实际不成立；并发场景可能覆盖另一个进程刚写好的封面缓存文件。
- `TryReadUtf16Text()`：缺少输出大小限制，若上层传入大 payload，UTF-16 到 UTF-8 可产生较大内存分配；目前多数入口有限制，但函数自身不自保护。
- `ReadOggVorbisCommentEntries()`：对 Ogg stream 的建模较严格且只跟踪一个 serial，fuzz 安全性尚可，但协议兼容性较弱，合法 chained/多流/不从 sequence 0 开始的文件可能被拒绝。
- 封面解码路径：把攻击面交给 FFmpeg image decoders，已有输入/像素/输出限制，但仍是 fuzzing 高价值区域。

# Bug Report

## 1. iconv 句柄泄漏

## 风险等级

Medium

## 问题类型

Resource leak

## 位置

`src/TagReader.cpp`，`ConvertTextWithIconv()`，约 1669-1678 行。

## 触发条件

构建时定义 `TAGREADER_HAS_ICONV=1`，输入触发 `ReadLocaleEncodedText()` 或 legacy encoding 检测，并且传给 `ConvertTextWithIconv()` 的 `size > kMaxDecodedTextBytes / 4`。

## 根本原因

函数先调用 `iconv_open()` 成功获取 `iconv_t cd`，随后检查输入大小。如果大小超过阈值，直接 `return {}`，没有调用 `iconv_close(cd)`。

## 实际风险

单次 `Read()` 会泄漏一个 iconv descriptor。服务端或批处理程序反复读取恶意文件时会逐步耗尽进程资源。因为 `DetectLegacyLocalEncoding()` 会尝试多个候选编码，某些路径可放大泄漏次数。

## 是否可被 fuzzing 命中

可命中，但需要 fuzz target 构建环境启用 Iconv，并构造大于 512 KiB 的 legacy text payload。

## 修复建议

把大小检查移动到 `iconv_open()` 之前，或用 RAII wrapper 管理 `iconv_t`。所有 `return` 路径都必须自动关闭句柄。

## 2. 非文本 ID3 帧被先按文本帧解码导致 CPU/内存 DoS

## 风险等级

Medium

## 问题类型

DoS / Parser dispatch bug

## 位置

`src/TagReader.cpp`，`ReadID3v22Frame()` 约 2744-2753 行，`ReadID3v2Frame()` 约 2839-2849 行。

## 触发条件

ID3v2 tag 中包含合法 frame header 和较大的非文本帧，例如 `GEOB`、`PRIV`、`MCDI`、`COMM` 的任意二进制 payload，且 frame id 不是 `APIC`/`PIC`。

## 根本原因

函数只特判图片帧，然后无条件执行 `ReadId3TextFrame(frameData, frameSize)`，之后才用 frame id 判断是否是标题、歌手、专辑等文本字段。未知或二进制帧也会经过 UTF-8/UTF-16/Latin-1 解码、trim 和校验。

## 实际风险

攻击者可以用 16 MiB ID3 tag 内的大量二进制帧触发重复文本解码、字符串分配和 UTF 校验。虽然 `kMaxId3TagBytes` 限制了上限，仍可造成明显 CPU/内存消耗，尤其是 iconv legacy 路径开启时。

## 是否可被 fuzzing 命中

很容易。生成合法 ID3v2.3/2.4 frame header，frame id 设为非目标 id，payload 首字节控制 encoding，其余填充随机字节即可。

## 修复建议

先判断 frame id 是否属于支持的文本字段集合，再调用 `ReadId3TextFrame()`。未知帧直接跳过。`COMM`、`TXXX` 等需要专门 parser 的帧不要走通用文本字段入口。

## 3. ID3v2.4 tag-level unsynchronization 与 frame offsets 处理不符合 spec

## 风险等级

Medium

## 问题类型

Spec violation / Parser compatibility bug

## 位置

`src/TagReader.cpp`，`ReadId3TagBytes()` 约 2617-2624 行，`PrepareId3v24FrameRegion()` 约 1558-1603 行。

## 触发条件

ID3v2.4 tag 设置 tag-level unsynchronization flag，同时带 footer 或 extended header，或 frame size/offset 依赖原始 tag layout。

## 根本原因

代码读取完整 tag payload 后，若 flags bit 0x80 置位就先对整个 `tagBytes` 执行 `RemoveId3Unsynchronization()`，随后才处理 footer、extended header 和 frame region。unsync 删除字节会改变所有后续 offset，而 v2.4 的 extended header/footer 验证逻辑仍按处理后的 buffer 判断。

## 实际风险

不会形成明显 OOB，因为 vector size 会同步变化且后续有边界检查，但合法 tag 可能被拒绝或错误解析。恶意文件可利用此行为让 metadata/lyrics 丢失，fuzz 会发现解析不稳定或兼容性差。

## 是否可被 fuzzing 命中

容易。构造带 unsync flag、footer flag、extended header flag 的 ID3v2.4 tag，并在 header/frames 中放 `0xFF 0x00` 序列。

## 修复建议

按版本和 spec 分离处理。先在原始 tag payload 上确定 extended header/footer/frame 区间，再对需要 unsync 的 frame payload 应用 unsync。v2.4 优先使用 frame-level unsynchronization flag；tag-level unsync 的处理必须保证 header/footer 结构不被错误修改。

## 4. MP4 item 中多个 data atom 时只处理第一个

## 风险等级

Medium

## 问题类型

Logic bug / Spec compatibility issue

## 位置

`src/TagReader.cpp`，`ReadMP4ItemAtom()`，约 3470-3497 行。

## 触发条件

`ilst` item 的 payload 第一个 child atom 是 `data`，但该 `data` 无效、空、类型不支持，后面还有一个有效 `data` atom。

## 根本原因

`ReadMP4ItemAtom()` 先读 offset 处的一个 atom。如果它是 `data`，处理一次后立即 `return`。只有第一个 atom 不是 `data` 时，才进入 sibling scan loop。MP4 metadata item 可以包含多个 child atom，当前逻辑会忽略第一个 `data` 后面的 sibling。

## 实际风险

合法或半合法 MP4 metadata 被错误读取；恶意文件可用第一个无效 `data` 屏蔽后续有效字段。对安全性主要是 parser 状态/兼容性问题，不是内存破坏。

## 是否可被 fuzzing 命中

可命中，需要 oracle 比较“第一个 data 无效、第二个 data 有效”时字段缺失。

## 修复建议

统一使用 child scan loop，不要对第一个 `data` 特殊 return。对每个 `data` atom 独立验证，成功填字段后可按字段策略决定是否继续扫描。

## 5. MP4 metadata parser 是递归实现，lyrics parser 是显式栈，结构重复且行为不一致

## 风险等级

Low

## 问题类型

Parser architecture weakness / Recursion risk

## 位置

`src/TagReader.cpp`，`ReadMP4AtomTree()` 约 3395-3461 行，`ReadMP4LyricsAtomTree()` 约 4071-4156 行。

## 触发条件

复杂 MP4 atom tree，特别是路径上包含 `moov/udta/meta/ilst`，metadata 和 lyrics 同时解析。

## 根本原因

metadata walker 用递归，lyrics walker 用显式 stack；两者都解析同一类 atom path，但状态枚举、descend 条件和 item handling 彼此复制。当前 metadata 递归深度只按严格 path 到 4，实际栈溢出风险低，但 duplicated walker 容易引入未来不一致。

## 实际风险

当前不太可能 stack overflow，因为深度受 path 限制且 `kMaxMp4Atoms` 限制总数。主要风险是维护时在一个 walker 修复边界/兼容性问题但忘记另一个，导致 fuzz 回归。

## 是否可被 fuzzing 命中

可通过 differential fuzzing 命中 metadata/lyrics walker 行为差异，但不是典型 sanitizer crash。

## 修复建议

提取单一 MP4 atom walker，使用显式栈和统一的 `Mp4PathState`，metadata 和 lyrics 只提供 item callback。保留 atom count、payload limit、size zero 规则在同一个实现中。

## 6. cover cache 发布不是严格“if absent”，并发时可能覆盖已有文件

## 风险等级

Medium

## 问题类型

File race / Atomicity bug

## 位置

`src/TagReader.cpp`，`AtomicWriteFileIfAbsent()`，约 370-445 行。

## 触发条件

两个进程或线程同时导出同一个 `coverPath`，或攻击者能在 `exists(finalPath)` 和 `rename(tempPath, finalPath)` 之间创建目标文件。

## 根本原因

函数先检查 `exists(finalPath)`，然后创建唯一临时文件，最后调用 POSIX `rename(tempPath, finalPath)`。在 POSIX 上 `rename()` 默认会替换已有目标文件，不会因为目标已存在返回 `EEXIST`。代码中的 `renameErrno == EEXIST` 分支不能实现“不覆盖”。

## 实际风险

如果哈希路径相同，两个并发写入会互相覆盖；若 cover 目录被其它主体写入，可能覆盖对方文件。由于文件名来自图片哈希，不直接允许路径穿越，但 “if absent” 语义不成立。

## 是否可被 fuzzing 命中

普通单进程 fuzz 不易命中。并发 stress test 或 filesystem race test 可命中。

## 修复建议

Linux 上使用 `renameat2(..., RENAME_NOREPLACE)`；或最终目标也用 `open(O_CREAT|O_EXCL)` 后写入；或用硬链接发布 `link(temp, final)` 并根据 `EEXIST` 处理。不要依赖 `rename()` 检测目标存在。

## 7. cover cache 使用非加密 FNV 风格哈希，存在可构造碰撞覆盖/混淆风险

## 风险等级

Low

## 问题类型

File trust issue / Cache collision

## 位置

`src/TagReader.cpp`，`HashEmbeddedImageBytes()` 和 `BuildCoverCachePath()`，约 273-302 行。

## 触发条件

攻击者提供两张不同嵌入图片，但能构造相同 128-bit FNV 风格哈希输出；或未来缩短 hash/path 后风险上升。

## 根本原因

cover path 只由非加密 hash 决定，没有校验已存在文件是否对应当前图片或当前 PNG 输出。`WriteCoverAsPng()` 如果 `exists(coverPath)` 为 true 会直接返回，不验证内容。

## 实际风险

碰撞难度高于普通 64-bit，但 FNV 不是抗碰撞设计。高对抗场景中可能让某个文件返回错误封面路径。不是内存安全问题。

## 是否可被 fuzzing 命中

常规 fuzz 不现实。符号/定向碰撞攻击才可能。

## 修复建议

使用 SHA-256/BLAKE3 等抗碰撞 hash，或在文件存在时验证旁路 metadata/文件内容 hash。至少把函数命名和注释改为非安全缓存 hash，避免误用。

## 8. `DecodeTextToUtf8()` 的 UTF-16 路径缺少输入大小上限

## 风险等级

Medium

## 问题类型

DoS / Missing resource limit

## 位置

`src/TagReader.cpp`，`DecodeTextToUtf8()` 约 4383-4396 行，`TryReadUtf16Text()` 约 1738-1824 行。

## 触发条件

有调用者传入超大 `raw` 并指定 `utf-16le` 或 `utf-16be`。当前主要 parser 对 ID3 tag、MP4 text payload、Vorbis block 有上限，但函数本身是通用内部接口，未来新 parser 很容易绕过限制。

## 根本原因

`DecodeTextToUtf8()` 对 UTF-16 分支没有检查 `raw.size() <= kMaxTextFieldBytes` 或输出增长上限；`TryReadUtf16Text()` `reserve(size)` 并逐字符 append，最坏输出接近输入两倍。

## 实际风险

当前代码路径大多被外层大小限制保护，立即可利用性有限。作为内部通用函数，它缺少局部防线，增加未来格式 parser 的 DoS 风险。

## 是否可被 fuzzing 命中

若 fuzz harness 直接调用内部函数可命中；当前 public `Read()` harness 较难绕过外层上限。

## 修复建议

在 `DecodeTextToUtf8()` 开始统一拒绝 `raw.size() > kMaxTextFieldBytes`，并在 `TryReadUtf16Text()` 中每次 append 前检查 `value.size()` 不超过 `kMaxDecodedTextBytes`。

## 9. `ReadUtf8Text()` 在 data 为 null 且 size 非零时可构造非法 string

## 风险等级

Low

## 问题类型

UB / Defensive coding issue

## 位置

`src/TagReader.cpp`，`ReadUtf8Text()`，约 1727-1735 行。

## 触发条件

内部调用者传入 `data == nullptr` 且 `size > 0`。

## 根本原因

函数没有 null 检查，直接 `std::string(reinterpret_cast<const char *>(data), size)`。当前调用路径通常来自 vector/string_view 或 frame buffer，实际不会传 null+nonzero；但函数本身不满足安全 helper 的局部契约。

## 实际风险

当前 public API 下可利用性低，未来新增 parser 调用时可能造成 UB 或 crash。

## 是否可被 fuzzing 命中

public fuzz 不易命中，unit fuzz 直接调用 helper 可命中。

## 修复建议

在函数开头加入 `if (data == nullptr && size != 0) return {};`。同类 byte reader 也应注明“调用者必须保证长度足够”。

## 10. ID3 frame size 处理对 v2.3 超大 frame 会触发大 vector 分配

## 风险等级

Medium

## 问题类型

DoS / Allocation pressure

## 位置

`src/TagReader.cpp`，`ReadID3v23Or24Frames()`，约 2724-2738 行。

## 触发条件

ID3v2.3 tag size 接近 `kMaxId3TagBytes`，其中一个 frame size 也接近该上限。parser 会把 frame payload 从 `tagBytes` 拷贝到新的 `frameData` vector。

## 根本原因

为了处理 frame flags，函数对每个 frame 都完整复制 payload。tag 已经在 `ReadId3TagBytes()` 中有一份完整 vector，随后 frame 再复制一份，APIC/大文本/未知帧都可能造成峰值内存翻倍。

## 实际风险

内存峰值约为 tag buffer + frame copy + 解码/图片处理 buffer。单文件上限约 16 MiB，通常不是灾难，但批处理或并发读取时可被放大。

## 是否可被 fuzzing 命中

容易，生成最大合法 tag 和单个大 frame 即可观察内存峰值。

## 修复建议

对无需修改的 frame 使用 `std::span`/pointer+size。只有存在 grouping、data length indicator、frame unsync 等需要修改时才复制。未知 frame 应在复制前跳过。

## 11. FLAC picture block 的 `need(n)` 存在 size_t 加法溢出隐患

## 风险等级

Low

## 问题类型

Integer overflow / Defensive coding issue

## 位置

`src/TagReader.cpp`，`ReadFlacPictureEntry()`，约 3318-3363 行。

## 触发条件

`p` 已接近 `SIZE_MAX` 且 `n` 很大时，`p + n` 溢出后错误通过边界检查。

## 根本原因

lambda `need` 使用 `return p + n <= pictureSize;`，没有写成 `n <= pictureSize - p`。当前 `pictureSize` 外层限制为 `kMaxCoverInputBytes`，`p` 只在这个范围内移动，所以实际难以溢出，但局部模式不安全。

## 实际风险

当前输入上限下几乎不可利用。代码模式容易在复制到其它 parser 后造成真实 OOB。

## 是否可被 fuzzing 命中

当前 public fuzz 不会命中溢出，但静态分析会标记。

## 修复建议

改为 `return p <= pictureSize && n <= pictureSize - p;`，并在所有手写 cursor parser 中统一使用该模式。

## 12. Ogg page 总 payload 大小没有显式检查是否超出文件大小

## 风险等级

Low

## 问题类型

Seek/read boundary handling

## 位置

`src/TagReader.cpp`，`ReadOggVorbisCommentEntries()`，约 3138-3175 行。

## 触发条件

Ogg page header 和 segment table 声明的 payloadSize 超过剩余文件大小。

## 根本原因

代码计算 `nextCursor = payloadOffset + payloadSize` 并检查 scan 上限，但没有显式检查 `nextCursor <= context.fileSize`，而是依赖 `ReadRange()` 短读失败。

## 实际风险

不会 OOB，因为 `ReadRange()` 要求完整读取；但错误路径依赖 IO 短读而不是协议边界判断，诊断能力差，fuzz corpus 中大量截断样本会反复触发无意义 seek/read。

## 是否可被 fuzzing 命中

容易。

## 修复建议

在 `ReadRange()` 前添加 `if (nextCursor > context.fileSize) return false;`。其它 parser 中也应优先在 offset 算术层拒绝越界，再执行 IO。

## 13. `ReadLyricsFromPlainText()` 对超多 LRC timestamp token 没有限制

## 风险等级

Medium

## 问题类型

DoS / Unbounded vector growth

## 位置

`src/TagReader.cpp`，`ReadLyricsFromPlainText()`，约 4184-4259 行。

## 触发条件

歌词文本一行包含大量合法 `[mm:ss.xxx]` token，后面跟一段非空歌词。

## 根本原因

每行先把所有 timestamp 放入 `timestamps` vector，再为每个 timestamp 向 `timed` vector 添加一条 lyric。没有每行 token 上限、总 lyric 行上限或总歌词输出上限。上游有 `kMaxLyricsBytes`，但 8 MiB 文本仍可制造几十万条 lyric。

## 实际风险

内存和 CPU 消耗显著，`BuildMusicTag()` 还会再次复制到 `Lyrics`。恶意歌词字段可造成处理延迟或内存压力。

## 是否可被 fuzzing 命中

容易，但需要性能/内存型 fuzz oracle，不一定触发 sanitizer。

## 修复建议

增加 `kMaxLyricLines`、`kMaxLrcTimestampsPerLine`、`kMaxPlainLyricsBytes`。超过限制时停止解析歌词或降级为 plain text。

## 14. timed lyrics 不排序、不去重

## 风险等级

Low

## 问题类型

Logic bug / API behavior issue

## 位置

`src/TagReader.cpp`，`ReadID3v22LyricsFrames()`、`ReadID3v23Or24LyricsFrames()`、`ReadLyricsFromPlainText()`、`BuildMusicTag()`，约 3749-3799、3890-3976、4180-4259、4690-4697 行。

## 触发条件

输入歌词中 timestamp 乱序或重复，或同一行 LRC 有多个 timestamp。

## 根本原因

parser 按文件出现顺序 append timed lines，`NormalizeLyrics()` 只验证文本并删除空行，不排序、不去重。

## 实际风险

调用方拿到的 `Lyrics` 可能不是按时间递增，重复 timestamp 也会原样暴露。不是安全 crash，但会影响播放器行为和 API 预期。

## 是否可被 fuzzing 命中

可通过 property test 命中。

## 修复建议

在 `NormalizeLyrics()` 或 `BuildMusicTag()` 中按 timestamp 稳定排序，并可选去重完全相同的 `(timestamp,text)`。

## 15. Vorbis Comment 严格要求 UTF-8，缺少错误隔离的字段级降级

## 风险等级

Low

## 问题类型

Invalid UTF handling / Compatibility issue

## 位置

`src/TagReader.cpp`，`ReadVorbisCommentEntry()` 约 3219-3235 行，`ReadVorbisLyricsEntry()` 约 4158-4175 行。

## 触发条件

FLAC/Ogg 文件里 Vorbis comment key 或 value 含非法 UTF-8 字节，或现实世界文件错误使用本地编码。

## 根本原因

Vorbis spec 要求 UTF-8，代码严格调用 `DecodeTextToUtf8(..., "utf-8")`，失败后整个 entry 被忽略。没有保留原始字节、没有 latin-1/locale 容错，也没有错误报告。

## 实际风险

安全上是保守选择；兼容性上会丢字段。若用户期望“尽量读取所有标签”，该行为可能被认为是 bug。

## 是否可被 fuzzing 命中

容易。

## 修复建议

保留严格模式作为默认安全行为，同时可在内部统计/报告字段级失败。若要兼容现实文件，可只对 value 做可配置 legacy fallback，但不要违反默认 UTF-8 输出约束。

## 16. `av_log_set_level()` 修改 FFmpeg 全局日志级别，存在跨线程副作用

## 风险等级

Low

## 问题类型

Global state / Multi-threading risk

## 位置

`src/TagReader.cpp`，`TagReader::Read()`，约 816-823 行。

## 触发条件

应用同时使用 TagReader 和其它 FFmpeg 组件，或多线程中其它模块期望不同 FFmpeg log level。

## 根本原因

每次 `Read()` 都调用 `av_log_set_level(AV_LOG_QUIET)`，这是 FFmpeg 进程全局状态，不属于 `ReadContext`。

## 实际风险

不会造成内存破坏，但会静默影响宿主程序其它 FFmpeg 使用者，属于库 API 副作用。多线程下日志级别可能被互相覆盖。

## 是否可被 fuzzing 命中

普通 fuzz 不会命中，需要并发/集成测试。

## 修复建议

不要在库内部无条件修改全局 log level。提供配置选项，或只在测试程序设置 FFmpeg log level。

## 17. `ValidatePath()` 的权限检查存在 TOCTOU 且不能代表实际可读性

## 风险等级

Low

## 问题类型

File trust issue / TOCTOU

## 位置

`src/TagReader.cpp`，`ValidatePath()` 和 `OpenContext()`，约 839-941 行。

## 触发条件

文件在 `exists/is_regular_file/status` 检查和后续 `ifstream.open()` / `avformat_open_input()` 之间被替换，或 ACL/特殊权限导致 mode bits 与实际可读性不一致。

## 根本原因

先用 filesystem metadata 做存在、类型、权限检查，再重新按路径打开文件。路径检查和打开不是原子的。

## 实际风险

本地攻击者可在共享目录中 race 替换文件。库最终还是会打开实际路径并失败/解析另一个文件，不是直接内存安全漏洞，但安全边界中不应把 `ValidatePath()` 视为授权检查。

## 是否可被 fuzzing 命中

普通 fuzz 不会命中，需要 filesystem race test。

## 修复建议

减少预检查，直接打开文件后基于 fd/stat 验证；或者明确文档说明 `ValidatePath()` 只是早期错误提示，不提供安全授权保证。

## 18. MP4 `meta` full box 固定跳过 4 字节但不验证 version/flags

## 风险等级

Low

## 问题类型

Spec violation / Compatibility issue

## 位置

`src/TagReader.cpp`，`ReadMP4AtomTree()` 约 3431-3441 行，`ReadMP4LyricsAtomTree()` 约 4128-4136 行。

## 触发条件

MP4 `meta` atom payload 小于 4 字节、version/flags 异常、或非 iTunes 风格 metadata atom。

## 根本原因

代码遇到 `meta` 就 `payloadOffset + 4` 作为 child offset，不读取也不验证 full box version/flags。若 childOffset 超过 atomEnd，会自然不递归；但 spec 语义没有被确认。

## 实际风险

不会 OOB，因为有 `TryAddUintmax()` 和 `childOffset < atomEnd`。兼容性上可能错误跳过某些非 full-box `meta` 变体，或接受非法 full box。

## 是否可被 fuzzing 命中

容易，但通常表现为字段缺失。

## 修复建议

读取并验证 4 字节 full box header。对已知兼容变体可显式分支，不要隐式固定跳过。

## 19. MP4 `ReadMp4AtomHeader()` 中 `atomSize > uintmax_t max` 检查在部分平台语义不稳

## 风险等级

Low

## 问题类型

Portability / Integer conversion

## 位置

`src/TagReader.cpp`，`ReadMp4AtomHeader()`，约 1986-1990 行。

## 触发条件

在 `std::uintmax_t` 小于 64 位的平台上解析 extended 64-bit atom size。

## 根本原因

`atomSize` 是 `uint64_t`，`std::numeric_limits<std::uintmax_t>::max()` 通常也是至少 64 位；在常见平台没有问题。但随后的 `static_cast<std::uintmax_t>(atomSize)` 依赖前置比较。该代码可移植性尚可，但需要确认所有目标平台。

## 实际风险

在当前 Linux 64-bit 环境无实际风险。在异常平台上可能截断。

## 是否可被 fuzzing 命中

当前平台不可命中。

## 修复建议

保留检查，并在项目支持平台中明确要求 `uintmax_t >= uint64_t`，或统一使用 `uint64_t` 做 MP4 offset，再在 seek 前检查 `streamoff`。

## 20. Ogg parser 不支持 chained streams、多 logical streams 和常见宽松场景

## 风险等级

Low

## 问题类型

Spec compatibility issue

## 位置

`src/TagReader.cpp`，`ReadOggVorbisCommentEntries()`，约 3080-3217 行。

## 触发条件

Ogg 文件包含 chained logical bitstreams、非目标 stream 先出现、sequence discontinuity、合法但复杂的 continuation pattern。

## 根本原因

parser 只跟踪一个 `expectedSerial` 和严格 `sequence == expectedSequence + 1`，且要求 packet continuation 与内部 `packet` 状态完全匹配。它适合最小 Ogg Vorbis comment 提取，但不是完整 Ogg demuxer。

## 实际风险

合法文件可能无法读取 metadata/lyrics。安全上较保守，不会越界。

## 是否可被 fuzzing 命中

容易，特别是 corpus 中加入 chained Ogg 和多 serial pages。

## 修复建议

若目标是广泛兼容，应按 serial 分组 packet assembly，寻找 Vorbis identification/comment packet 所属 logical stream。若目标是安全轻量，至少把限制写入设计文档。

# Structural Weakness

TagReader 是明显 God Object。单个 `src/TagReader.cpp` 同时包含 FFmpeg probe、四种容器 parser、文本解码、图片解码、文件缓存、歌词解析和格式兼容表。局部 helper 很多，但缺少统一的 bounded cursor 类型，导致 FLAC、ID3、MP4、Ogg 都手写 cursor/limit 算术。

parser dispatch 和字段映射耦合在同一个类里。`ReadMetadata()` 和 `ReadLyrics()` 已经做到入口分发，但实际格式 parser 仍共享大量隐式约定，例如 `ReadContext::input` 的状态、`coverExportDir` 为空表示不导出、`RawMetadata` 字段为空表示可被后续 fallback 覆盖。

`ReadContext` 是共享可变状态。它保存 stream、FFmpeg context、容器类型和 cover 目录。当前没有跨线程共享同一个 `ReadContext`，所以没有直接数据竞争；但 `av_log_set_level()` 和 cover cache 文件是进程级/文件系统级共享资源，会产生跨调用副作用。

异常策略混合。路径、FFmpeg probe、无音频流会让 `Read()` 失败；metadata 和 lyrics 的 runtime/filesystem 错误被吞掉。对用户来说“文件可读但字段为空”可能是合法无标签，也可能是 parser 拒绝 malformed block，API 无法区分。

资源限制总体比裸 parser 好：ID3 tag、文本字段、歌词、Ogg packet、Ogg scanned bytes、MP4 atoms、MP4 payload、cover input/output 都有限制。但限制散落在函数中，没有统一 policy，也有内部 helper 缺少自保护。

性能风险主要来自重复解析和重复拷贝。ID3 tag metadata 和 lyrics 各读取/解析一次；FLAC/Ogg Vorbis comments metadata 和 lyrics 也各扫描一次；MP4 metadata 和 lyrics 分两套 walker。大文件中这会造成重复 IO 和 CPU。

封面导出有比较完整的输入大小、像素、输出大小限制，并使用 tmp + fsync 试图保证原子写入；但 `rename()` 的 if-absent 语义错误，并且 hash 非加密。作为缓存尚可，作为安全隔离边界不足。

文本解码策略偏保守，最终 UTF-8 验证较好。风险在于 encoding sniff 对短 legacy 字段的误判、iconv 资源泄漏、以及部分 helper 没有独立大小限制。

# Spec Compliance Issues

## ID3v2.2

支持 3 字节 frame id 和 24-bit frame size，支持 `TT2/TP1/TAL/TP2/TCM/TCO/TYE/TRK/TPA/PIC`，歌词支持 `ULT/SLT`。未完整支持压缩/加密等扩展。`ReadID3Lyrics()` 对 v2.2 要求 `tagView.flags == 0`，这会拒绝带实验标志等非零 flag 的 tag。

## ID3v2.3

frame size 按 big-endian 32-bit 处理，支持 extended header 跳过、grouping identity、拒绝 compression/encryption。未实现 compressed frame 解压、encrypted frame、完整 `COMM`/`TXXX` metadata。对未知帧先文本解码违反 parser dispatch 最小化原则。

## ID3v2.4

frame size 按 syncsafe 处理，支持 footer 排除、extended header 跳过、frame-level unsync、data length indicator、拒绝 compression/encryption。tag-level unsynchronization 的处理顺序有 spec 风险：先对整个 tag payload remove unsync，再解释 extended header/footer/frame region，可能破坏结构 offset。v2.4 的多值文本分隔、genre、date 等只做简化读取。

## FLAC metadata spec

能扫描 metadata block chain，识别 Vorbis Comment block type 4 和 Picture block type 6，block length 24-bit，越界时拒绝。未强制 STREAMINFO 必须为第一个 block，也未利用 STREAMINFO 细节。Picture block 只导出 type 3，不处理其它合法 picture type fallback。`need()` 写法虽在当前上限下安全，但不是最佳 overflow-safe 模式。

## Vorbis Comment spec

按 little-endian vendor length、comment count、comment length 解析。严格要求 UTF-8，符合 spec，但对现实世界非法编码文件兼容性差。没有处理 duplicate fields 的多值策略，基本是 first-wins 或字段已有则忽略。

## Ogg Vorbis spec

能按 page segment table 组 packet，寻找 identification packet 后的 comment packet。实现不是完整 Ogg demuxer：只支持单 serial、严格 sequence 连续、复杂 chained/multi-stream 兼容性不足。不校验 CRC，这对 metadata 读取可接受但不是完整 spec 验证。

## ISO MP4 atom spec

支持 atom size 0、size 1 extended 64-bit、parent limit、atom count limit。`meta` atom 被固定当 full box 跳过 4 字节但不验证 version/flags。`ilst` item 的 child atom 处理不完整，多个 data atom 时可能忽略后续。`data` atom 只读取 type 和 locale/reserved 8 字节中的 type，忽略 locale。`covr` 不根据 data type 13/14 判断 JPEG/PNG，而是直接 sniff payload，兼容但不严格。

# Fuzzing Targets

## 最适合 fuzz 的函数

- `TagReader::Read()`：端到端 fuzz，覆盖 FFmpeg probe、容器识别、metadata、lyrics、cover。
- `ReadId3TagBytes()`、`ReadID3v23Or24Frames()`、`ReadID3v2PictureFrame()`：ID3 size、flags、unsync、APIC 边界。
- `ReadFlacMetadataBlocks()`、`ReadFlacPictureEntry()`：FLAC block length、picture field length、cover payload。
- `ReadOggVorbisCommentEntries()`：segment table、continuation、packet assembly、comment length。
- `ReadMp4AtomHeader()`、`ReadMP4AtomTree()`、`ReadMP4ItemAtom()`、`ReadMP4LyricsAtomTree()`：atom size 0/1、overlap、deep nesting、multi-data item。
- `DecodeTextToUtf8()`、`TryReadUtf16Text()`、`ParseLrcTimestamp()`、`ReadLyricsFromPlainText()`：encoding 和歌词 DoS/property fuzz。
- `WriteCoverAsPng()` / `DecodeAndEncodeCoverPng()`：图片 decoder fuzz，尤其是 APIC/FLAC picture/MP4 covr 包装层。

## 推荐 fuzz 输入

- ID3v2.2/2.3/2.4：valid minimal、truncated header、invalid syncsafe、max tag size、unsync flag、footer、extended header、compressed/encrypted flags、unknown huge binary frame、APIC missing terminator、UTF-16 odd length、surrogate mismatch。
- FLAC：missing STREAMINFO、oversized metadata block、truncated block、Vorbis comment count 超大、comment length 越界、PICTURE mime/desc/image length 边界。
- Ogg：segmentCount 0/255、payload truncated、continued packet without previous packet、multi serial、sequence gap、comment packet before identification、packet size near 8 MiB。
- MP4：atom size 0 at root/non-root、extended size 1 with too-small size、atomEnd outside parent、nested `moov/udta/meta/ilst`、`meta` payload 小于 4、item with multiple `data` atoms、`covr` huge/truncated image。
- Lyrics：一行大量 timestamp、非法 `[mm:ss]`、分钟 65535、秒 60、重复/乱序 timestamp、超大 plain lyrics。
- Encoding：UTF-8 overlong、truncated multibyte、UTF-16 odd byte、lone surrogate、wrong BOM、legacy bytes that iconv candidates都可部分解码。

## 推荐 sanitizer

- ASAN：发现 OOB、use-after-free、FFmpeg wrapper 中潜在内存错误。
- UBSAN：发现整数溢出、无效转换、未定义行为。
- LeakSanitizer：重点捕捉 iconv 句柄泄漏和 FFmpeg object 泄漏。
- libFuzzer value profile：提高长度字段、syncsafe、atom size、timestamp 分支覆盖。

## 推荐编译参数

仓库已有：`cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON`，然后 `cmake --build build-fuzz-clang`。

建议增强：`-fsanitize=address,undefined,leak,fuzzer`、`-fno-omit-frame-pointer`、`-fno-sanitize-recover=all`、`-g`、`-O1`。如果拆出纯 parser fuzz target，可加入 `-fsanitize-coverage=trace-cmp,trace-div,trace-gep`。

## 推荐 fuzz corpus

先运行 `python3 test/corpus/generate_corpus.py` 生成默认 corpus `/tmp/opencode/tagreader_fuzz_corpus`。在此基础上新增：ID3v2.4 tag-level unsync + footer、ID3 huge unknown binary frame、MP4 item multiple data atom、MP4 extended 64-bit atoms、Ogg chained streams、FLAC picture descLen/mimeLen 边界、歌词 timestamp 爆炸样本、非法 UTF-16 surrogate 样本。

# Overall Security Assessment

parser 安全等级：中等。代码整体有较多边界检查、大小上限和异常容错，常见 malformed 输入大概率不会造成越界读写。`ReadRange()`、MP4 parent limit、ID3 tag size、Ogg packet/scan limit、cover decode limit 是主要安全防线。

crash 风险：中低。未发现当前 public `TagReader::Read()` 路径下明确可利用的 OOB write、OOB read 或 UB crash。更现实的 crash/资源风险来自 FFmpeg image decoder 攻击面、iconv 泄漏、超大歌词/ID3 frame 造成内存压力，以及文件系统异常路径。

malformed 文件鲁棒性：中等偏好。多数 parser 在长度不一致时返回 false、break 或抛出后被上层吞掉，不会继续危险读取。但这种容错也会隐藏解析失败，导致字段静默缺失。Ogg/MP4/ID3 的 spec 兼容性不是完整实现，合法复杂文件可能读不到标签。

fuzzing 风险等级：Medium。当前 fuzz 最可能发现的是 DoS、内存泄漏、解析不一致、字段缺失和 sanitizer leak，而不是直接内存破坏。最高价值 fuzz 区域是 ID3 flags/unsync、MP4 atom walker、Ogg packet assembly、封面图片解码和 LRC timestamp 爆炸。

当前代码质量评估：比普通手写二进制 parser 更谨慎，已有统一读取 helper 和多处资源限制；但所有格式集中在一个大型实现文件，重复 walker 和手写 cursor 算术较多，缺少 parser-local bounded cursor 抽象和统一错误模型。建议优先修复 iconv 泄漏、ID3 unknown frame 预解码、MP4 multi-data item、cover rename race，并为歌词行数/UTF-16 输出增加硬限制。
