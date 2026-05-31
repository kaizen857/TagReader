# Architecture Overview

TagReader 当前是单入口 facade：`TagReader::Read(path)` 转到 `TagReader::Read(path, coverExportDir)`，完整主链路为 `ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。

`OpenContext()` 同时建立 `std::ifstream input` 和 FFmpeg `AVFormatContext`。FFmpeg 负责输入 probe、主音频流、容器名和基础媒体信息；标题、歌手、专辑、歌词、封面块等标签字段主要由手写 parser 从 `ReadContext::input` 读取原始字节。

主要数据流是：输入路径进入 `ReadContext`，FFmpeg 产生 `RawMediaInfo`，各格式 parser 产生 `RawMetadata` 和 `RawLyrics`，最后 `BuildMusicTag()` 写入 `MusicTag`。`MusicTag` 是最终 API 对象；解析中间态不直接暴露给调用方。

状态流集中在 `ReadContext`：文件路径、封面导出目录、原始输入流、文件大小、最后修改时间、FFmpeg context、音频流索引、容器识别结果和容器名都挂在同一个对象上。格式 parser 共享同一个 `std::ifstream`，但通用 `ReadRange()` 会在读前后清理 stream 状态，降低 failbit/eofbit 污染后续 parser 的风险。ID3v1 是少数直接 `seekg/read` 的路径，短读时会直接返回。

parser dispatch 逻辑分两层。容器识别优先看文件头签名：`ID3`、`fLaC`、`OggS`、`ftyp`；失败后回退 FFmpeg container name。`ReadMetadata()` 按容器分发：MP4 走 MP4 atom；FLAC/Ogg 走 Vorbis Comment；MP3 先 ID3v2 再 ID3v1 补字段；Unknown 只尝试 ID3v1。`ReadLyrics()` 按同一容器枚举分发到 ID3、Vorbis、MP4 歌词解析。

normalization 流程分为早期解码和最终清理。ID3 文本帧按 encoding byte 解码；ID3v1、MP4 data type 0、freeform 文本走 `DecodeRawText()` sniff；Vorbis Comment 严格按 UTF-8；MP4 metadata 的 data type 1/2/3 分别走 UTF-8、UTF-16BE、UTF-16LE。最后 `NormalizeMetadata()` 和 `NormalizeLyrics()` 再次 trim 并验证 UTF-8，非法字段被清空。timed lyrics 会排序、截断到 `kMaxLyricLines`，并在相同 timestamp 组内去重相同文本。

IO 模型以 bounded read 为核心。`ReadRange()` 检查单次最大读取、`streamoff`/`streamsize` 可表示性、`offset + size` 溢出，并要求 `gcount()` 等于请求长度。MP4 atom walker 使用显式栈和 `kMaxMp4Atoms`，不是递归实现。FLAC picture 使用 `ByteCursor` 做剩余长度检查。封面导出会把图片交给 FFmpeg decoder 转 PNG，再通过同目录临时文件、`fsync()`、`link(temp, final)` 发布到内容寻址缓存路径。

# Supported Formats Matrix

| 类别 | 当前实际支持 |
| --- | --- |
| 容器 | MP3、FLAC、Ogg Vorbis、MP4/M4A；Unknown 只尝试 ID3v1 |
| Metadata | ID3v1/ID3v1.1、ID3v2.2、ID3v2.3、ID3v2.4、FLAC Vorbis Comment、Ogg Vorbis Comment、MP4 `ilst` data atom |
| ID3 metadata 字段 | v2.2：`TT2/TP1/TAL/TP2/TCM/TCO/TYE/TRK/TPA/PIC`；v2.3/v2.4：`TIT2/TPE1/TALB/TPE2/TCOM/TCON/TDRC/TYER/TRCK/TPOS/APIC` |
| Vorbis metadata 字段 | `TITLE`、`ARTIST`、`ALBUM`、`ALBUMARTIST`、`ALBUM_ARTIST`、`COMPOSER`、`WRITER`、`GENRE`、`DATE/YEAR`、`TRACKNUMBER/TRACK/TRACKNUM`、`DISCNUMBER/DISC/DISCNUM` |
| MP4 metadata 字段 | `©nam`、`©ART`、`aART`、`©alb`、`©wrt`、`©gen`、`©day/©dat`、`trkn`、`disk`、`covr` |
| 图片 | ID3v2.2 `PIC`、ID3v2.3/2.4 `APIC`、FLAC `PICTURE` block、MP4 `covr`；只导出 picture type 3/front cover |
| 封面源格式 | PNG、JPEG、BMP、WEBP、GIF、TIFF；未知格式会尝试若干 FFmpeg decoder fallback |
| 歌词 | ID3v2.2 `ULT/SLT`、ID3v2.3/2.4 `USLT/SYLT/TXXX` 部分描述、Vorbis `LYRICS/UNSYNCEDLYRICS/LYRIC/SYLT/SYNCEDLYRICS`、MP4 `©lyr` 和 iTunes freeform lyrics |
| 文本编码 | ID3 encoding 0 Latin-1、1 UTF-16 with BOM fallback LE、2 UTF-16BE、3 UTF-8；Vorbis 严格 UTF-8；MP4 metadata 支持 data type 0/1/2/3 |
| 构建安全工具 | CMake 支持 `TAGREADER_ENABLE_SANITIZERS=ON` 的 ASAN/UBSAN；Clang 下 `TAGREADER_ENABLE_FUZZING=ON` 构建 `TagReaderFuzz` |

# Critical Risk Areas

- `WriteCoverAsPng()` / `AtomicWriteFileIfAbsent()`：封面缓存是唯一在显式传入 `coverExportDir` 后会产生文件系统副作用的标签解析路径。写入侧已有临时文件、`O_EXCL`、`O_NOFOLLOW`、`fsync()`、`link()` 发布防线；剩余风险主要是命中既有路径时的文件信任和错误可观测性。
- `DecodeAndEncodeCoverPng()` / `ConvertImageToPng()`：嵌入图片最终进入 FFmpeg PNG/JPEG/BMP/WEBP/GIF/TIFF decoder。代码有输入大小、像素、输出大小限制，但第三方图片 decoder 仍是最高价值 fuzz 区域。
- `ReadId3TagBytes()` / `ReadID3v23Or24Frames()` / `ReadID3v23Or24LyricsFrames()`：ID3 size、syncsafe、unsync、extended header、footer、frame flags 都集中在这里。当前边界防护较完整，但仍是最容易通过格式组合触发兼容性问题的区域。
- `ReadOggVorbisCommentEntries()`：Ogg page、segment table、packet continuation、serial/sequence 状态全部手写。当前偏严格，内存安全风险低，兼容性 false negative 风险高。
- `WalkMp4IlstItems()` / `ReadMp4AtomHeader()`：MP4 atom size 0/1、64-bit largesize、parent range 和 `meta` full box 都在这里处理。当前显式栈避免了递归栈风险，仍应重点 fuzz size/offset/parent 边界。
- `ReadLyricsFromPlainText()` / `ParseLrcTimestamp()`：LRC 是宽松文本约定，不是强规范二进制格式。代码已有大小、行数、每行 timestamp 数限制；剩余问题主要是方括号注释与 malformed timestamp 的兼容性。

已复核但不再作为当前漏洞列出的旧风险：iconv 句柄泄漏、未知 ID3 帧先文本解码造成 DoS、MP4 metadata 递归 walker、多 `data` atom 只处理第一个、cover cache `rename()` 覆盖、UTF-16 输出无上限、`ReadUtf8Text()` null+nonzero UB、FLAC picture 游标加法溢出、Ogg payload 越界只依赖短读、timed lyrics 不排序去重。这些点在当前代码中已有对应修复或防护。

# Bug Report

## 1. cover cache 命中既有路径时不校验内容

## 风险等级

Medium

## 问题类型

File trust issue / Cache poisoning

## 位置

`src/TagReader.cpp`，`HashEmbeddedImageBytes()` 约 331 行，`BuildCoverCachePath()` 约 353 行，`WriteCoverAsPng()` 约 876-916 行，尤其 `std::filesystem::exists(coverPath)` 为真时直接返回。

## 触发条件

调用方把 `coverExportDir` 指向可被其它用户、进程或历史版本写入的目录；攻击者或旧缓存预先创建了由当前嵌入图片 hash 推导出的 `<coverExportDir>/<2hex>/<rest>.png`。更极端的情况是自定义非加密 hash 出现碰撞。

## 根本原因

封面缓存是内容寻址路径，但命中时只检查路径存在，不验证文件类型、是否 symlink、内容是否真是当前嵌入图片转码得到的 PNG，也不存储 sidecar 元数据。写入侧已经避免覆盖，但读取命中侧仍信任既有文件。

## 实际风险

API 可能返回攻击者预置或过期的 `coverPath`。如果上层会展示、上传、复制或信任这个路径，就可能出现封面混淆、缓存投毒或本地文件信任问题。对私有应用缓存目录风险较低；对共享目录或服务端临时目录风险更高。

## 是否可被 fuzzing 命中

普通单输入 fuzz 不容易命中，因为需要预置文件系统状态。带状态 harness 可以稳定命中：先根据样本图片计算路径并预置文件或 symlink，再调用 `TagReader::Read(path, coverExportDir)`。

## 修复建议

文档和 API 明确要求 `coverExportDir` 必须是应用私有目录。代码命中既有路径时使用 `symlink_status()` 拒绝 symlink/非普通文件；必要时记录 sidecar hash 或回读 PNG 做内容校验。若要强化碰撞抗性，改用 SHA-256/BLAKE3 等抗碰撞摘要。

## 2. 显式封面导出失败被 metadata 容错层吞掉

## 风险等级

Medium

## 问题类型

Exception safety / API observability issue

## 位置

`src/TagReader.cpp`，`ReadMetadata()` 约 2802-2856 行，`ignoreMalformedMetadata` 捕获 `std::filesystem::filesystem_error` 和 `std::runtime_error`；封面写入路径 `WriteCoverAsPng()` 约 876-916 行。

## 触发条件

调用方显式传入 `coverExportDir`，文件含有可导出封面，但导出过程中出现权限变化、磁盘满、目录被替换、`fsync` 失败、`link()` 失败或其它文件系统错误。

## 根本原因

metadata parser 将 malformed 标签错误和 cover export 的文件系统错误放入同一个“可忽略 metadata failure”通道。异常被吞掉后，`Read()` 继续返回 `MusicTag`，只是 `coverPath` 为空。

## 实际风险

调用方无法区分“音频没有封面”“封面无法解码”“缓存目录不可写或被破坏”。批处理、服务端索引或安全敏感导出流程会隐藏真实故障，降低可观测性和运维诊断能力。

## 是否可被 fuzzing 命中

输入 fuzz 不容易命中；文件系统 fault injection、只读目录、满磁盘、并发替换目录或权限切换可以命中。

## 修复建议

继续吞掉 malformed tag 解析错误，但把 cover export 的 filesystem/runtime 错误单独建模。调用方显式传入 `coverExportDir` 时，建议让不可写/发布失败向上抛出，或在返回对象/日志中暴露可诊断状态。

## 3. fuzz target 固定 `/tmp` 路径且 public API gate 过强

## 风险等级

Medium

## 问题类型

Fuzzing coverage issue / Test isolation issue

## 位置

`test/fuzz/tagreader_fuzz.cpp`，`FuzzRoot()` 约 11-14 行固定为 `/tmp/tagreader_fuzz`；`CleanupFuzzFiles()` 约 26-31 行每轮删除共享输入和 cover 目录；核心调用在约 53-74 行。

## 触发条件

libFuzzer 使用 `-jobs`/`-workers` 或多个 fuzz 进程并行；或者随机输入无法通过 `OpenContext()` 的 FFmpeg probe 和 `DetectStream()` 音频流检测。

## 根本原因

harness 所有进程共享同一个输入路径和 cover 目录，互相删除状态。并且 fuzz 只调用 public `TagReader::Read()`，而 `Read()` 在手写 parser 之前要求 FFmpeg 能打开文件并找到音频流，导致大量随机 ID3/FLAC/Ogg/MP4 parser payload 根本进不到目标分支。

## 实际风险

这不是产品运行时漏洞，但会让 fuzz 覆盖率失真。端到端 fuzz 可能主要覆盖 FFmpeg 打开失败路径，而非本库手写 parser。并行 fuzz 还可能互相删除输入/cover 目录，制造噪音或遗漏文件系统竞态。

## 是否可被 fuzzing 命中

该问题本身在并行 fuzz 下可稳定暴露；覆盖不足需要通过 coverage report 或 corpus minimization 观察。

## 修复建议

使用包含 pid/thread id/随机后缀的 per-worker 临时目录。增加能通过 FFmpeg probe 的最小 MP3/FLAC/Ogg/MP4 seed。若允许测试专用入口，可增加 parser-level harness 或构造“合法音频外壳 + 变异标签 payload”的 corpus。

## 4. ID3v2.2 lyrics 对任意非零 tag flags 直接拒绝

## 风险等级

Low

## 问题类型

Spec violation / Logic bug

## 位置

`src/TagReader.cpp`，`ReadID3Lyrics()` 约 4039-4058 行；v2.2 分支中 `tagView.flags != 0` 直接 return。`ReadId3TagBytes()` 约 3064-3068 行已对 v2.4 以前的 tag-level unsync 做全 tag 反同步处理。

## 触发条件

ID3v2.2 tag 带 unsynchronization flag 或其它非零 flag，并包含 `ULT` 或 `SLT` 歌词帧。

## 根本原因

metadata 分支和 tag 读取分支已经能处理部分 v2.2/v2.3 tag-level unsync，但 lyrics 分支又用原始 flags 做“一票否决”。这导致同一 tag 的 metadata 可解析、lyrics 被静默丢弃。

## 实际风险

合法或常见变体 ID3v2.2 歌词缺失。不是内存安全问题，是兼容性和 parser 一致性问题。

## 是否可被 fuzzing 命中

很容易。构造带非零 flags 的 ID3v2.2 tag，并放入最小 `ULT` 或 `SLT` frame。

## 修复建议

按 flag 位精确判断。已由 `ReadId3TagBytes()` 处理过的 unsync 不应在 lyrics 层再次拒绝；真正不支持的压缩、加密或未定义 flag 才跳过或拒绝。

## 5. MP4 lyrics 不支持 UTF-16 data atom，metadata 与 lyrics 解码能力不一致

## 风险等级

Low

## 问题类型

Spec compatibility issue / Inconsistent decoding

## 位置

`src/TagReader.cpp`，`DecodeMp4TextData()` 约 3909-3943 行支持 data type 2/3；`ReadMP4LyricsItem()` 约 4848-4875 行只接受 data type 0/1；`ReadMP4FreeformLyricsItem()` 约 4877-4943 行也只处理 0/1。

## 触发条件

MP4 `©lyr` 或 iTunes freeform lyrics 的 `data` atom 使用 UTF-16BE/UTF-16LE data type。

## 根本原因

metadata 文本解码集中在 `DecodeMp4TextData()`，lyrics 路径没有复用它，而是手写只接受 UTF-8/raw sniff 的分支。

## 实际风险

合法 UTF-16 MP4 歌词会被忽略。metadata 和 lyrics 对相同 MP4 text data 的支持面不一致。

## 是否可被 fuzzing 命中

容易。构造 `moov/udta/meta/ilst/©lyr/data`，data type 2 或 3，payload 为合法 UTF-16 文本。

## 修复建议

让 MP4 lyrics 路径复用 `DecodeMp4TextData()`。对 `©lyr` 和 freeform lyrics 同时支持 data type 2/3，并保留 size 上限。

## 6. Ogg Vorbis comment parser 只支持简单单 logical stream

## 风险等级

Low

## 问题类型

Spec compatibility issue

## 位置

`src/TagReader.cpp`，`ReadOggVorbisCommentEntries()` 约 3560-3700 行；单一 `expectedSerial` 和严格 `sequence == expectedSequence + 1` 检查集中在约 3571-3605 行。

## 触发条件

Ogg 文件包含 chained logical bitstreams、多 serial pages、前置非目标 logical stream、sequence wrap-around、sequence gap 或更复杂 continuation。

## 根本原因

实现按一个 serial 串行拼 packet，不为多个 logical stream 建立状态，也不选择“包含 Vorbis identification/comment packet 的目标 stream”。这是一种安全保守但兼容性有限的轻量实现。

## 实际风险

合法复杂 Ogg 文件读不到 metadata/lyrics。不会造成越界读写，主要是 false negative。

## 是否可被 fuzzing 命中

很容易。变异 Ogg serial、sequence、continuation flag、segment table 或 chained stream 即可触发。

## 修复建议

若目标是广泛兼容，按 serial 建立 packet assembly 状态，并选择包含 Vorbis identification/comment packet 的 logical stream。若目标保持轻量，文档化“仅支持简单单流 Ogg Vorbis”。

## 7. LRC parser 会丢弃带方括号注释的 plain text 行

## 风险等级

Low

## 问题类型

Malformed text handling / Logic bug

## 位置

`src/TagReader.cpp`，`ReadLyricsFromPlainText()` 约 4477-4580 行；`ParseLrcTimestamp()` 约 4945-5000 行。

## 触发条件

歌词文本包含类似 `[Verse 1]`、`[Chorus]`、`[abc:def]`、或混合合法/非法 timestamp token 的行。

## 根本原因

只要行首出现 `[`，parser 就尝试按 LRC timestamp token 扫描。任意 token 解析失败后设置 `skippedInvalidTimestampLine`，该行不会再进入 plain text 兜底，最终可能整行丢失。

## 实际风险

有效歌词文本缺失或被截断。不会造成资源耗尽，因为已有 `kMaxPlainLyricsBytes`、`kMaxLyricLines`、`kMaxLrcTimestampsPerLine`。

## 是否可被 fuzzing 命中

很容易。普通文本歌词字段即可构造。

## 修复建议

只有在至少成功解析出一个 timestamp 时才把该行归类为 timed line。没有合法 timestamp 的方括号行应保留为 plain text，或把 bracket 注释作为普通文本处理。

## 8. MP4 size=0 atom 可提前终止同级扫描并隐藏后续 metadata

## 风险等级

Low

## 问题类型

Spec edge case / Parser recovery issue

## 位置

`src/TagReader.cpp`，`ReadMp4AtomHeader()` 约 2161-2236 行，`ForEachMp4ChildAtom()` 约 2288-2333 行，`WalkMp4IlstItems()` 约 2366-2450 行。

## 触发条件

MP4 文件根层出现 size=0 atom，且畸形文件在该 atom 后继续放置 metadata sibling 或其它字节。

## 根本原因

MP4 规范允许 size=0 表示延伸到文件末尾。当前只有 root 层允许 size=0；`ForEachMp4ChildAtom()` 遇到 `atom.atomSize == 0` 后直接返回 `Ok`，按规范把剩余扫描范围视作该 atom 占用，不做恢复性 sibling 扫描。

## 实际风险

畸形或对抗性文件可以利用 root size=0 atom 让 parser 按规范停止同级扫描，后续伪造 sibling metadata 不再被读取。不是内存安全问题，因为 parent range 和 progress 都有检查。

## 是否可被 fuzzing 命中

很容易。MP4 atom size 字段是高价值 fuzz 目标。

## 修复建议

区分“规范允许的到 EOF/root 终止”与“畸形文件试图在 size=0 atom 后继续放 sibling”。如果需要恢复性扫描，应显式记录 malformed 状态；否则至少在 fuzz corpus 中保留此类样本，确认行为稳定。

# Structural Weakness

`src/TagReader.cpp` 仍是 God Object：FFmpeg probe、容器识别、ID3/FLAC/Ogg/MP4 parser、文本解码、图片转码、cover cache、歌词处理和最终构建都在一个文件中。已有 `ReadRange()`、`ByteCursor`、`WalkMp4IlstItems()`、`IconvHandle` 等局部抽象，但格式间的边界策略仍靠人工约定维持。

`ReadContext` 是共享可变状态，所有 parser 共用同一个输入流和封面导出目录。通用读函数降低了 stream 状态污染风险，但直接使用 `context.input` 的路径仍要求维护者记得 `clear()`、边界检查和异常策略。

错误模型混合：路径无效、FFmpeg 打不开、无音频流会让 `Read()` 失败；metadata/lyrics 的 `runtime_error` 和 `filesystem_error` 往往被吞掉。这个模型适合“尽量读取可用字段”，但会隐藏封面导出失败、parser 拒绝 malformed 和真正没有标签之间的差异。

metadata 和 lyrics 多次解析同一 tag 或 atom tree。MP3 会为 metadata 和 lyrics 分别读取 ID3 tag；FLAC/Ogg 分别扫描 Vorbis Comment；MP4 metadata 和 lyrics 分别走 `WalkMp4IlstItems()`。当前有大小上限，资源风险可控，但批处理和 fuzz 下会增加 IO/CPU。

API 设计只返回 `MusicTag`，没有携带诊断信息。调用方无法获知字段缺失原因、封面失败原因、容器兼容性失败原因或 encoding 失败原因。

`TASKS.md` 是安全修复路线图而不是当前实现规范。本报告已把路线图里已经落地的 P0/P1 风险降级为“已复核但不再作为当前漏洞”，例如 iconv RAII、ID3 未知帧预过滤、MP4 显式栈 walker、cover cache if-absent 发布、UTF-16 输出上限、FLAC `ByteCursor`、Ogg 文件边界检查、歌词行数/排序/去重。仍未完成或仅部分覆盖的项目体现在当前 Bug Report 和 Fuzzing Targets 中：cover cache 命中验证、cover 导出错误可观测性、Ogg 复杂流兼容、MP4 lyrics UTF-16、LRC 方括号兼容、fuzz harness 隔离和 parser 覆盖。

# Spec Compliance Issues

- ID3v2.2：基础文本、`PIC`、`ULT/SLT` 可读；lyrics 对任意非零 tag flags 直接拒绝，与 metadata 的 tag-level unsync 处理不一致。
- ID3v2.3/v2.4：当前拒绝压缩/加密 frame，不完整支持 `COMM`、复杂 `TXXX` metadata、多值文本语义和 appended tag/footer 搜索。安全上偏保守，兼容性有限。
- ID3 unsynchronization：当前 v2.3 前 tag-level unsync 和 v2.4 frame/tag unsync 均有处理，未见明确 OOB；仍建议把 tag-level 和 frame-level unsync 的边界样本加入 fuzz corpus。
- FLAC metadata：metadata block length 和 `PICTURE` 字段长度有边界检查；只导出 picture type 3，不做其它合法 picture type fallback；Vorbis Comment 多 block 语义未显式建模。
- Vorbis Comment：严格 UTF-8 符合 spec，但现实世界 legacy 编码会丢字段；duplicate/multi-value 采用 first-wins 或忽略 total 字段，不是完整标签语义实现。
- Ogg Vorbis：不校验 CRC，只支持简单单 logical stream；chained/multi-stream、sequence wrap-around、复杂 continuation 兼容性弱。
- MP4/M4A：atom size、largesize、parent range 和显式栈防线较稳；主要支持 iTunes-style `moov/udta/meta/ilst`，不覆盖所有 QuickTime/ISO metadata path。`©lyr` 与 freeform lyrics 解码能力弱于 metadata。
- MP4 artwork：`covr` payload 依赖图片 magic/FFmpeg sniff，而不是严格依赖 data type 13/14；安全上有 decoder 限制，spec 语义上较宽松。
- LRC：没有强制国际规范。当前实现把方括号 token 当时间戳优先处理，导致注释行兼容性差。

# Fuzzing Targets

最适合 fuzz 的函数：

- `TagReader::Read()`：端到端 fuzz，覆盖 FFmpeg probe、容器识别、metadata、lyrics、cover，但 seed 必须能通过 FFmpeg 音频流 gate。
- `ReadId3TagBytes()`、`ReadID3v23Or24Frames()`、`ReadID3v23Or24LyricsFrames()`、`ReadID3v2PictureFrame()`：ID3 size、syncsafe、unsync、extended header、footer、frame flags、APIC terminator。
- `ReadFlacMetadataBlocks()`、`ReadFlacPictureEntry()`：FLAC block length、Vorbis Comment length/count、PICTURE mime/description/image length。
- `ReadOggVorbisCommentEntries()`：Ogg segment table、continuation、sequence/serial、多 page packet、packet size 上限。
- `ReadMp4AtomHeader()`、`ForEachMp4ChildAtom()`、`WalkMp4IlstItems()`、`ReadMP4ItemAtom()`、`ReadMP4LyricsItem()`：atom size 0/1、64-bit largesize、parent range、fullbox version、multi-data item。
- `DecodeTextToUtf8()`、`TryReadUtf16Text()`、`ReadLyricsFromPlainText()`、`ParseLrcTimestamp()`：UTF-8/UTF-16、legacy bytes、LRC timestamp 和歌词行数。
- `DecodeAndEncodeCoverPng()`、`WriteCoverAsPng()`：图片 decoder、cover cache 命中/发布、文件系统状态。

推荐 fuzz 输入：

- ID3v2.2/v2.3/v2.4 minimal valid、truncated header、invalid syncsafe、max tag size、tag/frame unsync、footer、extended header、compressed/encrypted flags、APIC missing terminator、UTF-16 odd length、lone surrogate。
- FLAC Vorbis Comment：vendor length 超限、field count 超大、field length 截断、非法 UTF-8、非法 field name、多个 comment block。
- FLAC PICTURE：mimeLen/descLen/imageLen 截断或超限、picture type 不是 3、`-->` URL picture、图片 payload 接近上限。
- Ogg：`255,0` lacing、零长度 packet、跨页 packet、sequence gap/wrap、multi serial、chained stream、page_segments 与 payload 不一致。
- MP4：size 0、size 1 largesize、小于 header 的 size、child 越过 parent、deep nesting、`meta` version 非 0、多 `data` atom、UTF-16 `©lyr`、freeform lyrics。
- LRC：`[Verse]`、`[abc:def]`、同一行超过 32 timestamp、超过 20000 行、合法/非法 token 混合。
- Cover：PNG/JPEG/BMP/WEBP/GIF/TIFF 截断、错误 magic、超大尺寸声明、输出接近 64 MiB、预置 cover cache 路径。

推荐 sanitizer 和编译参数：

- 普通 ASAN/UBSAN：`cmake -S . -B build-asan -DTAGREADER_ENABLE_SANITIZERS=ON`，然后 `cmake --build build-asan`。
- libFuzzer：`cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON`，然后 `cmake --build build-fuzz-clang`。
- corpus 生成：`python3 test/corpus/generate_corpus.py`，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`。
- 建议 libFuzzer 加 value profile，以提高长度字段、syncsafe、atom size、timestamp 分支覆盖。

现有测试资产分工：

- `TagReaderTest` 是人工字段打印程序：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`，适合验证单个真实样本的字段、歌词和封面路径。
- `TagReaderSecuritySmoke` 是安全 smoke 程序：`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [audio-file-path ...]`。当前已包含 cover cache smoke：重复读取同一文件时要求 `coverPath` 不变、mtime 不变；并发 8 个 async 读取时也要求路径不变、mtime 不变。
- `test/security/generate_samples.py` 生成 `/tmp/opencode/tagreader_security_samples` 下的安全 smoke 样本。它会在 ffmpeg CLI 可用时生成 audio-backed MP3/M4A/Ogg，并额外生成 ID3 APIC、USLT LRC、invalid LRC、declared 32 MiB ID3、deep MP4、MP4 lyrics atom、Ogg continuation 等 parser-target 样本。
- `test/corpus/generate_corpus.py` 生成 deterministic fuzz corpus，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`，分类包括 id3、flac、ogg、mp4、image、encoding；仓库不提交二进制 seed。

推荐 corpus 策略：

- 保留当前 deterministic corpus，但增加“能通过 FFmpeg probe 的合法音频外壳 + 变异标签 payload”。
- 为 `TagReaderFuzz` 使用 per-worker 临时目录，避免并行 fuzz 共享 `/tmp/tagreader_fuzz`。
- 增加文件系统状态型 harness：预置 cover cache 文件、symlink、不可写目录、满目录、并发读同一封面。
- 若允许测试专用入口，增加 parser-level harness 来绕过 FFmpeg gate，直接覆盖 ID3/FLAC/Ogg/MP4 边界函数。

# Overall Security Assessment

当前 parser 安全等级：中等偏稳健。代码对常见 malformed 输入有较多边界检查、大小上限和异常容错；当前 public `TagReader::Read()` 路径下未发现明确可利用的 OOB read、OOB write 或稳定 UB crash。

crash 风险：中低。主要 crash/资源风险来自 FFmpeg 图片 decoder 攻击面、文件系统异常路径、以及未来维护时绕过现有 bounded helper。ID3、FLAC、Ogg、MP4 主解析路径都有文件边界和大小上限。

malformed 文件鲁棒性：较好但偏静默。许多 metadata/lyrics malformed 会被吞掉并降级为空字段，避免崩溃，但也隐藏了错误来源。对 Ogg chained/multi-stream、MP4 非 iTunes metadata path、ID3 复杂 flags 组合等合法复杂结构，兼容性有限。

fuzzing 风险等级：中等。现有 fuzz target 能端到端调用 public API，但 FFmpeg probe/音频流 gate 会挡掉大量随机 parser payload，固定 `/tmp` 路径也不适合并行 fuzz。需要更强 corpus 和隔离策略才能覆盖手写二进制 parser。

当前代码质量评估：核心安全防线已经明显存在，包括 `ReadRange()`、`ByteCursor`、显式 MP4 walker、ID3 frame 预过滤、UTF-16/文本大小上限、cover decode limits、原子 cover 发布。剩余工作重点不应是大规模重写，而是补强文件系统信任边界、错误可观测性、fuzz harness 覆盖和格式兼容性说明。
