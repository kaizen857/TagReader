# TagReader 修复任务清单

本文件依据当前 `BUGS.md` 重新组织修复计划。执行顺序以“先统一文本解码，再修协议解析，再修封面输出，最后清理与回归”为准。

## 强制约束

- `BUGS.md` 顶部新增约束优先级最高，下面任何缺陷描述如果与约束冲突，必须以约束为准。
- 除音频流基础信息外，歌名、歌手、专辑、歌词、年份、曲号、碟号、封面等 tag 字段必须直接从文件原始字节解析，不能通过 FFmpeg metadata 读取。
- FFmpeg 只允许用于 probe、容器识别、音频流、时长、采样率、声道、bitrate 等基础媒体信息。
- `ReadMetadata()` 和 `ReadLyrics()` 只能作为分发函数，不能把具体协议解析细节塞回入口，也不能用大段 lambda 代替拆分。
- 字符串处理必须遵循“先嗅探原始字节编码，再转换为 UTF-8”的流程。
- 对已明确编码的格式，优先按协议字段解码；对历史字段或未明确编码的内容，再进入嗅探和回退流程。
- 目标是支持多种常见本地编码到 UTF-8 的转换，而不是把非 UTF-8 字节直接清空。
- 年份字段只保留年份，不保留月日。
- 图片只保留当前歌曲的 front cover，artist、leaflet、media、icon 等其他图片类型一律不导出。
- 封面最终输出必须是 `.png`；如果源图不是 PNG，必须实际转换后再写出，不能只改扩展名。
- 只处理 `MusicTag` 中已存在的字段，不新增额外公共字段。
- `ParseUInt16()` 超出 `uint16_t` 返回 `0` 是正确行为，不作为缺陷处理。
- 不引入 TagLib 等外部标签解析库。

## 验证要求

- 每个阶段完成后运行 `cmake --build build`。
- 对相关样本运行 `./build/TagReaderTest <audio-file-path>`。
- 重点核对 `title / artist / album / albumArtist / composer / genre / year / trackNumber / discNumber / coverPath / lyricsCount`。
- 封面验证必须确认 `coverPath` 后缀为 `.png`，且文件头确实是 PNG 签名。

## 第 1 阶段：通用文本解码与年份处理

### 1.1 重构文本入口

- 保留 `DecodedField`，但让它明确表示解码结果：`value` 为 UTF-8，`encoding` 记录实际识别到的编码，`success` 标记是否成功转换。
- 将 `NormalizeText(std::string_view)` 改为真正的“原始字节嗅探 + 转码”入口，而不是只做 UTF-8 校验。
- 拆出独立小函数，避免把逻辑堆进入口函数：
- `DetectTextEncoding(std::string_view raw)`
- `DecodeTextToUtf8(std::string_view raw, std::string_view encoding)`
- `DecodeRawText(std::string_view raw)`

### 1.2 扩展编码嗅探

- 保留 BOM 优先级：UTF-8 BOM、UTF-16LE BOM、UTF-16BE BOM。
- 无 BOM 时先验证 UTF-8，合法则直接按 UTF-8 处理。
- 对无 BOM UTF-16，使用更稳健的启发式判断，避免把短二进制片段误判为 UTF-16。
- 对已明确编码的字段，不要再次盲猜。
- 对未明确编码的历史字段，至少提供 Latin-1 回退，不再把所有非 UTF-8 内容直接清空。

### 1.3 修复 UTF-16 解码

- 修正 `ReadUtf16Text()`，让 BOM 决定大小端。
- `ReadId3ByteString()` 中 ID3 encoding `1` 必须走“自动识别 BOM 的 UTF-16 解码”。
- ID3 encoding `2` 继续按 UTF-16BE 无 BOM 处理。
- 正确处理 surrogate pair；遇到非法 surrogate 时必须明确失败或安全跳过，不能生成非法 UTF-8。

### 1.4 修复 NUL 裁剪

- 重写 `TrimText()` 的裁剪逻辑，避免使用含 `\0` 的字符串字面量作为 trim 集合。
- 首尾只裁剪空格、tab、CR、LF、NUL。
- 不删除字符串中间的合法内容；协议层应先按终止符或长度截断。

### 1.5 统一年份解析

- 新增 `ParseYearOnly(std::string_view text)`。
- 只提取文本开头或常见日期格式中的首个四位年份。
- 支持 `2024`、`2024-05-14`、`2024/05/14`、`2024-05`。
- 年份失败返回 `0`。
- 替换所有年份写入点：ID3v1、ID3v2、Vorbis Comment、MP4。

### 1.6 阶段验证

- 准备 UTF-8、UTF-8 BOM、UTF-16LE BOM、UTF-16BE BOM、Latin-1 的样本。
- 验证非 ASCII 字段最终都能进入 `MusicTag` 并保持 UTF-8。
- 验证损坏或未知编码不会崩溃。
- 验证日期格式只输出年份。

## 第 2 阶段：封面统一为 front cover PNG

### 2.1 固化封面选择策略

- 只接受 front cover。
- ID3 APIC / PIC 只接受 picture type `3`。
- FLAC PICTURE 只接受 picture type `3`。
- MP4 `covr` 视为当前歌曲封面，但仍需校验源图数据。
- 非 front cover 一律忽略，`coverPath` 保持空字符串。

### 2.2 固化 `.png` 输出

- 修改封面路径生成逻辑，使输出扩展名固定为 `.png`。
- 停止使用任何固定 `.jpg` 的临时封面路径。
- 保留防覆盖命名策略，但只生成 `.png`。
- 所有封面写出前都要校验目标扩展名是 `.png`。

### 2.3 实现图片识别与转换

- 新增图片格式识别小函数，至少识别 PNG、JPEG。
- 新增 `WriteCoverAsPng(...)`，统一负责 PNG 写出。
- 源图已经是 PNG 时，直接写出原始字节。
- 源图是 JPEG 时，必须实际转换成 PNG 后写出。
- 转换失败时不写伪装文件，不落 `.jpg`。

### 2.4 统一各协议封面入口

- ID3v2 APIC：只对 picture type `3` 进入封面写出。
- ID3v2 PIC：修正结构后只对 picture type `3` 进入封面写出。
- FLAC PICTURE：只对 picture type `3` 进入封面写出。
- MP4 `covr`：校验图片签名后进入统一写出。

### 2.5 阶段验证

- 准备 APIC front cover、APIC 非 front cover、FLAC front cover、MP4 `covr` 样本。
- 验证非 front cover 不导出。
- 验证所有输出路径后缀均为 `.png`。
- 验证输出文件前 8 字节为 PNG 签名。

## 第 3 阶段：ID3v2 metadata 协议修复

### 3.1 修复 ID3v2.2 PIC

- 按 `encoding + image format(3) + picture type + description + terminator + image data` 解析。
- 删除当前额外扫描 NUL 的错误逻辑。
- description 为空时也必须能正确定位 image data。
- 只接受 picture type `3`，并统一走 PNG 导出。

### 3.2 修正 v2.2 / v2.3 / v2.4 frame header 差异

- 明确区分 v2.2 的 6 字节 frame header 与 v2.3/v2.4 的 10 字节 frame header。
- v2.2 的 3 字节 frame id 不能按 4 字节处理。
- 不同版本的 size、flag、payload 边界必须分开处理。

### 3.3 修正 frame 跳过策略

- 先确认 frame id 再解析 frame size。
- 对不支持的 flag，只要能确定 frame size，就整帧跳过。
- 对无法安全解析 size 的情况，停止扫描，不要直接按固定长度硬跳。
- metadata 和 lyrics 的跳过策略保持一致。

### 3.4 完整处理 v2.3 / v2.4 flags

- v2.3：处理或跳过 compression、encryption、grouping identity。
- v2.4：处理或跳过 grouping identity、compression、encryption、unsynchronisation、data length indicator。
- v2.4 的 data length indicator 存在时要正确剥离 4 字节 syncsafe 长度。
- 不支持的压缩/加密 frame 直接跳过，不让整个 tag 失败。

### 3.5 修复 v2.4 extended header / footer

- 按 syncsafe size、flag bytes、flag data 的结构处理 extended header。
- footer flag 要显式处理，不能误读成 frame。
- 非法 extended header 要明确失败或安全跳过，避免错位扫描。

### 3.6 修复 genre 解析

- 新增 `NormalizeId3Genre(std::string_view value)`。
- 支持 `(13)`、`13`、`(13)Pop`、普通文本。
- v2.4 多值 NUL 分隔时取第一个可用值。
- 数字映射使用完整 ID3 genre 表。

### 3.7 统一 ID3 字段覆盖策略

- 同一优先级内采用“已有值不覆盖”，只在当前字段为空时写入。
- ID3v2 优先于 ID3v1。
- year / track / disc 只在目标值为 0 时回填。

### 3.8 阶段验证

- 准备 ID3v2.2 文本 + front cover PIC 样本。
- 准备 ID3v2.3 extended header + APIC 样本。
- 准备 ID3v2.4 flags + data length indicator 样本。
- 验证 TCON 数字 genre、日期只取年份、重复 frame 覆盖策略。

## 第 4 阶段：ID3 歌词修复

### 4.1 实现 ID3v2.2 歌词帧

- 实现 `ReadID3v22LyricsFrames()`。
- 支持 v2.2 `ULT` 纯文本歌词。
- 如需支持 `SLT`，必须明确其解析规则；否则要安全跳过，不误读。

### 4.2 对齐 lyrics 的 frame 处理

- 歌词路径复用 metadata 的 frame size、flag 跳过、v2.4 data length indicator、unsynchronisation 处理。
- 不允许 unsupported frame 只跳固定 10 字节。

### 4.3 修复 `SYLT` 时间轴

- `timestampFormat == 2` 按毫秒换算。
- `timestampFormat == 1` 是 MPEG frames；如果不能稳定换算成微秒，宁可跳过也不要写错时间。
- 其他 timestamp format 跳过。

### 4.4 明确歌词选择顺序

- `USLT` 优先作为纯文本歌词。
- `SYLT` 只在时间戳可正确解释时读取。
- `TXXX` 仅当 description 明确指向歌词时才读取。
- 多语言重复歌词先采用第一个可用值，不覆盖已有歌词。

### 4.5 阶段验证

- 准备 ID3v2.2 `ULT` 样本。
- 准备 ID3v2.3 / v2.4 `USLT` 样本。
- 准备 `SYLT` 毫秒样本。
- 准备 `SYLT` MPEG frames 样本，确认不会产生错误时间轴。
- 普通 `TXXX` 不应误识别为歌词。

## 第 5 阶段：ID3v1 修复

### 5.1 使用统一文本解码

- ID3v1 的 title / artist / album / year 从原始固定长度字节切片进入统一解码流程。
- Latin-1 仍可作为回退，但不再把它当成唯一策略。
- 已经是合法 UTF-8 的字节应原样保留。

### 5.2 修正字段映射

- 修正 `comment` 不应写入 `composer` 的问题。
- `year` 只能从年份字段提取四位年份。
- 其余字段按 ID3v1 规范映射到 `MusicTag` 已有字段。

### 5.3 完整 genre 表

- 扩展 ID3v1 genre 表到常见标准项与扩展项。
- 下标越界保持空值。
- genre 只在当前值为空时回填。

### 5.4 track 语义修正

- 仅当 `buffer[125] == '\0'` 且 `buffer[126] != '\0'` 时写入 track number。
- ID3v1 只能补空字段，不覆盖 ID3v2。

### 5.5 阶段验证

- 准备仅 ID3v1 样本。
- 准备 Latin-1 非 ASCII 样本。
- 准备 UTF-8 字节样本。
- 准备 genre index 80+ 样本。
- 准备 track byte 为 0 的样本。

## 第 6 阶段：Vorbis / FLAC / Ogg 修复

### 6.1 修正 Vorbis 总数字段误写

- `tracktotal` / `totaltracks` 不能写入 `trackNumber`。
- `disctotal` / `totaldiscs` 不能写入 `discNumber`。
- 当前公共模型没有总数字段，这类 key 直接忽略。

### 6.2 Vorbis Comment 文本解码

- 规范路径按 UTF-8 解码。
- 非法 UTF-8 时进入统一嗅探回退。
- 解码失败则忽略该 entry，不清空已有字段。

### 6.3 复用 Ogg comment packet 组包

- 抽出 Ogg Vorbis comment packet 读取函数，metadata 和 lyrics 共用。
- 正确识别 `0x03 + "vorbis"` comment packet。
- 支持跨页 packet。
- 至少校验 Vorbis identification/comment header 顺序，避免误读其他 logical bitstream。

### 6.4 FLAC block 扫描收敛

- Vorbis Comment 和 PICTURE 扫描保持行为一致。
- front cover 只接受 picture type `3`。
- 封面统一走 PNG 写出流程。

### 6.5 阶段验证

- 验证 FLAC Vorbis Comment 文本字段。
- 验证 FLAC front cover PNG / JPEG 源图均导出为 PNG。
- 验证 Ogg 跨页 comment metadata。
- 验证 Ogg 跨页 lyrics。
- 验证 totaltracks / totaldiscs 不污染当前编号。

## 第 7 阶段：MP4 / M4A 修复

### 7.1 严格限制 metadata 路径

- `ReadMP4AtomTree()` 只在 `moov/udta/meta/ilst` 路径下识别 metadata item。
- `meta` box 必须按 full box 处理；遇到非 full box 变体时要显式分支或保守跳过。
- 不在任意层级解析同名 atom。

### 7.2 增加 MP4 年份读取

- 支持 `©day`，必要时支持 `date`。
- 文本解码后统一调用 `ParseYearOnly()`。
- 只写入年份，不保留月日。

### 7.3 修正 `trkn` / `disk`

- 校验 data type 为合理的 integer / implicit 类型。
- payload 长度至少满足读取当前编号所需结构。
- 只读取当前 track / disc 编号，不读取总数。
- 编号为 0 时不写入。

### 7.4 修复 MP4 文本和歌词编码

- text data type `1` 按 UTF-8 处理。
- data type `0` 作为 implicit/text 时也进入统一嗅探解码。
- 如果支持 UTF-16 类型，要明确 data type 并走 UTF-16 解码；否则保守跳过。
- `©lyr` 使用明确字节序列定义，避免源码字符编码依赖。

### 7.5 拆出 MP4 lyrics 扫描

- 删除 `ReadMP4Lyrics()` 内过大的递归 lambda。
- 新增小函数，例如 `ReadMP4LyricsAtomTree(...)` 与 `ReadMP4LyricsItem(...)`。
- 保持 `ReadLyrics()` 只做分发。

### 7.6 修复 MP4 `covr`

- `covr` 只接受 JPEG / PNG 源图签名。
- data type 和签名冲突时以签名和实际内容为准，冲突则保守跳过。
- 输出统一走 `WriteCoverAsPng()`。

### 7.7 阶段验证

- 准备 M4A 文本字段样本。
- 准备 `©day=2024-05-14` 样本，验证 `year=2024`。
- 准备 `trkn` / `disk` 样本，验证当前编号正确、总数不污染。
- 准备 `©lyr` 样本。
- 准备 `covr` JPEG / PNG 样本，验证均导出 `.png`。

## 第 8 阶段：清理 FFmpeg metadata 依赖与边界代码

### 8.1 清除 tag 字段的 FFmpeg metadata 依赖

- 删除或替换所有通过 `av_dict_get` / `GetDictionaryValue()` 读取歌名、歌手、专辑、歌词、封面等字段的代码。
- 保留 FFmpeg 的容器探测和音频流信息读取逻辑。

### 8.2 改进分发兜底

- `ReadMetadata()` / `ReadLyrics()` 可继续优先使用容器名分发。
- 增加文件签名兜底：ID3、fLaC、OggS、MP4 box。
- 分发过程不得读取 FFmpeg metadata 字段。

### 8.3 修复边界工具

- 确认 `ParseUInt16()` 的溢出返回 `0` 行为保持不变。
- `ReadRange()` 检查 offset 和 size 的转换边界。
- atom size / block size 计算统一做溢出保护。

### 8.4 清理误导性封面代码

- 删除固定 `.jpg` 的旧临时封面路径。
- 删除或废弃不再使用的旧封面写出路径。
- 注释说明封面统一输出 PNG。

### 8.5 阶段验证

- 全仓搜索确认没有 tag 字段通过 FFmpeg metadata 读取。
- 构建通过。
- 随机样本不发生行为回退。

## 第 9 阶段：整体回归验收

### 9.1 构建

- 运行 `cmake --build build`。
- 如果 `build/` 不存在，先运行 `cmake -S . -B build`。

### 9.2 样本矩阵

- ID3v1：Latin-1、UTF-8 字节、genre 80+、track 0。
- ID3v2.2：文本、genre、year、front cover PIC、ULT / SLT。
- ID3v2.3：extended header、APIC front cover、USLT、SYLT milliseconds。
- ID3v2.4：data length indicator、frame unsync、TDRC 日期、TCON 多值。
- FLAC：Vorbis Comment、front cover PICTURE、totaltracks / totaldiscs。
- Ogg Vorbis：跨页 metadata、跨页 lyrics。
- MP4 / M4A：文本字段、`©day`、`trkn`、`disk`、`©lyr`、`covr`。

### 9.3 验收标准

- 所有 tag 字段均来自文件原始字节解析，不来自 FFmpeg metadata。
- 原始字符串先完成编码嗅探，再转为 UTF-8。
- 年份只输出年份数字。
- 只导出当前歌曲 front cover。
- 封面导出路径后缀为 `.png`，文件内容为 PNG。
- 普通 `TXXX` 不会误识别为歌词。
- totaltracks / totaldiscs 不污染当前 track / disc 编号。
- 构建通过，且样本输出符合预期。
