# CUE 相关文件处理调研

## 结论

项目需要支持 CUE，但不应把 CUE 塞进现有 `TagReader::Read(path) -> MusicTag` 单文件入口。最佳处理方式是：保持现有 `Read()` 语义不变，新增一个明确的 CUE/专辑级函数，读取 `.cue` 后返回一个装载该 CUE 中所有歌曲信息的 `MusicTag` 集合；每个 `MusicTag` 对应 CUE 中的一首虚拟 track，并尽量填满 `MusicTag` 中可得字段，复用现有元数据规范化、媒体信息读取和封面缓存链路。

当前已进入实现后的维护阶段：`ReadCueSheet` 已落地，后续只需围绕 CUE parser、路径安全、字段补全、封面 fallback、失败策略和测试迁移做增量维护。

## 当前仓库事实

- `include/TagReader.hpp` 只暴露 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`，返回值都是单个 `MusicTag`。
- `src/TagReader.cpp` 只是把两个 public API 转发到 `tagreader_core::ReadTag()`。
- `docs/DESIGN.md` 现已说明 `CUE` 通过显式 `ReadCueSheet` 支持 sidecar / album-level 输入；仍不支持把它塞进 `Read()`、目录输入或 `ReadAlbum`。
- `test/regression/regression_tests.cpp` 中的 `TR-AUDIT-056` 现改为守住新边界：允许 `ReadCueSheet`，继续禁止 `Read()` 批量返回与 `ReadAlbum`。
- 当前测试框架是 Catch2 + CTest + CMakePresets，默认验证入口是 `cmake --preset default`、`cmake --build --preset default`、`ctest --preset default --output-on-failure`。
- 项目架构要求标题、歌手、专辑、歌词和封面块来自文件原始字节解析，不能改成依赖 FFmpeg `AVDictionary`。

## CUE 格式调研

### 格式定位

CUE sheet 是纯文本命令文件，用来描述光盘或音频镜像的布局。它不是音频文件内嵌 tag，也不是单首歌的元数据容器。常见场景是一个 `.cue` sidecar 引用一个整碟音频文件，或引用多个音频文件，并通过 track/index 把它们切成逻辑曲目。

GNU `ccd2cue` 文档把 CUE 定义为由命令、参数、空白和行组成的简单文本文件；命令大小写不敏感，可缩进，空行被忽略。文档还把上下文分为全局、`FILE` 和 `TRACK` 三类，说明 `FILE`、`TRACK`、`INDEX`、`REM` 可以多次出现。

Debian `cue2toc` manpage 也把 CUE 描述为描述 CD-ROM 布局的文本文件，常见音频 CUE 会引用 MP3、Ogg Vorbis 等压缩音频文件，并需要工具把它们转换或映射成可刻录布局。

### 关键命令

- `FILE`：声明后续 track 使用的音频或数据文件，以及类型，例如 `WAVE`、`MP3`、`BINARY`。
- `TRACK`：声明 track 编号和模式；音频场景通常是 `AUDIO`。
- `INDEX`：声明 track 内的索引点，时间格式是 `mm:ss:ff`，其中 `ff` 是 CD frame。
- `TITLE`、`PERFORMER`、`SONGWRITER`：可出现在全局或 track 上下文，分别表示整碟或单曲的 CD-Text 字段。
- `REM`：注释；现实工具常把 `REM GENRE`、`REM DATE` 等非标准字段用作扩展元数据。
- `PREGAP`、`POSTGAP`：声明 pregap/postgap；`PREGAP` 与 `INDEX 00` 在不少工具语义中互斥。
- `FLAGS`、`ISRC`、`CATALOG`、`CDTEXTFILE`：分别表示 track flags、单曲 ISRC、整碟 catalog/UPC/EAN、外部 CD-Text 文件。

### 时间与 track 语义

- `INDEX 01` 是通常意义上的 track 起点。
- `INDEX 00` 可表示 pregap；第一轨的 `INDEX 00` 还可能涉及 Hidden Track One Audio 这类特殊场景。
- `INDEX 02..99` 可表示 sub-index，但对普通音乐库场景通常不是首版必须公开的信息。
- CUE 时间 `mm:ss:ff` 中每秒有 75 frame；换算到项目现有 `MusicTag::offset()` / `duration()` 时，应统一转成微秒。
- track duration 通常不能只从当前 track 本身得到，需要用下一轨 `INDEX 01`、下一个 `FILE`、或被引用音频文件总时长推导。

### 现实兼容性

`libcue` README 明确提示：CUE 被很多播放器和抓轨工具支持，但没有单一严格标准完整描述所有语法；库通常只尝试解析常见布局，不声称支持所有组合。这意味着 TagReader 若未来支持 CUE，应按“宽容解析、明确资源上限、测试驱动兼容样本”的方式推进，而不是一次性承诺完整 CUE 生态覆盖。

播放器和工具对 CUE 的支持范围也不一致。Hydrogenaudio 和 Kodi 都把 CUE 作为单大文件或多文件专辑的虚拟曲目/播放列表描述；mpv 的 CUE parser 则只关注播放所需的 `FILE`、`TRACK`、`INDEX`、`TITLE`、`PERFORMER` 子集，忽略 `CATALOG`、`CDTEXTFILE`、`FLAGS`、`ISRC`、`PREGAP`、`POSTGAP`、`REM` 等命令。这说明首版 TagReader 不需要把所有 CUE/CD-Text/刻录语义都提升为 public 字段，应先覆盖音乐库最需要的 track 展开和基础元数据映射。

还需要区分 `.cue` 文本 sidecar 与 FLAC 的内嵌 `CUESHEET` metadata block。RFC 9639 定义的 FLAC `CUESHEET` 是二进制 metadata block，位于 FLAC 文件内部；它与外部 `.cue` 文本文件不是同一输入层级。TagReader 未来处理 `.cue` 时不应把 FLAC 内嵌 `CUESHEET` 误写成已支持的 sidecar 能力，反之亦然。

## 为什么不放进现有 `Read()`

1. 语义不匹配：`Read(path)` 当前读取一个普通文件并返回一个 `MusicTag`；CUE 通常代表一个专辑布局，会产生多首虚拟 track。
2. C++ 无法只靠返回类型重载：不能新增同名同参但返回 `std::vector<MusicTag>` 的 `Read(path)`。
3. 安全边界不同：普通音频读取只处理传入文件；CUE 会解析 `FILE` 引用，涉及相对路径、目录穿越、symlink、缺失文件和多文件聚合。
4. 失败模型不同：单文件 malformed tag 可以局部跳过；CUE 中一条坏 track、一个坏引用文件或时间倒退，需要定义专辑级部分失败策略。
5. 测试边界需要迁移：`TR-AUDIT-056` 现应允许显式 `ReadCueSheet` 函数存在，同时继续禁止 `Read()` 批量返回；`ReadAlbum` 仍不应出现。

## 推荐方案

### 当前实现边界

- `ReadCueSheet(cuePath)` 和 `ReadCueSheet(cuePath, coverExportDir)` 已可用，返回 `std::vector<MusicTag>`。
- `TagReader::Read()` 的返回类型和单文件语义保持不变。
- `ReadCueSheet` 只处理显式传入的 `.cue` 文件，不隐式扫描目录，也不把 CUE 塞回 `Read()`。
- CUE 仍然不通过 FFmpeg `AVDictionary` 读取字段。
- `ReadCueSheet` 的失败策略已定：单文件缺失音频整体失败，多文件缺失音频跳过对应 tracks 并返回其余有效结果。

`MusicTag` 当前已有 `title`、`artist`、`album`、`albumArtist`、`composer`、`year`、`trackNumber`、`discNumber`、`filePath`、`offset`、`duration`、`coverPath`、`lyrics` 和媒体参数字段，足以承载首版 CUE track 的核心公开信息。CUE parser 仍应保留私有中间态，记录全局字段、track 字段、`REM` 扩展、`INDEX 00/01/...`、pregap/postgap、未知命令和解析诊断。映射阶段应尽量填满每个 `MusicTag`：优先使用 CUE 中更精确的 track/album 信息，再用被引用音频文件自身读取到的媒体参数、封面、歌词和缺失标签补全。

## 字段映射建议

| CUE 来源 | 未来映射建议 |
| --- | --- |
| 全局 `TITLE` | `album` |
| 全局 `PERFORMER` | `albumArtist`，若 track 缺少 performer 可作为 `artist` fallback |
| 全局 `SONGWRITER` | 可映射到默认 `composer` fallback |
| `REM GENRE` | `genre` |
| `REM DATE` / `REM YEAR` | `year`，只接受明确年份 |
| track `TITLE` | `title` |
| track `PERFORMER` | `artist` |
| track `SONGWRITER` | `composer` |
| track 编号 | `trackNumber` |
| `FILE` | `filePath`，解析为受限后的真实音频路径 |
| `INDEX 01` | `offset`，换算为微秒 |
| 下一轨 `INDEX 01` 或源文件总时长 | `duration`，换算为微秒 |
| `ISRC`、`CATALOG`、`FLAGS`、`CDTEXTFILE` | 首版不进 public `MusicTag`，保留在私有诊断或未来扩展模型 |

字段补全采用“CUE 优先、两边互补”的规则：当 CUE 文件和音频文件都提供同一字段时，优先使用来自 CUE 的字段；其他字段则哪边有就用哪边。CUE track 级字段覆盖对应 track 的标题、艺人、作曲家和 track number；CUE 全局字段提供 album、album artist、genre/year fallback；引用音频文件读取到的 sample rate、bit depth、bit rate、channels、format、lastModified、lyrics、内嵌封面和 CUE 未提供的普通 tag 字段用于补缺。若同一音频文件承载多个 CUE track，媒体参数和封面可复用，但 `offset`、`duration`、`title`、`artist`、`trackNumber` 必须按 track 单独设置。

封面处理应分两级：优先使用 `FILE` 引用音频文件中的内嵌封面，并继续复用现有封面解析、PNG 编码和 content-addressed cache；如果该音频文件没有可用封面，则在该音频文件所在目录中查找 cover-like 图像文件作为 fallback。只在同一目录查找普通文件，不递归、不跨目录、不跟随不安全 symlink；候选文件名优先级按 `cover`、`front`、`folder`、`album`、`artwork` 处理。候选扩展名不限定为固定列表，只要求是图像文件扩展名；找到图片后先统一转换成 PNG，再按现有封面存储操作写入对应目录，最终填入每个受影响 track 的 `coverPath`。`ReadCueSheet(cuePath, coverExportDir)` 应把外部封面图像也导出到调用方指定目录；无 `coverExportDir` 的重载则沿用默认封面导出目录策略。

## 失败策略

- 单音频文件 CUE：如果唯一 `FILE` 引用的音频文件缺失、不可读或无法建立媒体上下文，则整个 CUE 读取失败。
- 多音频文件 CUE：如果某个 `FILE` 引用的音频文件缺失、不可读或无法建立媒体上下文，则跳过该文件对应的 track；其它可用文件对应的 track 继续返回。
- CUE 文件自身不可读、无法转换为 UTF-8、语法结构无法建立基本 track 列表、所有 track 都不可用，均视为整个 CUE 失败。
- 单个 track 的局部字段 malformed 时，应尽量跳过该字段并保留该 track 的其它可用信息。

## 编码策略

不同文本编码的 CUE 必须先做编码嗅探，再转换为 UTF-8；后续解析、字段规范化和 `MusicTag` 填充只处理 UTF-8 字符。首版编码策略应至少包含 BOM 检测和 UTF-8 验证；非 UTF-8 CUE 进入 fallback 解码路径，成功转成 UTF-8 后再进入同一 parser。无论 fallback 采用 iconv 还是现有文本 codec，都不能让非 UTF-8 字符串直接进入 `MusicTag`。

## 安全与资源上限建议

未来实现必须先定义上限和路径策略：

- CUE 文件大小上限：建议首版 1 MiB 或 4 MiB，避免任意文本 DoS。
- 行数上限：建议 10000 行以内。
- track 数上限：CD 语义通常最多 99，首版可直接限制 99。
- 每个 track 的 index 数上限：按 CUE 规则限制 0..99。
- 字段长度上限：单个文本字段建议 64 KiB 以内；CD-Text 规范更短，但现实 `REM` 可能更长。
- `FILE` 引用数量上限：建议 256 以内。
- 解析路径必须以 `.cue` 所在目录为基准解析相对路径。
- 默认拒绝绝对路径、`..` 逃逸、symlink 逃逸、目录输入和非普通文件。
- 不自动递归扫描目录，不跟随 CUE 中的任意外部引用。
- 多文件 CUE 中，每个引用文件仍走现有 `ReadMediaInfo()` / `ReadMetadata()` 能力；缺失或不可读引用按失败策略处理：单文件 CUE 整体失败，多文件 CUE 跳过对应 track。
- 时间必须单调：同一文件内后续 track 的 `INDEX 01` 不能早于前一轨；duration 不能为负。
- 编码要明确：必须先嗅探并转换为 UTF-8，后续 parser 只处理 UTF-8；不要悄悄产生非 UTF-8 `MusicTag` 字段。
- 所有数字解析都必须检查范围和溢出，尤其是 `TRACK`、`INDEX`、`mm:ss:ff` 和换算到微秒的乘法。`libcue` 的 CVE-2023-43641 就是 `INDEX` 数字经不安全转换后导致越界写的真实案例。
- 对不进入首版 public 映射的命令，例如 `CATALOG`、`CDTEXTFILE`、`FLAGS`、`ISRC`、`PREGAP`、`POSTGAP`、大多数 `REM`，应安全解析或跳过，但不能因为未知命令破坏后续合法 track。
- sidecar 封面 fallback 只能查找 `FILE` 引用音频的同目录普通图像文件；必须限制候选数量、文件大小和图像扩展名集合，并拒绝目录穿越、递归扫描、绝对外部引用和 symlink 逃逸。

## 建议的未来 TDD 路线

1. 先更新 `TR-AUDIT-056` 的目标：允许新的显式 CUE 函数存在，但继续确认 `Read()` 返回单个 `MusicTag`，不隐式读取 `.cue`，不返回 `std::vector<MusicTag>`。
2. 新增 CUE API 之前，先写失败测试锁定：`Read()` 不隐式读取 `.cue`、目录输入仍失败、CUE 不改变单文件行为。
3. 为 CUE parser 写纯文本单元测试：命令大小写、引号、空白、全局/track 上下文、`REM`、未知命令。
4. 为时间解析写边界测试：`00:00:00`、`ff=74`、非法 `ff=75`、时间倒退、缺少 `INDEX 01`。
5. 为路径解析写安全测试：相对路径、绝对路径拒绝、`..` 逃逸拒绝、symlink 逃逸拒绝、缺失文件。
6. 为单文件整碟和多文件分轨分别写集成测试。
7. 为字段映射写回归测试：全局 album/albumArtist fallback、track title/artist override、`REM GENRE`、`REM DATE`、引用音频媒体参数补全、同一音频多 track 独立 offset/duration。
8. 为失败策略写回归测试：单文件 CUE 引用缺失音频时整体失败；多文件 CUE 引用缺失音频时跳过对应 track 并返回其它 track。
9. 为编码写回归测试：UTF-8/BOM/fallback 编码输入都先转成 UTF-8，非法编码整体失败或按策略报错。
10. 为封面写回归测试：音频内嵌封面优先；无内嵌封面时命中同目录 `cover/front/folder/album/artwork` 图像；任意允许图像扩展名先转 PNG 再走统一缓存；不递归、不跨目录、不跟随不安全 symlink。
11. 最后再接入 CUE public API，并保留 audit，确保新能力是显式入口而不是改变旧入口。

## 已确认的设计决策

- 新函数名：`ReadCueSheet`。
- 对外重载：`ReadCueSheet(cuePath)` 和 `ReadCueSheet(cuePath, coverExportDir)` 已实现。
- 返回类型：`std::vector<MusicTag>`。
- 字段优先级：CUE 与音频都有同一字段时优先使用 CUE；其它字段哪边有就用哪边。
- 缺失引用音频：单文件 CUE 整体失败；多文件 CUE 跳过对应 track。
- 编码：先嗅探 CUE 文本编码，转换为 UTF-8 后再解析。
- 外部封面：音频无内嵌封面时，在音频同目录按 `cover`、`front`、`folder`、`album`、`artwork` 顺序查找图像文件；图像扩展名不固定为少数几种，只要判定为图像文件扩展名即可；找到后转 PNG，并按现有封面存储逻辑放入对应目录。

## 可参考资料

- GNU `ccd2cue` manual, “CUE sheet format”：说明 CUE 是按行命令文本，命令大小写不敏感，有全局、`FILE`、`TRACK` 三类上下文，并列出 `CATALOG`、`CDTEXTFILE`、`TITLE`、`PERFORMER`、`SONGWRITER`、`FILE`、`TRACK`、`FLAGS`、`ISRC`、`PREGAP`、`INDEX`、`POSTGAP`、`REM` 等命令。
- GNU `ccd2cue` manual, “INDEX”：说明 `INDEX 01` 是 track 起点，`INDEX 00` 可表示 pregap，时间是相对当前 `FILE` 的 `mm:ss:ff`，每秒 75 frame。
- Debian `cue2toc(1)` manpage：说明 CUE 是描述 CD-ROM 布局的文本文件；track 数为 1 到 99；时间码 frame 范围是 0 到 74；`INDEX 0` 与 `PREGAP` 互斥；`FILE` 可影响后续 track。
- `lipnitsk/libcue` README：说明 CUE 没有单一严格标准覆盖所有语法，库只尝试解析常见布局，并要求新功能附带测试用例。
- Hydrogenaudio “Cue sheet” 与 Kodi “Cue sheets”：说明 CUE 在播放器生态中常用于把单大文件或多文件专辑展开为虚拟曲目。
- `libodraw` CUE sheet format 文档：提示 CUE 文本编码在现实中可能是 extended ASCII 或 UTF-8，支持时必须显式处理编码策略。
- `libcue` security advisory GHSA-5982-x7hv-r9cj / CVE-2023-43641：说明 CUE `INDEX` 数字解析曾触发整数溢出和越界写，资源上限与安全整数转换必须作为首版设计项。
- mpv `demux/cue.c`：显示播放器实现可以只解析播放所需的 CUE 子集，而不是完整暴露所有 CD-Text/刻录命令。
- RFC 9639 FLAC `CUESHEET`：说明 FLAC 内嵌二进制 `CUESHEET` metadata block 与外部 `.cue` sidecar 是不同能力。

## 最终判断

CUE 是项目需要支持的目标能力，但它不是“再加一个嵌入式标签 parser”这么简单，而是 sidecar/album-level 解析能力。最稳妥的路线是：先确认本文档中的 API 边界和行为策略；再单独制定实现计划；最后新增显式 `ReadCueSheet` 函数，把 CUE track 映射成尽量完整的 `std::vector<MusicTag>`，复用已有 raw-byte parser、规范化、媒体信息、内嵌封面缓存和同目录 sidecar 封面 fallback 能力，同时保持现有 `Read()` 单文件语义不变。
