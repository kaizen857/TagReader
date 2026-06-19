# TagReader 设计说明

## 当前定位

- TagReader 是一个 C++23 轻量音乐元数据读取库，支持 ID3v1/v2、Vorbis Comment（FLAC/Ogg Vorbis）、MP4 atom（ilst）和 APEv2 标签格式。
- 对外 facade 保持单一读取入口：`TagReader::Read(path)` 和可选封面导出目录的 `TagReader::Read(path, coverExportDir)`。
- `Read()` 的主流程是 `ValidatePath()` -> `OpenContext()` -> `ValidateCoverExportDir()` -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- `ContainerFromTagFormat()` 直接将 `TagFormat` 映射为 `DetectedContainer` 并写入 `context.detectedContainer`，不再有独立的 `DetectContainer()` 步骤。
- `ReadMetadata()` 入口处和每个 catch 分支均调用 `context.input.clear()` 恢复流状态；`ReadLyrics()` 入口处也执行 `clear()`。
- ID3v2→ID3v1 回退路径和 Ape→ID3 回退路径中，各 parser 调用之间插入 `context.input.clear()` 防止流状态级联污染。
- `MusicTag` 是最终返回对象；解析中间状态保存在内部的 `RawMediaInfo`、`RawMetadata`、`RawLyrics` 等结构中。
- 写入 `MusicTag` 的文本字段必须是 UTF-8。

## 当前完整支持的标签格式

- 当前代码完整处理的标签格式族是：`ID3v1`、`ID3v2.2/v2.3/v2.4`、`FLAC Vorbis Comment`、`Ogg Vorbis Comment`、`MP4 ilst`、`APEv2`。
- `TagFormat::VorbisComment` 目前作为 FLAC Vorbis Comment 分支处理，实际走 `ReadFlacMetadata()` 和 `ReadFlacLyrics()`；`TagFormat::Flac` 也是同一条 Vorbis Comment 解析路径。
- `ID3` 分支覆盖常规文本字段、歌词和封面帧，常规字段包括 `title`、`artist`、`album`、`albumArtist`、`composer`、`genre`、`year`、`track`、`disc`。
- `FLAC Vorbis Comment` 分支同样覆盖上述常规字段，并可解析 `PICTURE` 封面块和歌词相关字段。
- `Ogg Vorbis Comment` 当前只从 Vorbis comment packet 读元数据和歌词，不存在 Ogg 封面导出路径。
- `MP4 ilst` 分支覆盖常规字段、`©lyr` 歌词和 `covr` 封面，歌词也支持 iTunes freeform `----` 中 `com.apple.iTunes` / `lyrics` 项。
- `APEv2` 分支覆盖常规字段、歌词和 `COVER ART (FRONT/BACK)` 二进制封面项。
- 封面导出只支持已有格式里能解析出的内嵌图片来源，具体证据是 ID3 `PIC/APIC`、FLAC `PICTURE`、MP4 `covr`、APE `COVER ART (FRONT/BACK)`；导出后按 content-addressed PNG 缓存复用。
- `DetectTagFormat()` 中 APE footer 优先于 ID3 头/尾检测，因此 MP3+APE 文件会优先按 APE 主字段解析，再用 ID3v2/ID3v1 补缺。
- `APEv1`，也就是 version < 2000 的 APE tag，当前会静默跳过，不属于支持格式。
- 以上支持范围仅指标签格式支持，不表示所有容器都完整支持，也不包括 WMA/ASF、Matroska、WAV LIST INFO、Lyrics3 或 ID3v2.5。

## 项目最终目标覆盖范围

- 最终目标是覆盖已知音频后缀集合中的标签来源：`mp3`、`aac`、`m4a`、`ogg`、`wma`、`opus`、`mpc`、`mp+`、`mpp`、`flac`、`ape`、`wav`、`aiff`、`aif`、`wv`、`tta`、`alac`、`shn`、`tak`、`dsf`、`dff`、`dxd`、`mka`、`webm`、`dts`、`ac3`、`truehd`。
- 这里的“覆盖”指能从对应文件中读取标题、歌手、专辑、年份、流派、歌词、封面等标签信息；基础音频流识别仍由 FFmpeg probe 和 `ReadMediaInfo()` 负责。
- 当前已实现的基础能力继续作为复用层：`ID3v1/v2`、`Vorbis Comment`、`MP4 ilst`、`APEv2` 和封面 PNG 缓存。后续新增格式应优先把容器内嵌标签抽取成这些现有解析器可消费的原始 tag，再补充无法复用的原生标签解析。
- `mp3` 当前以 ID3v1/v2 为主，APEv2 优先检测用于 MP3+APE；如需兼容极少数旧歌词文件，可后续增加可选的 Lyrics3 v2 解析器，但它不是主线目标。
- `m4a`、`alac` 和带 `ftyp` 的 AAC/MP4 家族目标继续走 MP4 `ilst`；裸 `aac` 不新增专属原生标签格式，只在文件实际携带 ID3 或 MP4 容器标签时复用已有路径。
- `flac` 当前走 FLAC Vorbis Comment 和 `PICTURE`；`dxd` 如果封装在 FLAC 中也复用该路径，如果封装在 WAV 中则依赖后续 RIFF/WAV 目标。
- `ogg` 当前只覆盖 Ogg Vorbis comment 元数据和歌词；最终目标需要补齐 Ogg/Opus 的 `METADATA_BLOCK_PICTURE` 字段（Base64 编码 FLAC Picture Block），并为 `opus` 增加 OpusTags/comment packet 路径，而不是把 Opus 当作 Vorbis stream 处理。
- `ape`、`mpc`、`mp+`、`mpp`、`wv`、`tta`、`tak`、`shn` 的主目标不是新增一套标签语法，而是确保这些文件中常见的 APEv2、ID3v2 或 ID3v1 能被现有 parser 扫描到；其中 `shn` 原生通常没有标签，只处理外部工具追加的 APEv2 等尾部 tag。
- `wma` 需要新增 ASF 对象树解析，读取 `Content Description Object`、`Extended Content Description Object` 和 `Metadata Library Object`，并从中映射常规字段、歌词和内嵌封面。
- `mka` 和 `webm` 需要新增 Matroska/EBML 解析，读取 `Tags/SimpleTag` 的 `TagName`/`TagString`，并从 `Attachments/AttachedFile` 中提取封面图片数据。
- `wav` 需要新增 RIFF/WAV 解析：一条路径读取 `LIST`/`INFO` chunk（如 `INAM`、`IART`、`IPRD`、`ICRD`、`IGNR`），另一条路径定位 `id3 ` 或 `ID3 ` chunk 并复用现有 ID3v2 parser。
- `aiff` 和 `aif` 需要新增 IFF/AIFF 解析：原生 `NAME`、`AUTH`、`ANNO`、`(c) ` chunk 映射为基础字段；`ID3 ` chunk 应抽取后复用现有 ID3v2 parser。
- `dsf` 和 `dff` 的目标都是定位容器内嵌 ID3v2：`dsf` 需要按 DSF header 的 metadata pointer 读取 ID3 tag；`dff` 需要在 DSDIFF chunk 树（如 `FRM8`）中寻找常见的 `ID3 ` 或 `DI3v` 载荷。
- `dts`、`ac3`、`truehd` 作为裸音频流不规划独立标签 parser；若要携带标签，最终目标是通过 Matroska (`mka`) 等外层容器读取。
- 后续文档和实现中必须区分“当前已支持”和“最终目标”：新增路线图条目不得被写成当前 public API 已可完整处理的能力。

## 核心标签格式与后缀映射

- 音频后缀与标签格式不是一一对应关系；同一文件可能同时携带多个 tag，例如 `wav` 可同时有 RIFF INFO 和 RIFF 内嵌 ID3v2，`aiff` 可同时有原生 IFF chunk 和 `ID3 ` chunk。
- 后续扩展优先围绕核心标签格式建模，而不是按每个后缀复制一套字段解析逻辑；容器 parser 只负责定位 tag 区域、做边界校验、提取 raw payload，再交给对应 tag parser。

| 核心标签格式 | 目标后缀 | 处理重点 |
| --- | --- | --- |
| `ID3v2` (`v2.3/v2.4`，并保留当前 `v2.2`) | `mp3`、`aac`、`tta`、`wav`、`aiff`、`aif`、`dsf`，以及非标准 `dff` chunk | 支持封面、歌词和多字段文本；`wav`/`aiff`/`dff`/`dsf` 需要先从容器 chunk 或 header metadata pointer 定位 raw ID3v2。 |
| `ID3v1` | `mp3`、`ape`、`mpc`、`mp+`、`mpp`、`wv` 等 | 文件尾 128 字节 legacy fallback，只用于补缺，不作为现代主标签来源。 |
| `Vorbis Comment` | `flac`、`ogg`、`opus` | UTF-8 键值对；`flac` 由 FLAC metadata block 承载，`ogg`/`opus` 由 Ogg comment packet 承载；封面目标是 `METADATA_BLOCK_PICTURE` 或 FLAC `PICTURE`。 |
| `APEv2` | `ape`、`wv`、`tak`、`mpc`、`mp+`、`mpp`、`tta`，以及非标准 MP3 尾部 tag | 键值对和二进制 item；继续保持 APE footer 优先，以便 MP3+APE 使用 APE 主字段再由 ID3 补缺。 |
| `MP4/iTunes Metadata` | `m4a`、`alac`、封装在 MP4/M4A 容器中的 `aac` | QuickTime atom/box 路径 `moov -> udta -> meta -> ilst`，字段如 `©nam`、`©ART`、`aART`、`trkn`、`disk`、`covr`、`©lyr`。 |
| `RIFF INFO` | `wav` | `LIST`/`INFO` 子块中的 FourCC 字段，例如 `INAM`、`IART`、`IPRD`、`ICRD`、`IGNR`；与 RIFF `id3 `/`ID3 ` chunk 并行处理。 |
| `AIFF Native Chunks` | `aiff`、`aif` | IFF chunk 字段如 `NAME`、`AUTH`、`ANNO`、`(c) `；现代文件仍应优先检查内嵌 `ID3 ` chunk。 |
| `ASF Metadata` | `wma` | ASF GUID/object tree，重点对象包括 `Content Description Object`、`Extended Content Description Object`、`Metadata Library Object`。 |
| `Matroska/EBML Tags` | `mka`、`webm` | EBML `Tags/SimpleTag` 提供文本字段，`Attachments/AttachedFile` 提供封面文件名、MIME 和二进制数据。 |

| 文件后缀 | 最终目标标签格式 | 备注 |
| --- | --- | --- |
| `mp3` | `ID3v2`、`ID3v1`、`APEv2` | ID3v2 是主路径；APEv2 多为非标准尾部 tag，但当前策略已优先处理 MP3+APE。 |
| `aac` | `ID3v2` 或 `MP4/iTunes Metadata` | ADTS 裸流常见头部 ID3v2；若封装为 M4A/MP4，则走 MP4 `ilst`。 |
| `m4a`、`alac` | `MP4/iTunes Metadata` | 统一按 MP4 atom tree 处理。 |
| `ogg`、`opus` | `Vorbis Comment` / OpusTags | `ogg` 当前仅覆盖 Vorbis comment；`opus` 需要独立确认 Opus identification/comment packet。 |
| `flac` | `Vorbis Comment`、FLAC `PICTURE` | 当前已有主路径，后续可复用其 Picture Block parser 给 Ogg/Opus 图片字段。 |
| `wma` | `ASF Metadata` | 需要新增 ASF object parser。 |
| `ape`、`wv`、`tak` | `APEv2`、`ID3v1` | APEv2 是主路径，ID3v1 只做 legacy fallback。 |
| `mpc`、`mp+`、`mpp` | `APEv2`、`ID3v1` | Musepack 系列目标以 APEv2 为主。 |
| `tta` | `ID3v2`、`APEv2`、`ID3v1` | TrueAudio 需要同时处理头部 ID3v2、尾部 APEv2/ID3v1 的优先级。 |
| `wav` | `RIFF INFO`、RIFF 内嵌 `ID3v2` | 两条路径都要支持，字段冲突时应定义优先级。 |
| `aiff`、`aif` | `ID3v2`、`AIFF Native Chunks` | `ID3 ` chunk 优先承载现代标签，原生 chunk 作为基础字段来源。 |
| `dsf` | `ID3v2` | 通过 DSF header 中的 metadata pointer 定位 tag，常见位置在文件尾。 |
| `dff` | 非标准内嵌 `ID3v2`（可选目标） | DSDIFF 官方不定义标准 tag；只考虑常见 `DI3v`/`ID3 ` chunk 兼容路径。 |
| `dxd` | 取决于底层封装：`wav`、`flac` 或 `dsf` | 不是独立标签容器；先识别实际封装，再走对应标签路径。 |
| `mka`、`webm` | `Matroska/EBML Tags` | 文本字段来自 `Tags`，封面来自 `Attachments`。 |
| `shn` | 无原生标准；可兼容尾部 `APEv2` | Shorten 原生无标签，外部 sidecar 不纳入核心解析路径。 |
| `dts`、`ac3`、`truehd` | 无标准裸流标签 | 不为裸流规划独立 parser；标准携带标签应依赖 `mka`/`m4a` 等容器。 |

## 架构取舍与外部库评估

- 当前项目定位是自研 raw-byte parser：FFmpeg 只负责 probe、音频流、媒体信息和封面编解码，标签字段仍由本项目解析器从原始字节读取。
- 后续若继续自研，应优先补齐“容器提取器”而不是复制已有 tag parser：RIFF/IFF/DSF/DSDIFF 负责定位 ID3v2，Ogg/Opus 负责定位 comment 和图片字段，ASF/Matroska 由于标签模型不同才需要新增完整 parser。
- TagLib 可作为后续架构评估项：它覆盖大量常见音频 metadata 容器，能显著降低 `MP3/MPEG`、`MP4/M4A/AAC/ALAC`、`Ogg Vorbis/Opus/FLAC`、`ASF/WMA`、`MPC`、`WavPack`、`WAV`、`AIFF`、`TTA`、`DSF/DFF`、`Matroska/WebM`、`Shorten` 等格式的兼容成本。
- TagLib 的支持范围应按官方文档和具体版本确认；它是 metadata/tag 库，不是音频解码器。对 `TAK`、`DXD`、`DTS`、`AC3`、`TrueHD` 等未明确在官方支持矩阵中的格式，不应假定 TagLib 可直接覆盖。
- 即使引入 TagLib，也需要保留本项目的边界决策：`Shorten` 等格式可能只适合读取不适合写回，`ASF/WMA`、`Matroska/WebM`、`DSF/DFF` 的能力应限定为容器元数据能力，不应泛化为所有编码变体或所有非标准 tag 都可处理。
- 引入 TagLib 会改变当前“标题/歌手/专辑/歌词/封面块不依赖外部 tag 库”的边界，也需要重新设计错误处理、资源上限、封面缓存、字段优先级和构建依赖策略。
- 在正式决定引入 TagLib 前，文档和代码仍以当前自研解析路线为准；不得把 TagLib 支持范围写成当前能力，也不得混用 TagLib 与自研 parser 而不定义优先级。

## FFmpeg 与原始字节解析分工

- FFmpeg 用于输入 probe、容器识别、主音频流定位、基础媒体信息读取，以及封面图像解码和 PNG 编码。
- 标题、歌手、专辑、歌词、封面块等标签字段不使用 `AVDictionary` 作为元数据来源。
- 元数据和歌词解析优先通过 `ReadContext::input` 直接读取文件原始字节，再按 ID3、Vorbis/FLAC、Ogg Vorbis、MP4 atom、APE tag 等格式规则解释。
- 封面块来自 ID3 `PIC/APIC`、FLAC `PICTURE`、MP4 `covr`、APE `COVER ART (FRONT/BACK)` 等格式分支；未传入 `coverExportDir` 时导出到系统临时目录下 TagReader 自有子目录，传入时导出到调用方目录。

## 输入与失败策略

- `ValidatePath()` 只做早期路径形态检查，例如空路径、不存在、非普通文件、文件大小边界。
- 真实可读性以 `ifstream.open()`、`avformat_open_input()` 和后续读取结果为准；路径检查不是权限授权判断。
- `OpenContext()` 建立 FFmpeg 上下文和独立文件输入流，后续标签解析继续使用文件输入流读取原始字节。
- 格式解析器遇到 malformed 数据时尽量局部失败：损坏字段、损坏歌词或损坏封面可被跳过，`Read()` 在媒体流和容器仍可用时继续返回可用的 `MusicTag`。
- 输入无效、无法打开、没有可用音频流或容器无法建立时，`Read()` 仍按顶层错误路径失败。

## 元数据与歌词解析

- ID3 分支读取 ID3v1、ID3v2.2、ID3v2.3、ID3v2.4 的已支持文本帧、歌词帧和图片帧。
- Vorbis/FLAC 分支读取 Vorbis Comment entry；单个非法 UTF-8 entry 只影响该 entry，后续合法 entry 仍可解析。
- Ogg Vorbis 分支按 page 和 packet 边界扫描 comment packet，并对截断、continuation、payload 大小设置本地失败。
- MP4 分支通过 atom walker 定位 `moov/udta/meta/ilst`，读取已支持 metadata item、`©lyr` 和 iTunes freeform lyrics。
- MP4 walker 内部使用本地解析状态区分 `Ok`、`NotFound`、`Malformed`、`ResourceLimit`，但这些状态不进入 public API。
- APE 分支通过文件尾 32 字节 footer（魔数 `APETAGEX`）检测 APEv2 tag，解析 item 列表（key-NUL-value 格式）。Tag 级上限为 16 MiB/4096 items；单个文本项上限为 1 MiB。
- APE 格式检测优先于 ID3（`DetectTagFormat()` 中 APE footer 检查在 ID3 header 之前），确保 MP3+APE 文件使用 APE 元数据。
- MP3+APE 路径中，APE 解析为主，ID3v2→ID3v1 回退补充 APE 未提供的字段。
- APEv1（version < 2000）静默跳过；非封面二进制 item 和外部引用 item 静默跳过。
- FLAC `PICTURE` 当前使用 cpp 内部 bounded `ByteCursor` 解析字段长度和图片字节；其它 parser 游标尚未迁移到该 helper。

## 封面导出与缓存

- `Read(path)` 默认优先使用 `XDG_RUNTIME_DIR/tagreader-covers` 作为封面导出目录；不可用时回退到 UID 私有的临时目录，例如 POSIX 上的 `std::filesystem::temp_directory_path() / "tagreader-covers-$UID"`；`Read(path, coverExportDir)` 使用调用方显式提供的目录。
- 实际导出目录会按需创建，必须存在且为目录，并通过写入、读取、删除探针文件的权限验证；显式提供的目录如果是 symlink 会在探针和缓存写入前被拒绝。
- 封面缓存是 content-addressed PNG storage，缓存键基于音频文件内嵌图片原始字节计算。
- 缓存路径格式为 `coverExportDir / first2hex / rest.png`，其中 `first2hex` 是 hash 前两个十六进制字符，`rest.png` 是剩余 hash 加 `.png` 后缀。
- 已存在的缓存路径直接返回，不再重复解码或转码。
- 首次写入时会把内嵌图片解码并统一编码为 PNG；无法解码的封面保持为空或被跳过。

## 构建与测试资产

- 普通构建命令是 `cmake -S . -B build` 和 `cmake --build build`。
- 构建目标包括静态库 `TagReaderCore`、人工验收程序 `TagReaderTest`、安全 smoke 程序 `TagReaderSecuritySmoke`、回归测试程序 `TagReaderRegressionTests`。
- `TagReaderTest` 是字段打印程序，不是单元测试框架；`TagReaderRegressionTests` 对应各 TR-AUDIT 项的回归验证。
- fuzz corpus 由 `python3 test/corpus/generate_corpus.py` 生成，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`；仓库不提交二进制 seed。
- 仓库当前没有配置 CI workflow、lint、formatter 或单元测试框架。
