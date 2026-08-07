# TagReader 设计文档

> 本文档由源码与构建配置逆向重建，源码（`include/`、`src/`、`test/`、`CMakeLists.txt`、`CMakePresets.json`）为唯一可信来源；`README.md` 与历史文档不作为依据，冲突时以源码为准。
> 文中标注"无法确认"之处为源码中无法直接证明、需外部规范或设计意图确认的内容。

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
转发层           src/TagReader.cpp（只转发，不含逻辑）
核心管线         src/core/（ReadTag 固定流程、ReadContext、RawMetadata/RawLyrics）
支撑层           src/media/  src/io/  src/text/  src/cover/  src/common/
格式解析层       src/formats/<fmt>/（原始字节 parser，13 个格式目录 + common/）
CUE 管线         src/formats/cue/（独立于 Read()，每轨复用核心管线）
```

架构约定（全部经源码核实）：

- **解析器与媒体层分工**：标题/歌手/专辑/歌词/封面块由各格式 parser 用原始字节解析；FFmpeg 只做 probe、音频流选择、基础媒体信息与封面解码/像素转换。
- **共享输入与有界读取**：所有 parser 共用 `ReadContext`（`src/core/ReadContext.hpp`）中的 `input`（`tagreader_io::FileInput`，见 `src/io/ByteReader.hpp`）与 `fileSize`；二进制访问一律使用绝对 offset + 有界读取（`ReadRange`/`ReadRangeAt`），不依赖、不污染流位置。
- **中间态与终态分离**：解析结果先写入 `RawMediaInfo`/`RawMetadata`/`RawLyrics`（`src/core/RawTagData.hpp`），由 `NormalizeMetadata()`/`NormalizeLyrics()`（`src/text/TextNormalize.cpp`）收口为 UTF-8 后组装 `MusicTag`。
- **封面导出副作用**：所有格式 parser 通过 `ExportCoverFromContext`（`src/cover/CoverCache.hpp`）把封面数据交给统一缓存管线，返回 `CoverPaths{fullSizePath, thumbnailPath}`。
- **错误静默化 + 顶层策略**：格式 parser 全部返回 void、解析失败静默停止；元数据/歌词的局部失败不使顶层失败，由 core 按策略处理（见 6.3）。
- **资源上限集中管理**：常量集中在各 parser 文件顶部的 `kMax*` 与 `include/TagReaderInternal.hpp` 的 `CoverDecodeLimits`。

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
- `CoverProcessingOptions` 已核实字段：`mode`（默认 `FullAndThumbnail`；sidecar 回退仅在 mode != Disabled 时触发）、`failurePolicy`（默认 `Propagate`）、`maxSourceCoverBytes`（默认 64 MiB，内嵌封面与 sidecar 共用）、`maxSidecarEntries`（默认 4096）。完整字段清单以 `include/TagReader.hpp` 为准。
- `CoverErrorCode`：9 个错误码，包括 `ExportDirectoryUnavailable`、`SidecarDiscoveryFailed`、`SidecarEntryLimitExceeded` 等。
- `CoverProcessingError`：携带 `CoverErrorCode` 的异常类型，受 `failurePolicy` 调控（`Ignore` 抑制、`Propagate` 抛出）。
- `MusicTag`（include/Tag.hpp）：title、genre、artist、album、albumArtist、composer、year(uint16)、trackNumber(uint16)、discNumber(uint16)、`Lyrics`、filePath、coverPath、thumbnailPath、duration(int64 微秒)、offset(int64 微秒)、lastModified、sampleRate、bitDepth、bitRate、channels、format、playCount、rating、lastPlayed。**没有 comment 字段**（`RawMetadata::comment` 在组装阶段不被映射）。
- `Lyrics`（include/Lyrics.hpp）：`std::vector<Lyric>`；`Lyric{timestamp(微秒), text}`。纯文本歌词在组装时按行拆成 timestamp=0 的 Lyric（见 6.1）。

### 4.2 转发层（src/TagReader.cpp）

只做转发：`Read()` → `tagreader_core::ReadTag()`；`ReadCueSheet()` → `tagreader_cue::ReadCueSheet()`。不包含解析逻辑；CUE 不会进入 `Read()` 管线。

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
- `NormalizeMetadata`（TextNormalize.cpp:164-198）：对 7 个文本字段（title/genre/artist/album/albumArtist/composer/comment）统一处理：trim → 超过 65536 字节按 UTF-8 边界截断 → `IsValidUtf8` 校验（无效则清空）。数值字段（year/trackNumber/discNumber 等）直接透传；playCount/rating 在 ReadMetadata 中固定为 0。
- `NormalizeLyrics`（TextNormalize.cpp:200-236）：text 与 timedLines 各行 trim + UTF-8 校验（无效清空）；删除空行；超过 20000 行截断；按（时间戳, 文本）排序；完全重复行去重。`text` 的行拆分发生在 BuildMusicTag（按 `'\n'` 切行、trim 后空行跳过、时间戳统一为 0）。
- `ReadLyricsFromPlainText`（TextNormalize.cpp:238-346）：parser 侧的 LRC/纯文本歌词入口（非 NormalizeLyrics 一部分）：超过 1 MiB 直接返回；LRC 元数据行（`[ar]`/`[ti]` 等）跳过；每行最多 32 个时间戳；有时间戳进 timedLines、否则累积纯文本；timedLines 优先。

### 4.7 封面层（src/cover/）

- 默认导出目录：`XDG_RUNTIME_DIR/tagreader-covers`；POSIX 回退 `temp_directory_path()/tagreader-covers-$UID`。默认目录会创建、拒绝 symlink 并硬化为当前用户私有；显式 `coverExportDir` 同样会创建、探测读写并拒绝 symlink。
- `CoverCache`：SHA-256 内容寻址 PNG 缓存（分片子目录、原子发布）；命中直接复用，不重复解码或改写；`ExportCoverFromContext` 返回 `CoverPaths{fullSizePath, thumbnailPath}`，并对封面源字节做 per-read 预算扣账。
- `CoverDecoder`：封面解码/像素转换（FFmpeg）与 PNG 编码（fpng）；限制见 `CoverDecodeLimits`（`include/TagReaderInternal.hpp`）：封面编码输入与 PNG 输出各 64 MiB、单边 8192、总像素 32 Mi（32*1024*1024）。
- `SidecarCover`：sidecar 封面查找核心 `ExportSidecarCoverFromDirectory(directory, context)`（目录参数化）；`ExportSidecarCover` 以音频文件同目录调用之（`Read` 侧回退，见 6.2），`ExportFolderCover` 以 `folderPath` 自身目录调用之（见 4.1）。

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
| `mp4/` | MP4/M4A | atom 原语（`Mp4AtomReader`）+ 语义映射（`Mp4Parser`）；`moov→udta→meta→ilst` 路径状态机 DFS；11 个 ilst key（©nam/©ART/aART/©alb/©wrt/©gen/©day/date/trkn/disk/covr）；data atom 前 8 字节 type+locale，类型 0/1=UTF-8、2=UTF-16BE、3=UTF-16LE（trkn/disk 额外接受 21）；©lyr 与 `----` freeform（com.apple.iTunes/lyrics）歌词；限制：atom 数 100000、payload 64 MiB、文本字段 1 MiB、歌词 8 MiB、封面 64 MiB。实现事实：`FindNextMp4SiblingAfterSizeZero` 恒返回 nullopt，遇到 size-0 atom 时该层扫描终止（意图无法确认） |
| `matroska/` | Matroska/WebM/MKA | EBML VINT（marker 位扫描、unknownSize 支持）；Tags/SimpleTag 递归（大小写不敏感名映射）；Attachments `image/*` 附件封面；限制：元素 100000、深度 16、单元素 64 MiB、文本 1 MiB、根扫描 64 MiB；**无歌词解析入口** |
| `asf/` | ASF/WMA | GUID 对象遍历，仅处理 Content/Extended Content/Metadata/Metadata Library 四类对象；WM/Picture 封面（byte[0] 类型 + U32LE 大小 + MIME/描述 UTF-16 NUL 终止）；WM/Lyrics 等文本描述符歌词；限制：对象 100000、描述符 4096、文本 1 MiB、图片 64 MiB。实现事实：`ParseAsfHeader` 的"字段已齐"条件块对循环无实际效果（疑似无效代码，意图无法确认） |
| `riff/` | RIFF/WAV | LIST/INFO 块 |
| `aiff/` | AIFF/AIFC | FORM/COMM 块 |
| `dsd/` | DSF/DFF | 块解析（内嵌 ID3 复用） |
| `cue/` | CUE sheet | 独立管线（见 4.9） |

### 4.9 CUE 管线（src/formats/cue/）

- `tagreader_cue::ReadCueSheet`：独立于 `Read()`；`CueTextLoader`（文本上限 `kMaxCueTextBytes` = 4 MiB）→ `CueParser`（曲目/索引解析）→ `CuePathResolver`（引用文件解析，拒绝绝对路径、目录逃逸、symlink 与 CUE 自引用）→ `CueTiming`（帧 → 微秒）→ 每轨调用 `tagreader_core::ReadTag` 组装 `MusicTag`。
- **无歌词解析入口**（该目录无任何 lyric 符号）；CUE 不读取歌词。

### 4.10 测试（test/）

- Catch2 体系：Catch2 main 由 `Catch2::Catch2WithMain` 提供；`test/main.cpp` 是 `TagReaderTest` 人工 CLI 的入口（CMakeLists.txt:213），不是 Catch2 main。活跃用例在 `*_catch2_tests.cpp`（`test/catch2/` 与 `test/regression/`）；`test/regression/regression_tests.cpp` 不是独立 target，但被 `tr_audit_001_031_catch2_tests.cpp` 与 `tr_audit_032_056_catch2_tests.cpp` 以 `#include` 方式文本包含编译（提供 `RunTrAudit*` 实现），并非"未被编译"。
- CTest 目标（17 个注册）：smoke、tr-audit（001-056）、cue 系列（Cue/CueMapping/CuePath/CueTiming）、封面契约（CoverProcessingContract）、SidecarCover、TagReaderFolderCover、DefaultCoverExportDirectory、FlacMalformedMetadata、LyricsNormalizeComplexity、SecurityGenerateSamples、SecuritySmoke、FuzzGenerateCorpus、FuzzBoundedSmoke。（`TagReaderFuzz` 是 fuzz 可执行目标，不是 CTest。）
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

已核实的具体依赖：MP4 直接使用 `context.input`（FileInput）与 `ReadBE16/32`；Matroska/ASF 不直接触碰 `context.input`，全部经 `bounded::ReadRangeAt(context, ...)` 读取；三个盒式容器模块共用 `ExportCoverFromContext`；`profiling/Profiling.hpp` 的 `TAGREADER_PROFILE_FUNCTION` 被 core（TagPipeline）、cover（CoverCache/CoverDecoder）与多个格式 parser（ID3/FLAC/APE/Ogg Vorbis/Opus/MP4/CUE 等）使用，并非仅 MP4。

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
CUE：ReadCueSheet（src/formats/cue/）──每轨──> tagreader_core::ReadTag
```

## 6. 核心运行流程

### 6.1 `Read(path, ...)` 主流程（ReadTag，TagPipeline.cpp）

固定顺序（不要另加步骤）：

1. `ValidatePath`：路径非空、存在且为普通文件。
2. `OpenContext`（media）：打开 FFmpeg 上下文（含 symlink/类型/大小校验）。
3. 封面导出目录解析/校验/硬化（创建、拒绝 symlink、私有化）。
4. `DetectStream`（media）：确定音频流；无音频流即失败。
5. `DetectTagFormat`（media）：识别标签格式；不抛异常，Unknown 兜底。
6. `ContainerFromTagFormat`：TagFormat → DetectedContainer 映射写入 context。
7. `ReadMediaInfo`（media）：时长、码率、采样率、位深、声道等。
8. `ReadMetadata`：按 TagFormat 分发到格式 parser（每次调用包在 `ignoreMalformedMetadata` 内：filesystem_error/runtime_error 只记诊断；`CoverProcessingError` 按 `ClassifyCoverFailure` 分类——Ignored 清空 artwork 继续、Propagated 重抛）。
9. sidecar 回退：内嵌封面缺失（coverPath 与 thumbnailPath 均为空）且 mode != Disabled 时查找 sidecar（见 6.2）。
10. `ReadLyrics`：按 TagFormat 分发歌词入口（见 4.8 表格）；无入口的格式（RiffWav/Aiff/Dsf/Dff/Matroska/Unknown 等）歌词保持空；parser 异常被吞（记诊断 + 清空歌词），不影响顶层。
11. `BuildMusicTag`：字段映射（title→setTitle、genre→setGenre、artist→setArtist、album→setAlbum、albumArtist→setAlbumArtist、composer→setComposer、year→setYear、trackNumber→setTrackNumber、discNumber→setDiscNumber、lyrics→setLyrics、filePath/coverPath/thumbnailPath、duration/offset/lastModified/sampleRate/bitDepth/bitRate/channels/format、playCount/rating）；**comment 无映射**；歌词文本按 `'\n'` 切行、trim 后空行跳过、时间戳统一 0 写入 `Lyrics`。

### 6.2 封面导出与 sidecar 回退

- 内嵌封面：各 parser 提取封面块 → `ExportCoverFromContext` → 内容寻址缓存（命中直接复用）→ 写入全尺寸 + 缩略图 PNG → `MusicTag::coverPath`/`thumbnailPath`。
- 默认导出目录：`XDG_RUNTIME_DIR/tagreader-covers`（POSIX 回退 `temp_directory_path()/tagreader-covers-$UID`）；显式目录同样创建、探测读写、拒绝 symlink。
- sidecar 回退（SidecarCover.cpp）：仅当内嵌封面缺失且 mode != Disabled 触发。
  - 查找目录由调用方决定：`Read` 侧为音频文件同目录（`context.filePath.parent_path()`）；`ExportFolderCover` 侧为 `folderPath` 自身目录（同一查找核心 `ExportSidecarCoverFromDirectory` 复用）。
  - 名称清单（忽略大小写，按优先级）：`cover` / `front` / `folder` / `album` / `artwork`；扩展名清单（忽略大小写）：`.png` `.jpg` `.jpeg` `.bmp` `.webp` `.gif` `.tiff`。
  - 遍历跳过非普通文件与 symlink；同优先级内按文件名字典序；按优先级逐个尝试，首个成功（产生 fullSizePath 或 thumbnailPath）即返回。
  - 候选计数超过 `maxSidecarEntries`（默认 4096）抛 `SidecarEntryLimitExceeded`；目录遍历失败抛 `SidecarDiscoveryFailed`。
  - 单候选大小 0 或超过 `maxSourceCoverBytes`（默认 64 MiB）跳过；读取失败跳过。
  - 封面源字节对 per-read 共享预算（`coverSourceBytesDebited`）扣账，内嵌与 sidecar 共用。

### 6.3 顶层失败条件（ReadTag 会抛出的完整清单）

1. `ValidatePath`：空路径 → `invalid_argument`；查询失败/不存在/非普通文件 → `runtime_error`。
2. `OpenContext`：symlink、非普通文件、负大小、非 POSIX 平台、`avformat_open_input` 失败、`avformat_find_stream_info` 失败（均为 `runtime_error`；bad_alloc 直接传播）。
3. 封面目录块：`CoverProcessingError` 无条件重抛；其它异常包装为 `ExportDirectoryUnavailable` 后按策略（Propagate 抛、Ignore 吞并清空 coverExportDir）。
4. `DetectStream`：无音频流（"no audio stream found in input file"）、音频流 codecpar 缺失。
5. `ReadMediaInfo`：formatContext 空、audioStreamIndex < 0、音频流信息不完整。
6. `ReadMetadata`：parser 抛 `CoverProcessingError` 且策略为 Propagate；或 lambda 未捕获的其它异常。
7. sidecar：`CoverProcessingError` 且策略为 Propagate。

不使顶层失败的情况：`DetectTagFormat` 永远不抛（Unknown 兜底）；元数据局部解析失败（静默或仅诊断）；歌词解析失败（吞掉清空）；无封面（"no-art" 不是错误）。

### 6.4 `ReadCueSheet(path, ...)` 流程

文本加载（4 MiB 上限）→ 解析曲目/索引 → 引用文件解析（拒绝绝对路径/目录逃逸/symlink/自引用）→ 帧→微秒 → 每轨调用 `ReadTag` 组装 `MusicTag`（每轨复用核心管线；歌词取自被引用音频文件自身标签，CUE sheet 本身无歌词解析）。

## 7. 配置方式

### 7.1 运行时（`CoverProcessingOptions`）

| 字段 | 默认值 | 影响 |
|---|---|---|
| `mode` | `FullAndThumbnail` | 封面导出模式；Disabled 时跳过 sidecar 回退 |
| `failurePolicy` | `Propagate` | 封面错误策略；`Ignore` 只清空 artwork 并继续元数据与歌词 |
| `maxSourceCoverBytes` | 64 MiB | 封面源上限，内嵌与 sidecar 共用 |
| `maxSidecarEntries` | 4096 | sidecar 候选计数上限（超限抛 `SidecarEntryLimitExceeded`） |

### 7.2 构建期（CMake）

- Presets：`default`（常规构建+测试）、`release`、`sanitize`、`fuzz`、`profile`。
- `TAGREADER_USE_SYSTEM_CATCH2`（默认 ON）：优先系统 Catch2 包，缺失时 FetchContent 下载（离线环境首次配置需要网络）。
- `TAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV`（默认 OFF）：关闭 iconv 依赖的回退开关。
- `TAGREADER_ENABLE_PROFILING`（默认 OFF）：Tracy 性能分析；Profile preset 依赖系统 TracyClient 库与 `/usr/include/Tracy`（非 pkg-config 入口）。
- `release`/`profile` 编译参数含 `-march=native`，产物仅限本机使用。
- `fuzz` preset 仅在 Clang/libFuzzer 下生成 `TagReaderFuzz`；相关 CTest 先生成语料。
- 验证命令：`cmake --preset default` → `cmake --build --preset default` → `ctest --preset default --output-on-failure`。

## 8. 扩展方式

### 8.1 新增格式解析器（基于既有模式）

1. 新建 `src/formats/<fmt>/`，提供 `Read<Fmt>Metadata(ReadContext&, RawMetadata&)`；若支持歌词，再提供 `Read<Fmt>Lyrics(ReadContext&, RawLyrics&)`（Matroska 等无歌词格式可省略）。
2. 在 `src/core/TagFormat.hpp` 增加枚举值，在 media 层 `ContainerDetector` 的识别与 `ContainerFromTagFormat` 映射中登记。
3. 在 `src/core/TagPipeline.cpp` 的 `ReadMetadata` 与 `ReadLyrics` 分发 switch 中注册入口。
4. 资源上限沿用既有约定：文件顶部 `kMax*` 常量；二进制访问只用 `ReadRange`/`ReadRangeAt`（绝对 offset）；封面经 `ExportCoverFromContext`；文本经 `TextCodec` 解码、`TrimText` 等规范化；歌词文本用 `ReadLyricsFromPlainText`（LRC 支持）。
5. 在 `test/regression/` 下按 `*_catch2_tests.cpp` 模式补充测试；安全样本如需在 `test/security/generate_samples.py` 扩展。

### 8.2 新增 MusicTag 字段

`MusicTag`（Tag.hpp）加字段与访问器 → `RawMetadata` 加中间态 → parser 填充 → `BuildMusicTag` 映射 → `NormalizeMetadata`（若是文本字段）处理。

## 9. 开发建议

- **有界读取**：所有二进制访问使用绝对 offset + `ReadRange`/`ReadRangeAt`/`BoundedReader`，禁止依赖或污染流位置（各 parser 与 `context.input` 的约定）。
- **资源上限**：新解析逻辑必须带上限（参考各 parser 顶部 `kMax*` 与 `CoverDecodeLimits`：64 MiB 读取/封面、1 MiB 文本、100000 元素/atom/对象、4096 描述符/条目等），禁止无界分配。
- **错误语义**：局部 malformed 元数据/歌词字段应跳过或清空局部结果，不使顶层失败；只有输入不可用、无音频流、上下文/容器无法建立才走顶层失败（见 6.3）。封面错误遵循 `failurePolicy`（Ignore 清空 artwork 继续）。
- **编码收口**：解析器只产出原始字节/中间态；文本终态必须经 `NormalizeMetadata`/`NormalizeLyrics` 收口为 UTF-8；无效 UTF-8 清空而非猜测。
- **first-wins**：多来源同字段（如 MP3+APE、内嵌+sidecar）遵循既有优先级：APE 优先于 ID3、首个非空字段生效。
- **封面副作用集中**：不要在 parser 内自行写文件；一律经 `ExportCoverFromContext` 走缓存管线。

## 10. 维护建议

- 事实基准：以 `CMakeLists.txt`、`CMakePresets.json`、公共头文件、`src/`、`test/` 为准；`README.md` 与本文档冲突时不采信 README。
- 常量集中：修改资源上限时检查 `include/TagReaderInternal.hpp`（`CoverDecodeLimits`）与各 parser 顶部 `kMax*`，保持与 AGENTS.md 记录一致。
- 测试入口：`ctest --preset default -R <regex> --output-on-failure`；Catch2 discovered 测试名即精确 `TEST_CASE` 文本（可用子串如 `TR-AUDIT-001`、`CoverContract:`、`cue file resolver`）；安全 Smoke 在缺 ffmpeg/codec 时返回 77（skip），不是失败。
- 遗留代码：`test/regression/regression_tests.cpp` 本身不是独立 target，但被 `tr_audit_*_catch2_tests.cpp` 以 `#include` 方式编译（`RunTrAudit*` 由 TR-AUDIT-001~056 用例调用）；勿把它当作独立可执行入口。
- 已知实现事实（勿"修复"为规范行为前先确认意图）：
  - MP4 `FindNextMp4SiblingAfterSizeZero` 恒返回 nullopt（size-0 atom 处该层扫描终止）。
  - ASF `ParseAsfHeader` 的"字段已齐"条件块不改变循环行为（疑似无效代码）。
  - MP4 `gnre`（数字流派）不在支持 key 集内；`trkn`/`disk` 接受 dataType 21（语义无法确认）。
  - ASF Metadata Object 每描述符前的 4 字节前缀被直接跳过（规范符合性无法确认）。
  - `RawMetadata::comment` 在组装阶段无映射（MusicTag 无 comment 字段）。
