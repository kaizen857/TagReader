# TagReader 设计文档

> 本文档由源码与构建配置逆向重建，源码（`include/`、`src/`、`test/`、`CMakeLists.txt`、`CMakePresets.json`）为唯一可信来源；`README.md` 与历史文档不作为依据，冲突时以源码为准。
> 文中标注"无法确认"之处为源码中无法直接证明、需外部规范或设计意图确认的内容；这类段落只描述**观察到的行为**，不构成推荐做法，也不构成规范背书。

## 1. 项目简介

TagReader 是一个 C++23 实现的音频标签读取库：

- 从常见音频容器中读取元数据、歌词与封面，统一输出 `MusicTag`；文本终态一律 UTF-8。
- 封面导出为内容寻址 PNG 缓存（全尺寸 + 缩略图），并支持同目录 sidecar 图片回退。
- 附命令行演示工具 `TagReaderTest` 与基于 Catch2 的测试套件。
- 是库，无独立启动流程；读取功能经 `Read` / `ReadCueSheet` 两个入口触发，另有独立封面导出入口 `ExportFolderCover`（只查找并导出文件夹自身目录的封面图像，不读音频标签，见 4.1）。

技术要点：

- 标签解析全部由原始字节 parser 完成（不使用 FFmpeg 的 `AVDictionary`）；FFmpeg 仅负责 probe、音频流与基础媒体信息、封面解码/像素转换。PNG 编码使用 fpng（`third_party/fpng`，x86 下启用 SSE4.1/PCLMUL 编译选项）。
- 依赖：FFmpeg（libavformat/libavcodec/libavutil/libswscale，经 pkg-config 解析）；Iconv（默认必需，仅显式构建选项允许回退）；Catch2（系统包优先，缺失时 FetchContent 拉取）；可选 Tracy 性能分析（`TAGREADER_ENABLE_PROFILING`）。

## 2. 整体架构

分层结构（依赖自上而下）：

```
公共 API 层      include/TagReader.hpp（Read / ReadCueSheet / ExportFolderCover）
转发层           src/TagReader.cpp（只转发与薄适配，不含解析逻辑）
核心管线         src/core/（ReadTag 固定流程、ReadContext、RawMetadata/RawLyrics）
支撑层           src/media/  src/io/  src/text/  src/cover/  src/common/  src/platform/
格式解析层       src/formats/<fmt>/（原始字节 parser，12 个格式目录 + common/）
CUE 管线         src/formats/cue/（独立于 Read()，每个被引用音频文件复用核心管线）
```

架构约定（全部经源码核实）。**下列各条均为不可破坏的红线**：破坏其中任何一条，都会使当前设计的前提失效，而不只是"实现变丑"：

- **解析器与媒体层分工（红线）**：标题/歌手/专辑/歌词/封面块**必须**由各格式 parser 用原始字节解析；FFmpeg 只做 probe、音频流选择、基础媒体信息与封面解码/像素转换，**PNG 编码由 fpng 完成（`third_party/fpng`）而不是 FFmpeg**。**不得改用 FFmpeg 的 `AVDictionary` 读取标签**——容器的容错与优先级规则（first-wins、APE 优先于 ID3 等）由 parser 自己掌握，交给 FFmpeg 会同时丢掉这份控制权和三端行为一致性。
- **共享输入与有界读取（红线）**：所有 parser 共用 `ReadContext`（`src/core/ReadContext.hpp`）中的 `input`（`tagreader_io::FileInput`，见 `src/io/ByteReader.hpp`）与 `fileSize`；二进制访问**必须**使用绝对 offset + 有界读取（`ReadRange`/`ReadRangeAt`），**不得依赖或污染流位置**——多个 parser 会先后复用同一个文件描述符，任何"读到哪了"的隐式状态都会让后续 parser 从错误位置读取。
- **中间态与终态分离（红线）**：解析结果先写入 `RawMediaInfo`/`RawMetadata`/`RawLyrics`（`src/core/RawTagData.hpp`），由 `NormalizeMetadata()`/`NormalizeLyrics()`（`src/text/TextNormalize.cpp`）收口为 UTF-8 后组装 `MusicTag`；`MusicTag` 的文本终态**必须是 UTF-8**，parser 不得把未收口的文本直接写进 `MusicTag`。
- **封面导出副作用（红线）**：所有格式 parser **必须**通过 `ExportCoverFromContext`（`src/cover/CoverCache.hpp`）把封面数据交给统一缓存管线，返回 `CoverPaths{fullSizePath, thumbnailPath}`；**parser 内不得自行写文件、不得绕过缓存管线**。
- **失败粒度（红线）**：格式 parser 全部返回 void、解析失败静默停止；局部 malformed 的元数据或歌词字段只跳过或清空局部结果。**只有输入不可用、无音频流、上下文/容器无法建立，才允许普通读取在顶层失败**（完整清单见 6.3）；封面失败另走 `failurePolicy`（见 4.7、6.2）。
- **资源上限集中管理（红线）**：常量集中在各 parser 文件顶部的 `kMax*` 与 `include/TagReaderInternal.hpp` 的 `CoverDecodeLimits`；所有 `ReadRange` 都必须带上限，**既有上限不得放宽**（见 4.5、9）。
- **公共头边界（红线）**：`include/` 是公共契约面，**不得暴露平台类型、平台宏或条件编译产物**。注意 `include/TagReaderInternal.hpp` 虽然名为 internal、只服务于库内部，但它位于公共 include 目录，且被 `install(DIRECTORY include/)` 一并安装，因此同样受此约束（当前只含 `<cstddef>`/`<cstdint>` 与常量结构体）。
- **转发层与 CUE 隔离（红线）**：`src/TagReader.cpp` 只做转发与薄适配，**不得在其中加入解析逻辑**；CUE **不得并入 `Read()` 管线**，必须始终走独立的 `tagreader_cue::ReadCueSheet()`（见 4.2、4.9）。

### 2.1 跨平台与消费形态约束

跨平台对本库是**硬约束**，不是"发布前再适配"：

- **三端必须可配置、可构建、可测试**：Windows / Linux / macOS 三端都要能独立配置、构建并通过测试。各端具体工具链与打包形态由消费方的构建配置决定，不在本文档范围。
- **两种消费形态并存**：本库既作为独立仓库被消费，也作为被 `EXCLUDE_FROM_ALL` 嵌入的 vendored 子树被消费。因此源码**不得假设自己是顶层工程**——例如不得假设"测试目标一定存在"，也不得假设自己一定位于 `binaryDir` 根部。
- **文件系统一律 `std::filesystem`**：**不得**在共享代码里裸用 POSIX-only 调用（`unistd`/`dirent`/`::realpath` 等）或 Windows-only API。确需平台文件描述符语义（`pread`、`O_NOFOLLOW`、`flock`、私有权限硬化）时，必须收敛在既有平台边界内。
- **既有平台边界**：`src/platform/PosixCompat.hpp` 是统一兼容层——Windows 分支把 POSIX fd API 映射到 CRT 对应物（`_wopen`/`_read`/`_lseeki64`/`_fstat64`/`_locking` 等）并补齐 `ssize_t`/`S_ISREG`/`O_CLOEXEC`/`flock` 语义，其余平台直通系统头。此外 `src/media/FfmpegSession.cpp`、`src/core/TagPipeline.cpp`（默认封面目录硬化）、`src/cover/CoverCache.cpp`、`src/io/ByteReader.cpp`、`src/formats/cue/CueReader.cpp` 各自持有少量条件编译分支。**新增平台分支必须保证其余平台仍可配置构建，并且不得把平台条件散布到公共头。**
- **新增依赖必须三端可供给**：FFmpeg/Iconv 经 pkg-config 解析（Windows 由 vcpkg 提供，Linux/macOS 走系统包或 Homebrew）；无法覆盖三端的能力应经 CMake 条件关闭或降级，而不是阻塞其它平台。
- **本机无法验证另一平台的改动**：至少在提交信息中说明受影响面与验证方式——平台相关改动不得只凭单一平台通过就合入。

## 3. 项目目录

```
include/
  TagReader.hpp           公共 API（Read/ReadCueSheet、CoverProcessingOptions、CoverProcessingError、CoverErrorCode）
  Tag.hpp                 MusicTag
  Lyrics.hpp              Lyrics / Lyric
  TagReaderInternal.hpp   内部共享声明（CoverDecodeLimits 等）
src/
  TagReader.cpp           仅转发：Read → tagreader_core::ReadTag；ReadCueSheet → tagreader_cue::ReadCueSheet
  common/ParseHelpers.hpp 通用解析辅助（ParseYearOnly、ToLower、IEquals、ParseSlashNumber）
  core/                   ReadContext.hpp、RawTagData.hpp、TagFormat.hpp、TagPipeline.hpp/.cpp、CoverBudget.hpp、CoverErrorPolicy.hpp
  media/                  FfmpegSession、ContainerDetector、MediaInfoReader
  io/ByteReader.hpp/.cpp  FileInput、ReadRange、大小端读取原语（bounded::ReadRangeAt 位于 formats/common/BoundedReader）
  text/                   TextCodec、TextNormalize
  cover/                  CoverCache、CoverDecoder、SidecarCover
  profiling/              Profiling.hpp（TAGREADER_PROFILE_FUNCTION）、TracyClient.cpp
  platform/               PosixCompat.hpp（POSIX fd API 兼容层：Windows 映射到 CRT，其余平台直通）
  formats/
    common/               BoundedReader（bounded::ReadRangeAt / MakeBoundedRange / ReadU16Le 等）
    id3/ ape/ vorbis/ flac/ ogg-vorbis/ opus/ mp4/ matroska/ asf/
    riff/ aiff/ dsd/      RIFF/WAV、AIFF/AIFC、DSF/DFF
    cue/                  CUE 独立管线（CueReader、CueParser、CuePathResolver、CueTextLoader、CueTiming）
third_party/fpng/         fpng（PNG 编码，x86 启用 -msse4.1 -mpclmul / /arch:AVX）
test/
  main.cpp                TagReaderTest 人工 CLI 入口（Catch2 main 由 Catch2::Catch2WithMain 提供）
  catch2/                 smoke_test.cpp、lyrics_normalize_complexity_catch2_tests.cpp
  regression/             各 *_catch2_tests.cpp（活跃用例）+ 支持/夹具 + regression_tests.cpp（被 tr-audit 测试以 #include 方式文本包含编译，提供 RunTrAudit* 实现）
  security/               generate_samples.py、security_smoke.cpp
  fuzz/                   tagreader_fuzz.cpp（仅 Clang/libFuzzer）
  corpus/                 generate_corpus.py（fuzz 语料生成）
  CMakeLists.txt
CMakeLists.txt            库目标 TagReaderCore（STATIC）
CMakePresets.json         default / release / sanitize / fuzz / profile
```

## 4. 模块说明

### 4.1 公共 API（include/TagReader.hpp）

- `Read` 与 `ReadCueSheet` 各有 3 个重载：`(path)`、`(path, coverExportDir)`、`(path, coverExportDir, CoverProcessingOptions)`。
- `ExportFolderCover(folderPath, coverExportDir, options)`：独立封面导出入口，只查找 `folderPath` 自身目录中的 `cover`/`front`/`folder`/`album`/`artwork` 图像（名称/扩展名规则、档位优先级与 `Read` 的 sidecar 回退逐字节同源，复用同一内容寻址 PNG 缓存管线，不递归子目录、不查父目录）；返回仅含 `coverPath`/`thumbnailPath`（按 options 模式）的 `MusicTag`；无候选或全部失败返回路径为空的 `MusicTag`，不抛错（`CoverProcessingError` 一律被吞并转空结果）。
- `CoverProcessingOptions` 共 8 个字段：`mode`、`failurePolicy`、`generateThumbnail`、`thumbnailSize`、`scalingQuality`、`pngCompression`、`maxSourceCoverBytes`、`maxSidecarEntries`（默认值与影响见 7.1，完整声明以 `include/TagReader.hpp` 为准）。其中两条是结构性的：`mode == Disabled` 时**全部**封面处理被跳过（含目录解析与内嵌导出，不只是 sidecar 回退），元数据与歌词不受影响；`failurePolicy == Ignore` 只抑制 `CoverProcessingError`（见 4.7）。
- `CoverErrorCode`：9 个错误码，包括 `ExportDirectoryUnavailable`、`SidecarDiscoveryFailed`、`SidecarEntryLimitExceeded` 等。
- `CoverProcessingError`：携带 `CoverErrorCode` 的异常类型，受 `failurePolicy` 调控（`Ignore` 抑制、`Propagate` 抛出）。
- `MusicTag`（include/Tag.hpp）：title、genre、artist、album、albumArtist、composer、year(uint16)、trackNumber(uint16)、discNumber(uint16)、`Lyrics`、filePath、coverPath、thumbnailPath、duration(int64 微秒)、offset(int64 微秒)、lastModified、sampleRate、bitDepth、bitRate、channels、format、playCount、rating、lastPlayed。**没有 comment 字段**（`RawMetadata::comment` 在组装阶段不被映射）。
- `Lyrics`（include/Lyrics.hpp）：`std::vector<Lyric>`；`Lyric{timestamp(微秒), text}`。纯文本歌词在组装时按行拆成 timestamp=0 的 Lyric（见 6.1）。

### 4.2 转发层（src/TagReader.cpp）

只做转发：`Read()` → `tagreader_core::ReadTag()`；`ReadCueSheet()` → `tagreader_cue::ReadCueSheet()`；`ExportFolderCover()` 只组装 `ReadContext`、调用 `tagreader_cover::ExportSidecarCoverFromDirectory` 并把结果包装成 `MusicTag`（吞掉 `CoverProcessingError`）。不包含解析逻辑；CUE 不会进入 `Read()` 管线。

### 4.3 核心管线（src/core/）

- `ReadContext`：parser 共享上下文（`input`、`fileSize`、封面源预算扣账字段 `coverSourceBytesDebited` 等）。
- `RawTagData.hpp`：`RawMetadata`、`RawLyrics`、`DecodedField` 等中间态。
- `TagFormat.hpp`：格式枚举；已核实的值：`Id3v1`、`Id3v2`、`RawId3v2`、`Flac`、`VorbisComment`、`RawVorbisComment`、`OggVorbis`、`OggOpus`、`Mp4`、`RawMp4Ilst`、`Ape`、`RawApeV2`、`RiffWav`、`Aiff`、`Dsf`、`Dff`、`Asf`、`Matroska`、`Unknown`。
- `TagPipeline.cpp`：`ReadTag()` 与 `ReadCueSheet` 复用的核心流程（见 6.1）；`BuildMusicTag` 完成 Raw* → MusicTag 映射；`ClassifyCoverFailure` 实现封面错误分类。
- `CoverErrorPolicy.hpp`：封面错误策略（Ignore / Propagate）类型与分类。
- `CoverBudget.hpp`：封面源预算（per-read 共享，默认 64 MiB）。
- 顶层失败条件清单见 6.3。

### 4.4 媒体层（src/media/）

- `FfmpegSession.cpp`：`OpenContext`——打开 FFmpeg 上下文；拒绝 symlink、非普通文件、负大小；`avformat_open_input`/`avformat_find_stream_info` 失败即抛 `runtime_error`。
- `MediaInfoReader.cpp`：`DetectStream`——确定音频流，无音频流抛 "no audio stream found in input file"；`ReadMediaInfo`——时长/码率/采样率等，音频流信息不完整即抛。
- `ContainerDetector.cpp`：`DetectTagFormat`——**不抛异常**，无法识别时返回 `TagFormat::Unknown` 兜底；`ContainerFromTagFormat`——`TagFormat` → `DetectedContainer` 映射（不再有独立的 `DetectContainer` 步骤）。

### 4.5 IO 层（src/io/ByteReader.hpp/.cpp）

- `FileInput`：基于 pread 的绝对 offset 读取，不维护流位置。
- `ReadRange`：有界读取，默认上限 64 MiB（`kMaxGenericReadBytes`）；`TryAddSize` 防加法溢出。（`bounded::ReadRangeAt` 在 `src/formats/common/BoundedReader.cpp`，默认上限同为 64 MiB。）
- 大小端原语：`ReadBE16`、`ReadBE32` 及 LE 系列。

### 4.6 文本层（src/text/）

- `TextCodec`：UTF-8/UTF-16BE/UTF-16LE/Latin-1 等转换（`DecodeRawText`、`DecodeTextToUtf8`、`ReadUtf8Text`、`ReadUtf16Text`）。Iconv 默认必需；仅显式 `TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON` 才允许无 iconv 回退。
- **Iconv 为何默认必需**：无 iconv 时 `DetectLegacyLocalEncoding` 直接退化为 Latin-1，GB18030/GBK/SHIFT_JIS/BIG5/CP932/WINDOWS-1252/1251/1250 的探测被整体跳过，CJK 与西里尔/中欧文本会出现 mojibake。因此该降级只能由调用方显式开启，不能变成默认行为。
- **遗留编码探测必须无损往返（红线）**：候选编码是否可用由 `DecodesLosslesslyAs`（`src/text/TextCodec.cpp`）判定——把解码结果再编回候选编码，必须与原字节逐字节相等；有损候选一律拒绝。**不得放宽为"解码未报错即接受"**：各平台 libiconv 对非法字节的行为差异极大（例如 macOS 15 的 SHIFT_JIS/CP932 会静默丢弃非法字节并返回成功，rc=0），仅凭错误码无法识别"被吞掉的字节"，往返校验才是可移植的判据。
- `NormalizeMetadata`（TextNormalize.cpp:164-198）：对 7 个文本字段（title/genre/artist/album/albumArtist/composer/comment）统一处理：trim → 超过 65536 字节按 UTF-8 边界截断 → `IsValidUtf8` 校验（无效则清空）。数值字段（year/trackNumber/discNumber 等）直接透传；playCount/rating 在 ReadMetadata 中固定为 0。
- `NormalizeLyrics`（TextNormalize.cpp:200-236）：text 与 timedLines 各行 trim + UTF-8 校验（无效清空）；删除空行；超过 20000 行截断；**按时间戳稳定排序**（`std::stable_sort`，只比较时间戳，同时间戳的多行保持出现顺序——这是「同时间戳组内第 1 行 = 原文」配对约定可确定的前提）；去重谓词为（时间戳, 文本）相同；排序键只有时间戳，故 `std::unique` 只移除**相邻**的完全重复行。`text` 的行拆分发生在 BuildMusicTag（按 `'\n'` 切行、trim 后空行跳过、时间戳统一为 0）。
- `ReadLyricsFromPlainText`（TextNormalize.cpp:238-346）：parser 侧的 LRC/纯文本歌词入口（非 NormalizeLyrics 一部分）：超过 1 MiB 直接返回；LRC 元数据行（`[ar]`/`[ti]` 等）跳过；每行最多 32 个时间戳；有时间戳进 timedLines、否则累积纯文本；timedLines 优先。

### 4.7 封面层（src/cover/）

- 默认导出目录：`XDG_RUNTIME_DIR/tagreader-covers`；POSIX 回退 `temp_directory_path()/tagreader-covers-$UID`（目录的安全处理见下条红线）。
- `CoverCache`：SHA-256 内容寻址 PNG 缓存（分片子目录、原子发布）；命中直接复用，不重复解码或改写；`ExportCoverFromContext` 返回 `CoverPaths{fullSizePath, thumbnailPath}`，并对封面源字节做 per-read 预算扣账。
- `CoverDecoder`：封面解码/像素转换（FFmpeg）与 PNG 编码（fpng）；限制见 `CoverDecodeLimits`（`include/TagReaderInternal.hpp`）：封面编码输入与 PNG 输出各 64 MiB、单边 8192、总像素 32 Mi（32*1024*1024）。
- `SidecarCover`：sidecar 封面查找核心 `ExportSidecarCoverFromDirectory(directory, context)`（目录参数化）；`ExportSidecarCover` 以音频文件同目录调用之（`Read` 侧回退，见 6.2），`ExportFolderCover` 以 `folderPath` 自身目录调用之（见 4.1）。
- **封面导出的安全与副作用（红线）**：默认导出目录与显式 `coverExportDir` 都会被创建、探测读写并拒绝 symlink，默认目录还会额外硬化为当前用户私有（`src/core/TagPipeline.cpp` 的 `HardenDefaultCoverExportDir`：POSIX 分支用 `O_NOFOLLOW` + `fstat` 属主校验 + `fchmod(S_IRWXU)` 并复检权限位；非 POSIX 分支用 `std::filesystem::permissions(owner_all, replace)`）。缓存是内容寻址 PNG（SHA-256 分片），命中时直接复用、不重复解码或改写。
- **`Ignore` 的边界（红线）**：`failurePolicy == Ignore` **只抑制 `CoverProcessingError`**（清空 artwork，元数据与歌词继续）；**其它异常不得被该策略吞掉**（分类由 `ClassifyCoverFailure` 完成，非封面异常返回 `NotACoverError`）。

### 4.8 格式解析层（src/formats/）

共同约定：命名空间 `tagreader_<fmt>`；入口签名 `void Read<Fmt>Metadata(ReadContext&, RawMetadata&)` 与可选的 `void Read<Fmt>Lyrics(ReadContext&, RawLyrics&)`；void 返回、失败静默停止；文本字段 first-wins；封面经 `ExportCoverFromContext` 副作用导出。

| 目录 | 覆盖格式 | 关键实现（源码可证） |
|---|---|---|
| `id3/` | ID3v1、ID3v2.2/2.3/2.4（含 Raw* 复用） | 帧解析、同步安全整数；歌词入口 `ReadID3Lyrics` |
| `ape/` | APEv2 | footer 检测优先于 ID3；MP3+APE 以 APE 为主、ID3 补缺；歌词入口 `ReadApeLyrics` |
| `vorbis/` | Vorbis Comment | 键名映射；`VorbisCommentLimits.hpp` 集中常量 |
| `flac/` | FLAC | Vorbis Comment + PICTURE 块；歌词入口 `ReadFlacLyrics` |
| `ogg-vorbis/` | Ogg Vorbis | Ogg 页遍历；歌词入口 `ReadOggVorbisLyrics` |
| `opus/` | OpusTags | 歌词入口 `ReadOggOpusLyrics` |
| `mp4/` | MP4/M4A | atom 原语（`Mp4AtomReader`）+ 语义映射（`Mp4Parser`）；`moov→udta→meta→ilst` 路径状态机 DFS；11 个 ilst key（©nam/©ART/aART/©alb/©wrt/©gen/©day/date/trkn/disk/covr）；data atom 前 8 字节 type+locale，类型 0/1=UTF-8、2=UTF-16BE、3=UTF-16LE（trkn/disk 额外接受 21）；©lyr 与 `----` freeform（com.apple.iTunes/lyrics）歌词；限制：atom 数 100000、payload 64 MiB、文本字段 1 MiB、歌词 8 MiB、封面 64 MiB。观察到的行为：`FindNextMp4SiblingAfterSizeZero` 恒返回 nullopt，遇到 size-0 atom 时该层扫描终止（非规范背书，意图见 10） |
| `matroska/` | Matroska/WebM/MKA | EBML VINT（marker 位扫描、unknownSize 支持）；Tags/SimpleTag 递归（大小写不敏感名映射）；Attachments `image/*` 附件封面；限制：元素 100000、深度 16、单元素 64 MiB、文本 1 MiB、根扫描 64 MiB；**无歌词解析入口** |
| `asf/` | ASF/WMA | GUID 对象遍历，仅处理 Content/Extended Content/Metadata/Metadata Library 四类对象；WM/Picture 封面（byte[0] 类型 + U32LE 大小 + MIME/描述 UTF-16 NUL 终止）；WM/Lyrics 等文本描述符歌词；限制：对象 100000、描述符 4096、文本 1 MiB、图片 64 MiB。观察到的行为：`ParseAsfHeader` 的"字段已齐"条件块对循环无实际效果（非规范背书，意图见 10） |
| `riff/` | RIFF/WAV | LIST/INFO 块 |
| `aiff/` | AIFF/AIFC | FORM/COMM 块 |
| `dsd/` | DSF/DFF | 块解析（内嵌 ID3 复用） |
| `cue/` | CUE sheet | 独立管线（见 4.9） |

### 4.9 CUE 管线（src/formats/cue/）

- `tagreader_cue::ReadCueSheet`：独立于 `Read()`；`CueTextLoader`（文本上限 `kMaxCueTextBytes` = 4 MiB）→ `CueParser`（曲目/索引解析）→ `CuePathResolver`（引用文件解析，拒绝绝对路径、目录逃逸、symlink 与 CUE 自引用）→ 每个被引用的音频文件调用一次 `tagreader_core::ReadTag` 取回该文件标签 → 按轨套用 CUE 全局/文件/曲目元数据 → `CueTiming`（帧 → 微秒）应用到该文件的各轨。
- **无歌词解析入口**（该目录无任何 lyric 符号）；CUE 不读取歌词。
- **引用解析安全边界（红线）**：CUE 的 `FILE` 引用必须被解析到 CUE 所在目录之内；绝对路径/带根路径、任何一级目录逃逸（`.`/`..`）、每一级 symlink、以及 CUE 自引用（`equivalent()` 命中 CUE 自身）全部拒绝，最终目标还必须是普通文件且非目录。这是 CUE 路径穿越的唯一防线，**不得为兼容个别 CUE 文件而放宽**。

### 4.10 测试（test/）

- Catch2 体系：Catch2 main 由 `Catch2::Catch2WithMain` 提供；`test/main.cpp` 是 `TagReaderTest` 人工 CLI 的入口（顶层 `CMakeLists.txt` 的 `TagReaderTest` 目标），不是 Catch2 main。活跃用例在 `*_catch2_tests.cpp`（`test/catch2/` 与 `test/regression/`）；`test/regression/regression_tests.cpp` 不是独立 target，但被 `tr_audit_001_031_catch2_tests.cpp` 与 `tr_audit_032_056_catch2_tests.cpp` 以 `#include` 方式文本包含编译（提供 `RunTrAudit*` 实现），并非"未被编译"。
- 测试结构分两类，**不要把两类混成一个"目标数"**：
  - **13 个 Catch2 可执行目标**（smoke、tr-audit 001-031 / 032-056、cue 系列 Cue/CueMapping/CuePath/CueTiming、封面契约 CoverProcessingContract、SidecarCover、TagReaderFolderCover、DefaultCoverExportDirectory、FlacMalformedMetadata、LyricsNormalizeComplexity）。每个目标经 `catch_discover_tests` 展开为**逐 `TEST_CASE` 的 CTest 条目**，因此条目数远大于目标数，且随用例增删自动变化。
  - **手工 `add_test` 条目**：默认配置下 2 条（`TagReaderSecurityGenerateSamples`、`TagReaderSecuritySmoke`）；仅在 `fuzz` preset 且 `TagReaderFuzz` 目标确实生成（Clang + libFuzzer）时再增加 2 条（`TagReaderFuzzGenerateCorpus`、`TagReaderFuzzBoundedSmoke`）。（`TagReaderFuzz` 自身是 fuzz 可执行目标，不是 CTest 条目。）
  - 二者共同构成一次 `ctest` 运行看到的条目集合；写死一个总数没有意义，判断"测试是否齐全"应看目标与用例清单，而不是条目数。
- 安全测试：样本由 `test/security/generate_samples.py` 生成；缺少 ffmpeg CLI 或 codec 导致无样本时 Smoke 返回 77，CTest 记为 skip。
- Fuzz：仅 Clang/libFuzzer 下生成 `TagReaderFuzz`；语料由 `test/corpus/generate_corpus.py` 先生成。
- `TagReaderTest`：人工 CLI，`./build/default/TagReaderTest <audio-file-path> [cover-export-dir]`，不能替代 CTest。

## 5. 模块关系

### 5.1 依赖方向

```
include/（公共头）
  └─ src/TagReader.cpp（转发）
       └─ src/core/（TagPipeline）
            ├─ src/media/（OpenContext/DetectStream/DetectTagFormat/ReadMediaInfo）
            ├─ src/io/（FileInput、ReadRange、字节序原语）
            ├─ src/text/（TextCodec、TextNormalize）
            ├─ src/cover/（CoverCache、SidecarCover、CoverDecoder）
            └─ src/formats/<fmt>/（各 parser）
                 ├─ src/core/（ReadContext、RawTagData）
                 ├─ src/io/（ByteReader）
                 ├─ src/text/（TextCodec、TextNormalize）
                 ├─ src/cover/（ExportCoverFromContext）
                 ├─ src/common/（ParseHelpers）
                 └─ src/formats/common/（BoundedReader；Matroska/ASF 经其读取，不直接触碰 input）
```

已核实的具体依赖：MP4 直接使用 `context.input`（FileInput）与 `ReadBE16/32`；Matroska/ASF 不直接触碰 `context.input`，全部经 `bounded::ReadRangeAt(context, ...)` 读取；MP4/Matroska/ASF 三个盒式容器模块共用 `ExportCoverFromContext`；`profiling/Profiling.hpp` 的 `TAGREADER_PROFILE_FUNCTION` 被 core（TagPipeline）、cover（CoverCache/CoverDecoder）与多个格式 parser（ID3/FLAC/APE/Ogg Vorbis/Opus/MP4/CUE 等）使用，并非仅 MP4。

### 5.2 数据流

```
FileInput（src/io） ──> ReadContext（src/core）
                          │ 各 parser（src/formats/<fmt>/）
                          ▼
                    RawMediaInfo / RawMetadata / RawLyrics
                          │ NormalizeMetadata / NormalizeLyrics（src/text/）
                          ▼
                    BuildMusicTag（src/core/TagPipeline.cpp）──> MusicTag
封面：各 parser ──ExportCoverFromContext──> CoverCache（SHA-256 PNG，全尺寸+缩略图）
      └─ 内嵌封面缺失 ──> SidecarCover（音频文件同目录 sidecar 图片）
ExportFolderCover ──ExportSidecarCoverFromDirectory(folderPath)──> 同一缓存管线
CUE：ReadCueSheet（src/formats/cue/）──每个被引用音频文件──> tagreader_core::ReadTag
```

## 6. 核心运行流程

### 6.1 `Read(path, ...)` 主流程（ReadTag，TagPipeline.cpp）

固定顺序（不要另加步骤；尤其**不得引入独立的 `DetectContainer()` 步骤**，容器判定由 `DetectTagFormat` + `ContainerFromTagFormat` 两步完成，见 4.4）：

1. `ValidatePath`：路径非空、存在且为普通文件。
2. `OpenContext`（media）：打开 FFmpeg 上下文（含 symlink/类型/大小校验）。
3. 封面导出目录解析/校验/硬化（仅当 `mode != Disabled`：解析默认目录或显式目录，创建、拒绝 symlink、私有化并探测读写）。
4. `DetectStream`（media）：确定音频流；无音频流即失败。
5. `DetectTagFormat`（media）：识别标签格式；不抛异常，Unknown 兜底。
6. `ContainerFromTagFormat`：TagFormat → DetectedContainer 映射写入 context。
7. `ReadMediaInfo`（media）：时长、码率、采样率、位深、声道等。
8. `ReadMetadata`：按 TagFormat 分发到格式 parser（每次调用包在 `ignoreMalformedMetadata` 内：filesystem_error/runtime_error 只记诊断；`CoverProcessingError` 按 `ClassifyCoverFailure` 分类——Ignored 清空 artwork 继续、Propagated 重抛）。
9. sidecar 回退：内嵌封面缺失（coverPath 与 thumbnailPath 均为空）且 mode != Disabled 时查找 sidecar（见 6.2）。
10. `ReadLyrics`：按 TagFormat 分发歌词入口（见 4.8 表格）；无入口的格式（RiffWav/Aiff/Dsf/Dff/Matroska/Unknown 等）歌词保持空；parser 异常被吞（记诊断 + 清空歌词），不影响顶层。
11. `BuildMusicTag`：字段映射（title→setTitle、genre→setGenre、artist→setArtist、album→setAlbum、albumArtist→setAlbumArtist、composer→setComposer、year→setYear、trackNumber→setTrackNumber、discNumber→setDiscNumber、lyrics→setLyrics、filePath/coverPath/thumbnailPath、duration/offset/lastModified/sampleRate/bitDepth/bitRate/channels/format、playCount/rating）；**comment 无映射**；歌词文本按 `'\n'` 切行、trim 后空行跳过、时间戳统一 0 写入 `Lyrics`。

### 6.2 封面导出与 sidecar 回退

- 内嵌封面：各 parser 提取封面块 → `ExportCoverFromContext`（`mode == Disabled` 时直接返回空，不做任何封面处理）→ 内容寻址缓存（命中直接复用）→ 按 `mode` 写出全尺寸/缩略图 PNG → `MusicTag::coverPath`/`thumbnailPath`。
- 默认导出目录：`XDG_RUNTIME_DIR/tagreader-covers`（POSIX 回退 `temp_directory_path()/tagreader-covers-$UID`）；默认目录与显式目录的创建/探测读写/拒绝 symlink/私有化规则见 4.7 红线。
- sidecar 回退（SidecarCover.cpp）：仅当内嵌封面缺失且 mode != Disabled 触发。
  - 查找目录由调用方决定：`Read` 侧为音频文件同目录（`context.filePath.parent_path()`）；`ExportFolderCover` 侧为 `folderPath` 自身目录（同一查找核心 `ExportSidecarCoverFromDirectory` 复用）。
  - 名称清单（忽略大小写，按优先级）：`cover` / `front` / `folder` / `album` / `artwork`；扩展名清单（忽略大小写）：`.png` `.jpg` `.jpeg` `.bmp` `.webp` `.gif` `.tiff`。
  - 遍历跳过非普通文件与 symlink；同优先级内按文件名字典序；按优先级逐个尝试，首个成功（产生 fullSizePath 或 thumbnailPath）即返回。
  - 候选计数超过 `maxSidecarEntries`（默认 4096）抛 `SidecarEntryLimitExceeded`；目录遍历失败抛 `SidecarDiscoveryFailed`。
  - 单候选大小 0 或超过 `maxSourceCoverBytes`（默认 64 MiB）跳过；读取失败跳过。
  - 封面源字节对 per-read 共享预算（`coverSourceBytesDebited`）扣账，内嵌与 sidecar 共用。

### 6.3 顶层失败条件（ReadTag 会抛出的完整清单）

1. `ValidatePath`：空路径 → `invalid_argument`；查询失败/不存在/非普通文件 → `runtime_error`。
2. `OpenContext`：symlink、非普通文件、负大小、既非 POSIX 也非 Windows 的平台、`avformat_open_input` 失败、`avformat_find_stream_info` 失败（均为 `runtime_error`；bad_alloc 直接传播）。
3. 封面目录块（仅当 `mode != Disabled`）：`CoverProcessingError` 无条件重抛；其它异常包装为 `ExportDirectoryUnavailable` 后按策略（Propagate 抛、Ignore 吞并清空 coverExportDir）。
4. `DetectStream`：无音频流（"no audio stream found in input file"）、音频流 codecpar 缺失。
5. `ReadMediaInfo`：formatContext 空、audioStreamIndex < 0、音频流信息不完整。
6. `ReadMetadata`：parser 抛 `CoverProcessingError` 且策略为 Propagate；或 lambda 未捕获的其它异常。
7. sidecar：`CoverProcessingError` 且策略为 Propagate。

此外，`ReadMetadata`/`DetectStream`/`ReadMediaInfo` 入口处的不变量守卫（`format context is not initialized`、`audio stream index is not initialized`）同为 `runtime_error`；正常流程下前序步骤已保证其不可达。

**失败粒度规则（红线）**：局部 malformed 元数据或歌词字段只跳过或清空局部结果，不使顶层失败；只有输入不可用、无音频流、上下文/容器无法建立才走顶层失败。

不使顶层失败的情况：`DetectTagFormat` 永远不抛（Unknown 兜底）；元数据局部解析失败（静默或仅诊断）；歌词解析失败（吞掉清空）；无封面（"no-art" 不是错误，也绝不抛 `CoverProcessingError`）。

**两个方向都不得违反**：既不得把局部解析失败升级为顶层异常（会让一个坏字段毁掉整次读取），也不得把"输入不可用/无音频流/容器无法建立"这类真实失败静默吞掉（会让调用方把损坏文件当成空标签文件）。

### 6.4 `ReadCueSheet(path, ...)` 流程

文本加载（4 MiB 上限）→ 解析曲目/索引 → 逐文件引用解析（拒绝绝对路径/目录逃逸/symlink/自引用）→ 每个被引用的音频文件调用一次 `ReadTag` 取回该文件标签 → 按轨套用 CUE 全局/文件/曲目元数据 → 帧→微秒计时应用到该文件的各轨。歌词取自被引用音频文件自身标签，CUE sheet 本身无歌词解析；文本加载/解析失败，或单音频文件下引用解析、读取失败时返回空 `MusicTag` 列表；`CoverProcessingError` 原样抛出。

## 7. 配置方式

### 7.1 运行时（`CoverProcessingOptions`）

| 字段 | 默认值 | 影响 |
|---|---|---|
| `mode` | `FullAndThumbnail` | 封面导出模式（`FullAndThumbnail`/`ThumbnailOnly`/`FullOnly`/`Disabled`）；`Disabled` 跳过全部封面处理（目录解析、内嵌导出与 sidecar 回退），元数据与歌词不受影响 |
| `failurePolicy` | `Propagate` | 封面错误策略；`Ignore` 只清空 artwork 并继续元数据与歌词 |
| `generateThumbnail` | `true` | `FullAndThumbnail` 下是否额外产出缩略图 |
| `thumbnailSize` | 256×256，保持宽高比 | 缩略图尺寸上限（原图更小时不放大） |
| `scalingQuality` | `Fast` | 缩略图缩放质量（`Fast`/`Good`/`Best`） |
| `pngCompression` | `Fast`（level 1） | 缩略图 PNG 压缩级别（`Fast`/`Balanced`/`Best`） |
| `maxSourceCoverBytes` | 64 MiB | 封面源上限，内嵌与 sidecar 共用 |
| `maxSidecarEntries` | 4096 | sidecar 候选计数上限（超限抛 `SidecarEntryLimitExceeded`） |

### 7.2 构建期（CMake）

- Presets：`default`（常规构建+测试）、`release`、`sanitize`、`fuzz`、`profile`。
- `TAGREADER_USE_SYSTEM_CATCH2`（默认 ON）：优先系统 Catch2 包，缺失时 FetchContent 下载（离线环境首次配置需要网络）。
- `TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV`（默认 OFF）：关闭 iconv 依赖的回退开关（另有测试专用的 `TAGREADER_FORCE_DISABLE_ICONV_FOR_TESTS`）。
- `TAGREADER_ENABLE_SANITIZERS`（默认 OFF，仅 Clang/GNU 生效）：为所有目标启用 ASan+UBSan（`-fno-sanitize-recover=all`），`sanitize` preset 使用。
- `TAGREADER_ENABLE_PROFILING`（默认 OFF）：Tracy 性能分析。**Release 下被强制关闭**——`CMakeLists.txt` 在 `CMAKE_BUILD_TYPE STREQUAL "Release"` 时以 `FORCE` 把该选项置 OFF，只有 `profile` preset（`RelWithDebInfo`）保留它。
- `profile` preset 的 Tracy 依赖是**系统包供给路径，不是 pkg-config 入口**：`find_library(TRACY_LIBRARY NAMES TracyClient REQUIRED)` + 硬编码 `TRACY_INCLUDE_DIR "/usr/include/Tracy"`，并在链接时追加 `pthread` 与 `dl`（均为 Linux 语义）。因此该 preset 目前是 Linux/系统包供给的路径（CMake 自身的失败提示写作 `yay -S tracy`）；TracyClient 缺失时在**配置期**即失败——`find_library(... REQUIRED)` 直接终止 configure，`else()` 分支里的 `FATAL_ERROR` 提示在支持 `REQUIRED` 的 CMake 上不会被走到。
- `release` 使用 `-march=x86-64` 基线（LTO 由 preset 的 `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON` 提供），产物可跨机器分发；仅 `profile` 追加 `-march=native`（`-O3 -DNDEBUG -g`），产物仅限本机使用。
- `fuzz` preset（`TAGREADER_ENABLE_FUZZING`，默认 OFF）仅在 Clang/libFuzzer 下生成 `TagReaderFuzz`；相关 CTest 先生成语料。
- `TagReaderCore` 支持安装导出：`cmake --install`（`install(EXPORT TagReaderCoreTargets)` + `cmake/TagReaderCoreConfig.cmake.in`）装到共享前缀后，消费方（Seriona_Backend 等）可用 `find_package(TagReaderCore CONFIG)` 复用构建产物；消费方需能重建 FFmpeg/Iconv 依赖。
- 验证命令：`cmake --preset default` → `cmake --build --preset default` → `ctest --preset default --output-on-failure`。

## 8. 扩展方式

### 8.1 新增格式解析器（基于既有模式）

1. 新建 `src/formats/<fmt>/`，提供 `Read<Fmt>Metadata(ReadContext&, RawMetadata&)`；若支持歌词，再提供 `Read<Fmt>Lyrics(ReadContext&, RawLyrics&)`（Matroska 等无歌词格式可省略）。
2. 在 `src/core/TagFormat.hpp` 增加枚举值，在 media 层 `ContainerDetector` 的识别与 `ContainerFromTagFormat` 映射中登记。
3. 在 `src/core/TagPipeline.cpp` 的 `ReadMetadata` 与 `ReadLyrics` 分发 switch 中注册入口。
4. 资源上限沿用既有约定：文件顶部 `kMax*` 常量；二进制访问只用 `ReadRange`/`ReadRangeAt`（绝对 offset）；封面经 `ExportCoverFromContext`；文本经 `TextCodec` 解码、`TrimText` 等规范化；歌词文本用 `ReadLyricsFromPlainText`（LRC 支持）。新增 parser 同样受第 2 节各条红线与第 9 节约束的约束（尤其是"不得用 `AVDictionary`""不得放宽上限""不得污染流位置"）。
5. 在 `test/regression/` 下按 `*_catch2_tests.cpp` 模式补充测试，新增测试目标必须经 `tagreader_enable_sanitizers()` 接入（不得绕过）；安全样本如需在 `test/security/generate_samples.py` 扩展。

### 8.2 新增 MusicTag 字段

`MusicTag`（Tag.hpp）加字段与访问器 → `RawMetadata` 加中间态 → parser 填充 → `BuildMusicTag` 映射 → `NormalizeMetadata`（若是文本字段）处理。

## 9. 开发约束与建议

下列各条中标注"（红线）"者为不可协商的约束，与第 2 节一致；其余为经验性建议。

- **有界读取（红线）**：所有二进制访问使用绝对 offset + `ReadRange`/`ReadRangeAt`/`BoundedReader`，**禁止依赖或污染流位置**——parser 先后复用同一个 fd，流位置不是可靠状态（各 parser 与 `context.input` 的约定）。
- **资源上限（红线）**：新解析逻辑必须带上限，**禁止无界分配**，且**既有上限不得放宽**（各 parser 顶部 `kMax*` 与 `CoverDecodeLimits`：`ReadRange` 默认 64 MiB、MP4 atom payload 64 MiB / 最多 100000 atoms、封面编码输入与 PNG 输出各 64 MiB / 单边 8192 / 总像素 `32 * 1024 * 1024`、文本字段 1 MiB、描述符与条目 4096 等）。输入是第三方音频文件，属完全不可信数据。
- **错误语义（红线）**：局部 malformed 元数据/歌词字段只跳过或清空局部结果，不使顶层失败；只有输入不可用、无音频流、上下文/容器无法建立才走顶层失败（见 6.3）。封面错误遵循 `failurePolicy`（`Ignore` 只清空 artwork 并继续，且只对 `CoverProcessingError` 生效）。
- **编码收口（红线）**：解析器只产出原始字节/中间态；文本终态必须经 `NormalizeMetadata`/`NormalizeLyrics` 收口为 UTF-8；无效 UTF-8 清空而非猜测；遗留编码探测必须满足 `DecodesLosslesslyAs` 的无损往返（见 4.6），不得放宽为"解码未报错即接受"。
- **first-wins**：多来源同字段（如 MP3+APE、内嵌+sidecar）遵循既有优先级：APE 优先于 ID3、首个非空字段生效。
- **封面副作用集中（红线）**：不要在 parser 内自行写文件；一律经 `ExportCoverFromContext` 走缓存管线。
- **平台改动（红线）**：新增平台分支必须保证其余平台仍可配置构建，且不得把平台条件带进公共头（见 2.1）。

## 10. 维护建议

- 事实基准：以 `CMakeLists.txt`、`CMakePresets.json`、公共头文件、`src/`、`test/` 为准；`README.md` 与本文档冲突时不采信 README。
- 常量集中：修改资源上限时检查 `include/TagReaderInternal.hpp`（`CoverDecodeLimits`）与各 parser 顶部 `kMax*`，保持与 AGENTS.md 记录一致。
- 测试入口：`ctest --preset default -R <regex> --output-on-failure`；Catch2 discovered 测试名即精确 `TEST_CASE` 文本（可用子串如 `TR-AUDIT-001`、`CoverContract:`、`cue file resolver`）；唯一例外是 `TagReaderFolderCoverCatch2Tests` 目标带 `TEST_PREFIX "TagReaderFolderCover."`；安全 Smoke 在缺 ffmpeg/codec 时返回 77（skip），不是失败。
- 遗留代码：`test/regression/regression_tests.cpp` 本身不是独立 target，但被 `tr_audit_*_catch2_tests.cpp` 以 `#include` 方式编译（`RunTrAudit*` 由 TR-AUDIT-001~056 用例调用）；勿把它当作独立可执行入口。
- 已知实现事实：以下均为**从源码观察到的行为**，不是推荐做法，也不是规范背书；其**设计意图无法从源码确认**。修改前请先确认原始意图，不要仅凭"看起来不对"就改成其它行为：
  - MP4 `FindNextMp4SiblingAfterSizeZero`（`src/formats/mp4/Mp4AtomReader.cpp`）恒返回 `nullopt`：遇到 size-0 atom 时该层扫描即终止。是否有意如此，源码无依据。
  - ASF `ParseAsfHeader`（`src/formats/asf/AsfParser.cpp`）的"字段已齐"条件块不改变循环行为——其 `continue` 位于循环体末尾。是否本意为提前终止，源码无依据；疑似无效代码。
  - MP4 `gnre`（数字流派）不在支持 key 集内；`trkn`/`disk` 额外接受 dataType 21（`src/formats/mp4/Mp4Parser.cpp`）。dataType 21 的语义源码无依据，接受或拒绝哪个更正确同样无依据。
  - ASF Metadata Object 每描述符前的 4 字节前缀被直接跳过（`src/formats/asf/AsfParser.cpp`）；其与规范的一致性源码无依据。
  - `RawMetadata::comment` 在组装阶段无映射（`MusicTag` 无 comment 字段，见 4.1）。
