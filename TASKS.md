# TagReader 修复任务清单

本文件根据当前 `BUGS.md` 重新规划修复顺序，只跟踪已确认缺陷。执行时优先修协议级错读，再修字段映射，再补边界和收尾。

## 总原则

- 先保证“读对”，再保证“读全”，最后再做容错和清理。
- `ReadMetadata()` 只做分发，不在入口塞具体格式解析代码。
- 与音频流无关的 tag / 歌词必须直接读文件原始字节，不依赖 FFmpeg metadata。
- 每个阶段完成后都要做一次 `cmake --build build`，再用对应样本走 `./build/TagReaderTest <audio-file-path>` 复核。

## 第 1 阶段：ID3v2 协议级修复

### 1.1 拆分 ID3v2 的版本分发

- 保留 `ReadID3v2Metadata(ReadContext &, RawMetadata &)` 作为入口。
- 把 frame 遍历拆成两个内部函数：
  - `ReadID3v22Frames(ReadContext &, RawMetadata &, const std::vector<uint8_t> &)`
  - `ReadID3v23Or24Frames(ReadContext &, RawMetadata &, const std::vector<uint8_t> &, uint8_t versionMajor)`
- `ReadID3v2Metadata()` 只负责：读取 tag header、处理 tag 级 unsynchronization、处理 extended header、按版本分发到对应 frame 解析函数。
- `ReadID3v2Metadata()` 不再直接包含 frame 解析循环。

### 1.2 修正 ID3v2.2 frame 解析

- 新增 `ReadID3v22Frame(...)`，专门处理 3 字节 frame ID。
- 按 v2.2 规则解析：
  - 3 字节 frame ID
  - 3 字节 big-endian frame size
  - 不使用 10 字节 header
  - 不复用 v2.3/v2.4 的 frame flags 逻辑
- 为 v2.2 增加最小合法 frame ID 校验。
- `TT2 / TP1 / TAL / TP2 / TCM / TCO / TYE / TRK / TPA` 映射到现有元数据字段。
- `PIC` 单独走 v2.2 封面解析。

### 1.3 修正 ID3v2.3 extended header

- 核对 v2.3 extended header 的 size 定义。
- 修正 `cursor` 推进，确保跳过 `size` 字段本身后再进入 frame 区域。
- 只在 extended header 长度合法时继续扫描 frame。

### 1.4 修正 ID3v2.4 frame flags

- 把 v2.4 frame flags 与 v2.3 分开处理。
- 不再复用 `0x000C` 作为 v2.4 的压缩/加密判断。
- 重新核对单帧 unsynchronization、data length indicator 等位的语义。
- 对不支持的 frame flags，优先跳过单帧，不要直接污染整段 tag 的读取结果。

### 1.5 修正 APIC 游标

- 调整 `ReadID3v2PictureFrame()` 的游标推进顺序。
- 严格按以下顺序读取：
  - text encoding
  - MIME type
  - MIME terminator
  - picture type
  - description
  - description terminator
  - image data
- 删除多余的游标前移。
- 确认 description 为空时仍能正确落到 image data 起点。

### 1.6 调整单帧失败策略

- 遇到压缩/加密等不支持 frame 时，不再 `throw` 终止整个 ID3v2 读取。
- 改为跳过该 frame 并继续扫描后续 frame。
- 保留 `throw` 的情况只限于：
  - tag header 非法
  - tag size 非法
  - 扩展头尺寸非法
  - frame size 越界
  - 已经截断到无法安全继续扫描

### 1.7 验证样本

- 至少准备一份真实 ID3v2.2 样本，验证基本文本帧和 `PIC` 封面。
- 至少准备一份含 v2.3 extended header 的样本，验证不会从扩展头中间误读 frame。
- 至少准备一份含 v2.4 帧标志的样本，验证不会把合法 frame 误判为压缩/加密。
- 至少准备一份含不支持 frame 的样本，确认其他字段仍能正常读取。

## 第 2 阶段：ID3v2 歌词修复

### 2.1 同步歌词路径的版本分发

- 检查 `ReadID3Lyrics(ReadContext &, RawLyrics &)`。
- 让歌词路径和 metadata 路径共享同一套 ID3v2 版本判断。
- 不再允许 v2.2 走 10 字节 frame header 逻辑。

### 2.2 修正 SYLT 帧偏移

- 核对 `SYLT` payload 布局：
  - encoding
  - language(3)
  - timestamp format
  - content type
  - descriptor
  - lyrics data
- 修正当前把 timestamp format / content type 读错位的问题。
- 确保 descriptor 位置正确，歌词文本和时间戳不会错位。

### 2.3 收紧 TXXX 歌词识别

- 不要把所有 `TXXX` 都当作歌词。
- 只在 description 明确指向歌词相关字段时，才把内容纳入歌词结果。
- 其他 `TXXX` 保持忽略。

### 2.4 验证样本

- 至少准备一份含 `USLT` 的样本，确认纯文本歌词可读。
- 至少准备一份含 `SYLT` 的样本，确认时间戳和文本对齐。
- 至少准备一份含普通 `TXXX` 的样本，确认不会被误识别成歌词。

## 第 3 阶段：ID3v1 修复

### 3.1 修正 Latin-1 到 UTF-8

- 把 `ReadID3v1Metadata()` 的字段读取改为复用 `ReadLatin1Text()`。
- 不再直接用 `std::string(buffer.data() + offset, size)` 保存原始字节。
- 确保常见 Latin-1 字符不会在 `NormalizeMetadata()` 阶段被清空。

### 3.2 补齐 year

- 读取 ID3v1 年份字段 `93-96`。
- 先提取文本，再做 `ParseUInt16()`。
- 仅在 `metadata.year == 0` 时回填，避免覆盖更高优先级来源。

### 3.3 补齐 genre 映射

- 在匿名命名空间中加入最小可用 ID3v1 genre 表。
- 从 `buffer[127]` 读取 genre index。
- index 合法则写入对应字符串，越界则保持空值。

### 3.4 删除 comment -> composer

- 从 `ReadID3v1Metadata()` 中移除 comment 写入 composer 的逻辑。
- 不要把 ID3v1 的 comment 映射到不存在的 composer 字段。

### 3.5 复核 track number

- 保留 v1.1 的 track number 逻辑。
- 确认 `buffer[125] == '\0'` 时才读取 `buffer[126]`。

### 3.6 验证样本

- 至少准备一份仅依赖 ID3v1 的 MP3 样本。
- 至少准备一份包含非 ASCII Latin-1 字符的 ID3v1 样本。
- 验证 `title / artist / album / year / genre / trackNumber`。
- 验证 `composer` 不再被 comment 污染。

## 第 4 阶段：FLAC / Ogg 修复

### 4.1 统一 FLAC 块失败策略

- 复核 `ReadVorbisCommentBlock()` 和 `ReadFlacPictureBlock()` 的边界行为。
- 文件签名错误、块大小越界、已声明块无法完整读出时抛异常。
- 非关键字段无法识别时跳过并继续。

### 4.2 补充 Vorbis Comment 别名

- 扩展 `ReadVorbisCommentEntry()` 的别名覆盖。
- 至少检查：`tracktotal`、`totaltracks`、`disctotal`、`totaldiscs`。
- 不新增公共字段，只保证主字段的回填策略一致。

### 4.3 统一 Vorbis Comment 覆盖策略

- 明确“后写覆盖先写”还是“只在空值时回填”。
- 将策略统一到 `title / artist / albumArtist / composer / genre / year / trackNumber / discNumber`。

### 4.4 修复 Ogg Vorbis 跨页 packet

- 重新实现 `ReadOggVorbisComments()` 的 packet 组装。
- 不要只解析单个 Ogg page 的 payload。
- 能跨页的 comment packet 要完整拼接后再解析。

### 4.5 复核 FLAC picture 优先级

- 保持 `pictureType == 3` 的 front cover 优先级。
- 确认已有封面时，非 front cover 不覆盖。

### 4.6 验证样本

- 至少准备标准 FLAC + Vorbis Comment + PICTURE 样本。
- 至少准备一份带别名字段的 Vorbis 样本。
- 至少准备一份跨页 Ogg Vorbis 样本。

## 第 5 阶段：MP4 修复

### 5.1 修正 `dataType` 处理

- 检查 `ReadMP4DataAtom()` 对 `dataType` 的分支。
- 不要把所有 payload 都按 UTF-8 文本处理。
- 对非文本类型保持保守处理，避免错误写入元数据。

### 5.2 复核 MP4 递归扫描

- 检查 `ReadMP4AtomTree()` / `ReadMP4ItemAtom()` 的边界判断。
- 保证 atom size、ext size、child size 越界时不会错读后续 atom。
- 确认 `moov/udta/meta/ilst` 递归路径不会漏扫有效文本 atom。

### 5.3 验证样本

- 至少准备一份 M4A / MP4 样本。
- 验证 `title / artist / album / albumArtist / composer / genre / trackNumber / discNumber / coverPath`。

## 第 6 阶段：封面与收尾

### 6.1 处理空的 `ExtractCoverToTempFile()`

- 取消 `ReadMetadata()` 对空实现的依赖，或者明确删除该调用点。
- 不新增新的通用封面兜底逻辑。
- 保持格式专有封面路径各自落盘。

### 6.2 修正封面文件命名冲突

- 重新评估 `MakeCoverPathForAudioFile()` 的命名策略。
- 避免同名音频、重复读取、并发读取时覆盖封面文件。
- 若不改命名策略，至少在验证阶段确认风险可接受。

### 6.3 复核 `NormalizeText()`

- 检查哪些字段在进入 `MusicTag` 前会被清空。
- 如果某些格式仍需额外编码转换，补到格式解析阶段，而不是只依赖 `NormalizeText()`。

## 第 7 阶段：整体回归

### 7.1 构建

- 运行 `cmake --build build`。

### 7.2 手动输出核对

- 运行 `./build/TagReaderTest <audio-file-path>`。
- 重点核对：
  - `year`
  - `genre`
  - `trackNumber`
  - `discNumber`
  - `coverPath`
  - `lyricsCount`

### 7.3 完成标准

- `BUGS.md` 中列出的确认问题都已有对应修复。
- ID3v2 能按版本正确分发，且 v2.2 / v2.3 / v2.4 都按各自协议读取。
- ID3v1 不再丢失 `year` / `genre`，也不再把 `comment` 错写到 `composer`。
- 歌词路径不会把普通 `TXXX` 当作歌词。
- FLAC / Ogg / MP4 的边界与覆盖策略一致且可回归验证。
