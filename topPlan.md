# TagReader 顶层模块推进计划

## 规划依据

- 当前完整标签能力是 ID3v1/v2.2/v2.3/v2.4、FLAC Vorbis Comment、Ogg Vorbis Comment、MP4 `ilst`、APEv2；`docs/DESIGN.md` 的最终目标不是当前能力。
- 后续扩展优先围绕“标签来源”建模，不按后缀复制字段解析逻辑：能抽出 raw ID3、Vorbis Comment、MP4 `ilst` 或 APEv2 的目标应复用现有 parser。
- 主流程继续保持 `ValidatePath()` -> `OpenContext()` -> 封面目录校验/硬化 -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`，不要新增独立 `DetectContainer()` 步骤。
- FFmpeg 只负责 probe、音频流、基础媒体信息和封面编解码；标题、歌手、专辑、歌词和封面块仍从文件原始字节解析。
- 每个阶段都必须同步考虑资源上限、局部 malformed 跳过策略、封面缓存错误传播、默认/显式封面目录副作用，以及现有回归/安全 smoke/fuzz 资产。

## 阶段 0：锁定现有基线

目标是先让现有六类 parser 成为可靠复用层，而不是立即扩大后缀列表。

- 巩固 `id3`、`vorbis`、`flac`、`ogg-vorbis`、`mp4`、`ape` 的当前字段、歌词、封面和补缺优先级。
- 保持 APE footer 优先于 ID3、MP3+APE 用 APE 主字段再由 ID3v2/ID3v1 补缺、ID3v2 再由 ID3v1 补缺的现有语义。
- 保持封面导出目录硬化、显式目录 symlink 拒绝、content-addressed PNG 缓存和 `cover cache` 错误上抛语义。
- 验收以现有可执行程序为准：`TagReaderRegressionTests`、`TagReaderSecuritySmoke`、`TagReaderFlacMalformedMetadataTests`、`TagReaderDefaultCoverExportDirectoryTests`、`TagReaderLyricsNormalizeComplexityTests`，必要时补充新的 TR-AUDIT case。

## 阶段 1：拆清检测与复用型分发

目标是先补“能把现有 parser 送到正确 raw tag 的路径”，避免把后缀名误写成已支持能力。

- 已接近可复用的目标：`m4a`、`alac` 继续走 MP4 `ilst`；`mpc`、`wv`、`tak`、`tta` 继续以 APEv2/ID3 fallback 为主，但要验证 `DetectTagFormat()` 和 `ContainerFromTagFormat()` 的实际分发。
- 需要新增或明确分发的目标：裸 `aac`、`mp+`、`mpp`、`shn`。只有文件实际携带 ID3、MP4 容器标签或 APEv2/ID3 尾部时才复用现有 parser，不新增无标准来源的专属标签 parser。
- 这一阶段输出应是明确的检测规则、字段优先级和回归样本，而不是宣称整个后缀族“完整支持”。

## 阶段 2：单独推进 Ogg 与 Opus

目标是补齐 Ogg/Opus comment 家族，但不能把 Opus 当作 Ogg Vorbis 小改。

- `ogg` 当前只覆盖 Vorbis comment 元数据和歌词；后续先补 `METADATA_BLOCK_PICTURE` 等图片字段，再复用 FLAC Picture Block 解析思路导出封面。
- `opus` 需要独立确认 Opus identification/comment packet 和 OpusTags 结构，新增分支时仍复用 Vorbis Comment 字段映射与 UTF-8/资源上限策略。
- Ogg/Opus 新增封面时必须沿用现有封面缓存、64 MiB 输入/输出限制、局部坏封面跳过和 `cover cache` 错误上抛规则。

## 阶段 3：实现可复用 ID3 的容器提取器

目标是为容器内嵌标签定位 raw payload，再交给现有 ID3 或字段规范化链路。

- `wav`：新增 RIFF/WAV 解析，一路读取 `LIST/INFO` 原生字段，另一路定位 `id3 ` 或 `ID3 ` chunk 并复用 ID3v2 parser；需要先定义原生字段与 ID3 字段冲突优先级。
- `aiff`/`aif`：新增 IFF/AIFF chunk 解析，原生 `NAME`、`AUTH`、`ANNO`、`(c) ` 作为基础字段来源，`ID3 ` chunk 优先承载现代标签。
- `dsf`：按 DSF header metadata pointer 定位 ID3v2。
- `dff`：只兼容常见非标准 `ID3 ` 或 `DI3v` chunk，不宣称 DSDIFF 有标准标签。
- `dxd`：不作为独立标签容器；先识别实际封装，再复用 FLAC、WAV 或 DSF 路径。

## 阶段 4：新增独立元数据模型容器

目标是补齐无法自然折叠进 ID3/Vorbis/MP4/APE 的容器模型。

- `wma`/ASF：新增 ASF object tree parser，读取 `Content Description Object`、`Extended Content Description Object`、`Metadata Library Object`，映射常规字段、歌词和内嵌封面。
- `mka`/`webm`/Matroska：新增 EBML/Matroska parser，读取 `Tags/SimpleTag` 文本字段，并从 `Attachments/AttachedFile` 提取封面图片数据。
- 这些分支需要自己的 atom/object/element 资源上限和 malformed 隔离策略，但最终仍输出 `RawMetadata`、`RawLyrics` 并走统一规范化与封面缓存。

## 阶段 5：收束裸流与文档边界

目标是明确不该新增孤立 parser 的边界，避免为没有标准标签来源的裸流造私有路径。

- `dts`、`ac3`、`truehd` 不规划独立标签 parser；如需标签，应通过 Matroska、MP4 等外层容器读取。
- 更新文档时必须区分“当前可读”“检测可达”“目标能力”“明确不支持”，不要把最终目标写成 public API 已完整支持。
- 若后续评估 TagLib，只能作为单独架构决策；在正式决定前不得混用 TagLib 与自研 parser，也不得把 TagLib 支持矩阵写成当前能力。

## 阶段 6：未来 CUE 目录/索引解析

目标是在单曲文件能力稳定后，再评估 CUE 这种 sidecar/album-level 输入，把一张碟或一个目录中的虚拟曲目展开成多首 `MusicTag`。当前阶段 6 尚未实现，Task 14 只审计边界，不新增 public API、目录扫描或批量读取语义。

- CUE 未来不复用现有 `Read(path) -> MusicTag` 入口；如需落地，应设计独立的专辑级入口，避免同名同参仅返回值不同的非法 C++ 重载，也避免改变单曲读取语义。
- Public 返回模型未来应单独设计：每个元素表示 CUE 中的一首虚拟歌曲，`filePath` 指向 `FILE` 引用的真实音频文件，`offset` 来自 `INDEX 01`，`duration` 由下一轨 `INDEX 01` 或源音频总时长推算。
- `MusicTag` 已能承载 CUE track 的主要公开信息：`title`、`artist`、`album`、`albumArtist`、`genre`、`year`、`trackNumber`、`discNumber`、`filePath`、`offset`、`duration`、`coverPath`、`lyrics` 和基础媒体参数。
- 内部中间态未来应保持私有，保留完整 CUE 结构，包括全局字段、`REM`、`FILE`、`TRACK`、`INDEX 00/01/...`、pregap/postgap、未知命令和编码诊断，但这些结构不作为首版 public API 暴露。
- CUE 解析未来要同时支持单文件整碟和多文件分轨；目录入口只在专辑级入口中处理，不修改当前 `ValidatePath()` 对普通 `Read()` 的单文件约束。
- CUE track 字段最终仍应汇入现有规范化链路，复用 `NormalizeMetadata()`、`NormalizeLyrics()`、`ReadMediaInfo()` 产出的媒体参数，以及现有 content-addressed 封面 PNG 缓存。

## 总体优先级

1. 锁定现有 parser、资源上限、封面副作用和回归资产。
2. 补检测与复用型分发：MP4 家族、APEv2/ID3 fallback 家族、裸 `aac` 等边界。
3. 单独推进 Ogg 图片与 OpusTags/comment 路径。
4. 实现 RIFF/IFF/DSF/DSDIFF 类容器提取：WAV、AIFF/AIF、DSF/DFF、DXD。
5. 新增 ASF/WMA 与 Matroska/WebM/MKA 元数据模型。
6. 收束 DTS、AC3、TrueHD 裸流边界与文档表述。
7. 最后评估 CUE 目录/索引解析：仅通过独立专辑级入口建模，并继续复用现有规范化、媒体信息和封面缓存链路。
