# TagReader.cpp 重新审查发现的问题

本文件基于当前 `src/TagReader.cpp` 的重新扫描结果覆盖生成，重点检查各 tag 协议解析正确性，以及原始字符串编码嗅探和转 UTF-8 能力。

## 新增约束

1. 字符串嗅探的目标不是只支持 UTF-8 / Latin-1，而是支持多种常见本地编码到 UTF-8 的转换。
   - 约束：后续实现应优先覆盖常见本地编码到 UTF-8 的转换路径，而不是把非 UTF-8 字节简单清空。
   - 约束：对于已明确编码的标签格式，仍应优先使用格式规范中的编码信息；对于未明确编码的历史字段，再进入嗅探与回退流程。

2. 封面最终导出格式必须是 `.png`。
   - 约束：音频文件中存储的封面如果不是 PNG，需要进行图像格式转换后再写出 `.png` 文件，不能只改扩展名。
   - 约束：可考虑使用单头文件库 `stb`、`opencv` 或 `ffmpeg` 作为图像格式转换实现手段。
   - 约束：对于文件只提供非 `front cover` 的情况，必须直接将 `coverPath` 设为空字符串，不导出封面文件。

3. 标签解析的范围只覆盖 `Tag.hpp` 中 `MusicTag` 已存在的字段。
   - 约束：只需要提取 `MusicTag` 结构中已经定义的元数据字段。
   - 约束：非 `MusicTag` 范围内的元数据不需要补充读取，也不需要为其新增公共字段。

4. `ParseUInt16()` 对超出 `uint16_t` 的值直接返回 0，这个行为是正确的。
   - 约束：后续不要将此行为视为 bug，也不要为了保留异常数值而改变该函数语义。

## 编码与文本

1. 字符串嗅探仍只覆盖 UTF-8、UTF-16LE/BE、Latin-1。
   - 位置：`DetectTextEncoding()`、`DecodeTextToUtf8()`、`DecodeRawText()`。
   - 影响：GBK、Shift-JIS、Big5、Windows-125x 等历史本地编码无法识别，相关字段仍会被降级成 Latin-1 解释或被清空。

2. UTF-16 嗅探使用 NUL 分布启发式，容易把短二进制片段或异常字节序列误判为 UTF-16。
   - 位置：`DetectTextEncoding()`。
   - 影响：对损坏标签或偶发 NUL 的原始内容，可能进入错误的 UTF-16 解码路径，得到乱码或空字符串。

3. `ReadUtf16Text()` 对非法 surrogate pair 采取跳过式修复，而不是明确失败。
   - 位置：`ReadUtf16Text()`。
   - 影响：损坏 UTF-16 输入可能被部分吞掉，导致文本内容被悄悄截断，而不是暴露为解析失败。

## ID3v2

1. ID3v2.2 `SLT` 仍未实现。
   - 位置：`ReadID3v22LyricsFrames()`。
   - 影响：ID3v2.2 同步歌词会被直接跳过，无法读取。

2. `SYLT` 仅接受 timestamp format 2（milliseconds），MPEG frame-based 的时间轴会被忽略。
   - 位置：`ReadID3v23Or24LyricsFrames()`。
   - 影响：使用 `timestampFormat == 1` 的同步歌词无法导出。

3. ID3v2.4 extended header 仍按当前实现的最小结构解析，未覆盖更复杂/罕见的扩展字段组合。
   - 位置：`ReadID3v2Metadata()`。
   - 影响：某些带复杂 extended header 的 v2.4 标签可能被提前拒绝或跳过。

4. ID3 复合文本字段仍依赖扫描顺序与“已有值不覆盖”策略。
   - 位置：`ReadID3v22Frame()`、`ReadID3v2Frame()`。
   - 影响：同一 tag 中重复 frame 的优先级只取首次命中，无法表达更复杂的“主副值”优先关系。

## Vorbis / FLAC / Ogg

1. Ogg Vorbis 解析仍要求先看到 identification header，再接受 comment header。
   - 位置：`ReadOggVorbisCommentEntries()`。
   - 影响：非标准顺序或多 logical bitstream 中的有效 comment 可能被漏读。

2. Ogg lyric 读取和 metadata 读取共用 comment packet 组包逻辑，但仍只接受标准 Vorbis comment packet 结构。
   - 位置：`ReadOggVorbisCommentEntries()`、`ReadVorbisLyrics()`。
   - 影响：不是标准 `0x01 vorbis` / `0x03 vorbis` 结构的 Ogg 内容不会被解析。

3. FLAC 目前只遍历 comment block 和 picture block，其他 metadata block 即使存在也不会参与 tag 解析。
   - 位置：`ReadFlacMetadataBlocks()`。
   - 影响：除了 Vorbis Comment 和封面之外的 FLAC metadata 仍被忽略。

## MP4 / M4A

1. MP4 元数据只在 `moov/udta/meta/ilst` 路径下解析。
   - 位置：`ReadMP4AtomTree()`。
   - 影响：非标准 QuickTime 变体或不在该路径下的 metadata 会被漏读。

2. MP4 文本 data atom 仅接受 `dataType == 0/1`。
   - 位置：`ReadMP4DataAtom()`。
   - 影响：其他合法或历史写法的文本数据会被忽略。

3. MP4 `©lyr` 仅在 `dataType == 0/1` 时读取。
   - 位置：`ReadMP4LyricsItem()`。
   - 影响：非标准歌词 data type 会被跳过。

4. `covr` 仅接受 PNG/JPEG 签名并要求 `dataType == 13/14`。
   - 位置：`ReadMP4DataAtom()`。
   - 影响：其他格式封面或错误 data type 的封面会被丢弃。

## 其他边界

1. `ReadRange()` 依赖 `std::streamoff` / `std::streamsize` 可表示目标 offset 和 size；超大文件仍会返回空结果。
   - 位置：`ReadRange()`。
   - 影响：极端大文件场景下可能无法读取对应区段。

2. `ParseUInt16()` 对超出 `uint16_t` 的值直接返回 0。
   - 位置：`ParseUInt16()`。
   - 影响：异常大的 track / disc / year 值不会报错，只会降为 0。

3. 封面写出只保留“当前歌曲封面”图像，其他图片类型一律忽略。
   - 位置：`ReadID3v2PictureFrame()`、`ReadID3v22PictureFrame()`、`ReadFlacPictureEntry()`、`ReadMP4DataAtom()`。
   - 影响：如果文件只提供非 front cover 图片，则 `coverPath` 为空。
