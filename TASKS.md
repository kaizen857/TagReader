# TagReader 修复任务清单

本文件根据当前 `BUGS.md` 和最新约束重新规划代码修复步骤。执行时优先修通用字符串解码能力，再修协议级错读和字段映射，最后做封面 PNG 导出、清理和整体回归。

## 强制约束

- 除音频流信息外，歌名、歌手、专辑、歌词、年份、曲号、封面等 tag 字段必须直接读取文件原始字节解析，不能通过 FFmpeg metadata 获取。
- FFmpeg 只允许用于 probe、容器识别、音频流、时长、采样率、声道、bitrate 等基础媒体信息。
- `ReadMetadata()` 和 `ReadLyrics()` 只能作为分发函数，不能塞入具体协议解析代码，也不能用大段 lambda 规避拆分。
- 字符串处理流程必须是：先嗅探当前原始字节编码，再按嗅探结果转换为 UTF-8。
- 年份字段只读取年份，不考虑月和日；例如 `2024-05-14`、`2024/05/14`、`2024-05` 均只写入 `2024`。
- 图片只提取当前歌曲的封面图片，不提取 artist、leaflet、media、icon 等其他类型图片。
- 封面导出文件统一导出为 `.png`，即使源图片是 JPEG，也需要写出 PNG 文件；如果无法转成 PNG，则不写错误扩展名文件。
- 不引入 TagLib 等标签解析库。

## 验证要求

- 每个阶段完成后运行 `cmake --build build`。
- 对相关样本运行 `./build/TagReaderTest <audio-file-path>`。
- 重点核对 `title / artist / album / albumArtist / composer / genre / year / trackNumber / discNumber / coverPath / lyricsCount`。
- 封面验证必须确认 `coverPath` 后缀为 `.png`，且文件内容确实是 PNG 签名。

## 第 1 阶段：通用字符串解码与年份解析

### 1.1 重构 `DecodedField` 和文本入口

- 保留 `DecodedField` 结构，但让它真正表达嗅探结果：`value` 为 UTF-8 字符串，`encoding` 为嗅探出的编码名，`success` 表示是否成功转成 UTF-8。
- 将 `NormalizeText(std::string_view)` 改为对原始字节做嗅探和转码，而不是只做 UTF-8 校验。
- 新增或拆分小函数，避免把逻辑塞进 `NormalizeText()`：
- `DetectTextEncoding(std::string_view raw)`
- `DecodeTextToUtf8(std::string_view raw, std::string_view encoding)`
- `DecodeRawText(std::string_view raw)`
- 保留 `RemoveUtf8Bom()`，但应纳入 UTF-8 解码路径。

### 1.2 实现最小可用编码嗅探顺序

- BOM 优先：UTF-8 BOM、UTF-16LE BOM、UTF-16BE BOM。
- 无 BOM 时先验证 UTF-8，合法则按 UTF-8。
- 检测 UTF-16LE / UTF-16BE 的 NUL 字节分布，用于处理无 BOM UTF-16 文本。
- 对 ID3 encoding byte 已明确给出编码的路径，不再重复猜测；直接按指定编码解码。
- 对未明确编码的历史字节，至少提供 Latin-1 回退，保证不会把每个非 UTF-8 字段直接清空。
- 如果后续决定支持 GBK / Big5 / Shift-JIS，应在本阶段新增独立解码器或明确依赖策略；未实现前不要在文档或代码中声称支持这些编码。

### 1.3 修复 UTF-16 转 UTF-8

- 修改 `ReadUtf16Text()`，让 UTF-16 with BOM 路径按 BOM 决定大小端。
- `ReadId3ByteString()` 对 ID3 encoding `1` 必须调用“带 BOM 自动识别”的 UTF-16 解码。
- ID3 encoding `2` 继续按 UTF-16BE 无 BOM 处理。
- 正确处理 surrogate pair；遇到非法 surrogate 时跳过该 code point 或失败，但不能生成非法 UTF-8。

### 1.4 修复 `TrimText()` 的 NUL 处理

- 不再使用包含 `\0` 的 C 字符串字面量作为 trim 字符集。
- 改为显式谓词 trim：空格、tab、CR、LF、NUL 都应可从首尾去除。
- 确认不会删除字符串中间合法 NUL 之后的数据，具体协议读取阶段应先按终止符截断。

### 1.5 统一协议文本读取入口

- ID3 文本继续以 encoding byte 为优先，不走盲猜。
- ID3v1 字段调用 `DecodeRawText()` 或明确的 Latin-1/嗅探入口，不再永远固定 Latin-1。
- Vorbis Comment 规范上按 UTF-8 读取，非法 UTF-8 时可进入嗅探回退，但要记录为兼容路径。
- MP4 text data atom 默认按 UTF-8，非法时进入嗅探回退。
- 歌词文本同样进入统一解码入口，保证 metadata 和 lyrics 行为一致。

### 1.6 统一年份解析为“只取年”

- 新增 `ParseYearOnly(std::string_view text)`。
- 行为：从文本开头或常见日期格式中提取第一个 4 位年份；只接受合理范围，建议 `1000..9999`。
- `2024`、`2024-05-14`、`2024/05/14`、`2024-05` 都返回 `2024`。
- 年份解析失败返回 0，不截断超大数字。
- 替换 metadata 中所有年份写入点：ID3v1、ID3v2、Vorbis Comment、MP4。

### 1.7 阶段验证

- 构造或准备 UTF-8、UTF-8 BOM、UTF-16LE BOM、UTF-16BE BOM、Latin-1 的 tag 样本。
- 验证非 ASCII 文本进入 `MusicTag` 后均为 UTF-8。
- 验证非法或未知编码不会导致程序崩溃。
- 验证日期格式只输出年份。

## 第 2 阶段：封面提取统一为当前歌曲封面 PNG

### 2.1 定义封面选择策略

- 只接受“当前歌曲封面”类型。
- ID3 APIC / PIC 只接受 picture type `3` front cover。
- FLAC PICTURE 只接受 picture type `3` front cover。
- MP4 `covr` 视为当前歌曲封面，但仍需校验图片字节。
- 非 front cover 图片一律忽略，不作为兜底导出。
- 如果没有 front cover，`coverPath` 保持空。

### 2.2 统一 PNG 输出路径

- 修改 `MakeCoverPathForAudioFile()` 调用策略，使导出扩展名固定为 `.png`。
- 删除或停止使用固定 `.jpg` 的 `MakeTempCoverPath()`。
- 保留防覆盖命名策略：音频 stem + timestamp + counter。
- 所有封面写出前必须确认目标路径后缀为 `.png`。

### 2.3 实现图片格式识别和 PNG 转换

- 新增小函数：
- `DetectImageFormat(const uint8_t *data, std::size_t size)`，至少识别 PNG、JPEG。
- `WriteCoverAsPng(const std::filesystem::path &audioPath, const uint8_t *data, std::size_t size)`。
- 如果源图已经是 PNG，直接写出原始字节到 `.png`。
- 如果源图是 JPEG，需要转换为 PNG 后写出。
- 优先评估是否可使用现有 FFmpeg/libavcodec 做图片解码编码；如果实现成本过高，应明确失败并不写 `.jpg` 伪装文件。
- 不允许把 JPEG 原始字节写进 `.png` 文件。

### 2.4 改造各协议封面路径

- ID3v2.3/v2.4 `APIC`：解析 picture type，只有 `3` 调用 `WriteCoverAsPng()`。
- ID3v2.2 `PIC`：修正结构解析后，只有 picture type `3` 调用 `WriteCoverAsPng()`。
- FLAC `PICTURE`：只有 picture type `3` 调用 `WriteCoverAsPng()`。
- MP4 `covr`：校验 `dataType` 和图片签名后调用 `WriteCoverAsPng()`。
- 如果转换失败，保持 `coverPath` 为空，不写错误文件。

### 2.5 阶段验证

- 准备 ID3 APIC front cover PNG、APIC non-front cover、FLAC front cover、MP4 covr 样本。
- 验证 non-front cover 不导出。
- 验证导出路径后缀均为 `.png`。
- 验证导出文件前 8 字节为 PNG 签名。

## 第 3 阶段：ID3v2 Metadata 协议修复

### 3.1 修复 ID3v2.2 `PIC` 解析

- 按结构 `encoding + image format(3) + picture type + description + terminator + image data` 解析。
- 删除当前额外扫描 NUL 的错误逻辑。
- description 为空时也必须能定位到 image data。
- 只接受 picture type `3`，并统一走 PNG 导出。

### 3.2 修复 ID3v2.3/v2.4 frame 跳过策略

- 在 frame id 合法后先解析 frame size。
- 对 unsupported flags，在能确定 frame size 时跳过完整 `10 + frameSize`。
- 对无法安全解析 frame size 的情况停止扫描，不要 `cursor += 10` 落入 payload 中间。
- metadata 和 lyrics 路径使用一致的策略。

### 3.3 完整区分 v2.3 / v2.4 frame flags

- v2.3：处理或跳过 compression、encryption、grouping identity。
- v2.4：处理或跳过 grouping identity、compression、encryption、unsynchronisation、data length indicator。
- v2.4 data length indicator 存在时正确剥离 4 字节 syncsafe 长度。
- 不支持 compression/encryption 时跳过该 frame，不抛出整个 tag 失败。

### 3.4 修复 ID3v2.4 extended header / footer

- v2.4 extended header 按 syncsafe size、number of flag bytes、flag data 解析和跳过。
- 对 footer flag 做显式处理和注释；tag body 读取仍以 header size 为准，但不能误读 footer 为 frame。
- 非法 extended header 抛异常；未知扩展 flag 可跳过。

### 3.5 修复 ID3 genre 解析

- 新增 `NormalizeId3Genre(std::string_view value)`。
- 支持 `(13)`、`13`、`(13)Pop`、普通文本。
- v2.4 多值 NUL 分隔时取第一个可用 genre。
- 数字映射复用完整 ID3 genre 表。

### 3.6 统一 ID3 字段覆盖策略

- 对同一优先级 tag 内采用“已有值不覆盖”，除非当前字段为空。
- ID3v2 优先于 ID3v1；ID3v1 只能补空字段。
- 对 year/track/disc 仅在目标值为 0 时回填。

### 3.7 阶段验证

- ID3v2.2 文本 + front cover PIC。
- ID3v2.3 extended header + APIC front cover。
- ID3v2.4 flags + data length indicator。
- TCON 数字 genre、日期只取年份、重复 frame 覆盖策略。

## 第 4 阶段：ID3 歌词修复

### 4.1 实现 ID3v2.2 歌词帧

- 实现 `ReadID3v22LyricsFrames()`。
- 支持 v2.2 `ULT` 纯文本歌词。
- 如实现同步歌词，支持 `SLT`；否则明确跳过，不误读。

### 4.2 同步 v2.3/v2.4 frame flag 处理

- 歌词路径复用或对齐 metadata 的 frame size、unsupported flags、v2.4 data length indicator、unsynchronisation 处理。
- 不允许 unsupported frame 只跳 10 字节。

### 4.3 修复 `SYLT` timestamp format

- `timestampFormat == 2` 按毫秒转换。
- `timestampFormat == 1` 是 MPEG frames；当前模型是时间微秒，若无法换算则跳过该 SYLT 或保守不写错误时间。
- 其他 timestamp format 跳过。

### 4.4 明确歌词选择策略

- `USLT`：读取纯文本歌词。
- `SYLT`：只在 timestamp 可正确解释时读取同步歌词。
- `TXXX`：仅 description 为 lyrics、unsyncedlyrics、lyric、syncedlyrics、sylt 时读取。
- 多语言重复歌词先采用第一个可用值，不覆盖已有歌词。

### 4.5 阶段验证

- ID3v2.2 `ULT` 样本。
- ID3v2.3 / v2.4 `USLT` 样本。
- `SYLT` milliseconds 样本。
- `SYLT` MPEG frames 样本应不产生错误时间轴。
- 普通 `TXXX` 不应进入歌词。

## 第 5 阶段：ID3v1 修复

### 5.1 使用嗅探转码读取字段

- ID3v1 title / artist / album / year 字段从原始 30/4 字节切片进入统一嗅探转 UTF-8 流程。
- Latin-1 作为常见回退，但不再宣称能正确识别所有本地编码。
- 已经合法 UTF-8 的 ID3v1 字节应保留为 UTF-8。

### 5.2 完整 genre 表

- 扩展 `Id3v1Genres` 至常见 ID3v1 + Winamp 扩展表。
- index 越界保持空值。
- genre 只在 `metadata.genre` 为空时回填。

### 5.3 修正 track number 语义

- 仅当 `buffer[125] == '\0'` 且 `buffer[126] != '\0'` 时写入 track number。
- ID3v1 仍只补空字段，不覆盖 ID3v2。

### 5.4 阶段验证

- 仅 ID3v1 样本。
- Latin-1 非 ASCII 样本。
- UTF-8 字节样本。
- genre index 80+ 样本。
- track byte 为 0 的样本。

## 第 6 阶段：Vorbis / FLAC / Ogg 修复

### 6.1 修正 Vorbis 总数字段误写

- `tracktotal` / `totaltracks` 不得写入 `trackNumber`。
- `disctotal` / `totaldiscs` 不得写入 `discNumber`。
- 当前公共模型没有总数字段，遇到这些 key 直接忽略。

### 6.2 Vorbis Comment 文本解码

- 规范路径按 UTF-8 解码。
- 非法 UTF-8 时进入统一嗅探回退。
- 解码失败则忽略该 entry，不清空已有字段。

### 6.3 复用 Ogg comment packet 组装到歌词路径

- 抽出 Ogg Vorbis comment packet 读取函数，metadata 和 lyrics 共用。
- 正确识别 `0x03 + "vorbis"` comment packet。
- 支持跨页 packet。
- 避免误读其他 logical bitstream；至少校验 Vorbis identification/comment header 顺序。

### 6.4 FLAC block 扫描收敛

- 可选：将 FLAC Vorbis Comment 和 PICTURE 扫描合并为一次 metadata block 遍历。
- 如果保持两次扫描，必须保证损坏块行为一致。
- front cover 只接受 picture type `3`，并走 PNG 导出。

### 6.5 阶段验证

- FLAC Vorbis Comment 文本字段。
- FLAC front cover PNG/JPEG 源图导出为 PNG。
- Ogg 跨页 comment metadata。
- Ogg 跨页 lyrics。
- totaltracks / totaldiscs 不污染当前编号。

## 第 7 阶段：MP4 / M4A 修复

### 7.1 严格限制 metadata atom 路径

- `ReadMP4AtomTree()` 只在 `moov/udta/meta/ilst` 路径下识别 metadata item。
- `meta` box 必须按 full box 处理；如果遇到非 full box 变体，需要显式分支或保守跳过。
- 不在任意层级解析同名 atom。

### 7.2 增加 MP4 年份读取

- 支持 `©day`，必要时支持 `date`。
- 文本解码后调用 `ParseYearOnly()`。
- 只写入年份，不保留月日。

### 7.3 修正 `trkn` / `disk`

- 校验 data type 为合理的 integer/implicit 类型。
- payload 长度至少满足读取当前编号所需结构，建议按 iTunes 常见布局检查 6 或 8 字节。
- 只读取当前 track/disc 编号，不读取总数。
- 编号为 0 时不写入。

### 7.4 修复 MP4 文本和歌词编码

- text data type `1` 按 UTF-8。
- data type `0` 作为 implicit/text 时也进入统一嗅探解码。
- 如果支持 UTF-16 类型，需要明确 data type 并走 UTF-16 解码；否则保守跳过。
- `©lyr` 使用 0xA9 字节数组定义，不再依赖源码字符编码。

### 7.5 拆出 MP4 lyrics atom 扫描

- 删除 `ReadMP4Lyrics()` 内的大段递归 lambda。
- 新增小函数，例如：
- `ReadMP4LyricsAtomTree(...)`
- `ReadMP4LyricsItem(...)` 保持只处理 item/data。
- 保持 `ReadLyrics()` 只做分发。

### 7.6 修复 MP4 `covr`

- `covr` 只接受 JPEG/PNG 源图签名。
- data type 和签名冲突时以签名为准或保守跳过。
- 输出统一走 `WriteCoverAsPng()`。

### 7.7 阶段验证

- M4A 文本字段样本。
- `©day=2024-05-14` 输出 `year=2024`。
- `trkn` / `disk` 当前编号正确，总数不污染。
- `©lyr` 歌词可读。
- `covr` JPEG/PNG 源图均导出 `.png`。

## 第 8 阶段：清理 FFmpeg metadata 依赖和死代码

### 8.1 删除 FFmpeg metadata 读取入口

- 删除 `GetDictionaryValue()`，或确认没有任何 tag 字段通过它读取。
- 全仓搜索 `av_dict_get`，确保没有用于歌名、歌手、专辑、歌词、封面等 tag 字段。
- 保留 FFmpeg 音频流信息读取逻辑。

### 8.2 改进格式分发兜底

- `ReadMetadata()` / `ReadLyrics()` 当前可继续使用 FFmpeg container name 做首选分发。
- 增加文件签名兜底：ID3、fLaC、OggS、MP4 box。
- 分发判断仍不能读取 FFmpeg metadata。

### 8.3 修复边界工具

- `ParseUInt16()` 增加溢出检查，或对年份/编号改用专门解析函数。
- `ReadRange()` 检查 offset 是否可转换为 `std::streamoff`。
- atom size / block size 计算统一做溢出保护。

### 8.4 清理未使用或误导函数

- 删除固定 `.jpg` 的 `MakeTempCoverPath()`。
- 删除不再使用的旧封面写出路径。
- 注释说明封面统一输出 PNG。

### 8.5 阶段验证

- 全仓搜索确认没有 FFmpeg metadata 字段读取。
- 构建通过。
- 随机样本不发生行为回退。

## 第 9 阶段：整体回归验收

### 9.1 构建

- 运行 `cmake --build build`。
- 如果 `build/` 不存在，先运行 `cmake -S . -B build`。

### 9.2 样本矩阵

- ID3v1：Latin-1、UTF-8 字节、genre 80+、track 0。
- ID3v2.2：文本、genre、year、front cover PIC、ULT。
- ID3v2.3：extended header、APIC front cover、USLT、SYLT milliseconds。
- ID3v2.4：data length indicator、frame unsync、TDRC 日期、TCON 多值。
- FLAC：Vorbis Comment、front cover PICTURE、totaltracks/totaldiscs。
- Ogg Vorbis：跨页 metadata、跨页 lyrics。
- MP4/M4A：文本字段、`©day`、`trkn`、`disk`、`©lyr`、`covr`。

### 9.3 验收标准

- 所有 tag 字段均来自文件原始字节解析，不来自 FFmpeg metadata。
- 原始字符串先完成编码嗅探，再转为 UTF-8。
- 年份只输出年份数字。
- 只导出当前歌曲 front cover。
- 封面导出路径后缀为 `.png`，文件内容为 PNG。
- 普通 `TXXX` 不会误识别为歌词。
- totaltracks / totaldiscs 不污染当前 track / disc 编号。
- 构建通过，代表样本输出符合预期。
