# TagReader.cpp 重新审查发现的问题

本文件基于当前 `src/TagReader.cpp` 的完整重新扫描结果覆盖生成。审查重点是各 tag 协议解析正确性，以及原始字符串编码嗅探和转 UTF-8 能力。

## 总体问题

1. `NormalizeText()` 并没有做编码嗅探或转码，只验证输入是否已是合法 UTF-8。
   - 位置：`NormalizeText()`、`NormalizeMetadata()`、`NormalizeLyrics()`。
   - 影响：来自 MP4/Vorbis/歌词等路径的非 UTF-8 文本会被清空；它无法识别 GBK、Shift-JIS、Big5、Windows-125x 等常见本地编码。
   - 现状：只有 ID3 encoding byte 明确标记的文本和 ID3v1 Latin-1 字段会在读取阶段转成 UTF-8。

2. `ReadUtf16Text()` 对 ID3 encoding 1 的 BOM 处理错误。
   - 位置：`ReadId3ByteString()` 对 encoding `1` 固定调用 `ReadUtf16Text(..., false)`。
   - 影响：ID3 encoding `1` 代表 UTF-16 with BOM，应按 BOM 决定大小端；当前即使 payload 是 FE FF big-endian BOM，也会按 little-endian 解码，导致中文、日文等非 ASCII 文本错码。

3. `TrimText()` 不能正确去除尾部 NUL 字节。
   - 位置：`TrimText()`。
   - 原因：字符串字面量 `" \t\r\n\0"` 在传给 `find_first_not_of` / `find_last_not_of` 时按 C 字符串处理，实际字符集在第一个 NUL 处结束，NUL 没有参与 trim。
   - 影响：Vorbis Comment、MP4 文本、Normalize 前后的通用路径可能保留尾部 `\0`。

4. 多处协议文本路径默认把原始字节当 UTF-8，却没有格式级编码策略。
   - 位置：`ReadVorbisCommentEntry()`、`ReadVorbisLyricsEntry()`、`ReadMP4DataAtom()`、`ReadMP4LyricsItem()`。
   - 影响：虽然 Vorbis Comment 和 MP4 iTunes 文本通常是 UTF-8，但遇到历史文件或非规范写入时，当前只会清空或保留错误文本，没有嗅探或回退。

## ID3v2 Metadata 问题

1. ID3v2.4 tag-level footer flag 没有处理。
   - 位置：`ReadID3v2Metadata()`。
   - 影响：如果 v2.4 header flags 带 footer present，tag 后还有 10 字节 footer；当前只按 tag body size 读取，不会跳过 footer，也未明确验证，可能影响后续尾部/相邻结构解析策略。

2. ID3v2.4 extended header size 下限判断可能过严或不完整。
   - 位置：`ReadID3v2Metadata()`。
   - 影响：v2.4 extended header 结构包含 size、flag bytes、flags data。当前只判断 `extSize < 6`，但没有解析 number of flag bytes，也没有根据 flag bytes 跳过/update 附加字段；复杂 extended header 可能被粗略跳过或误判。

3. ID3v2.3/v2.4 frame flag 处理仍不完整。
   - 位置：`ReadID3v23Or24Frames()`。
   - 影响：压缩、加密、分组、data length indicator、unsynchronisation 等 flag 没有完整按版本解释；当前仅跳过部分 unsupported flags，其他组合可能被误读或错误跳过。

4. 跳过不支持 frame 时游标推进错误。
   - 位置：`ReadID3v23Or24Frames()`。
   - 现状：遇到非法 frame id、unsupported flags、非法 syncsafe size 时只 `cursor += 10`。
   - 影响：frame header 后仍有 payload，跳 10 字节会落入 payload 中间继续扫描，可能导致后续 frame 误判、漏读或抛错。应在能读出 frame size 后跳过完整 frame；不能安全读 size 时应停止扫描。

5. `ReadID3v22PictureFrame()` 的 PIC 解析结构错误。
   - 位置：`ReadID3v22PictureFrame()`。
   - 原因：ID3v2.2 `PIC` 结构是 `encoding + image format(3) + picture type + description + terminator + image data`。当前先从 `payload` 开始扫描一个 NUL，然后再解析 description，相当于把 description 拆成两段，导致正常 PIC 描述为空时会把 image data 前几个字节误当 description 或直接返回。
   - 影响：ID3v2.2 封面可能无法导出，或导出的图片起点错误。

6. APIC/PIC 图片优先级不一致。
   - 位置：`ReadID3v2PictureFrame()`、`ReadID3v2ApicPayload()`、`ReadID3v22PictureFrame()`。
   - 影响：APIC 读取了 picture type 但丢弃，没有像 FLAC 一样保持 front cover 优先。多 APIC 时后读图片会覆盖先读图片，非 front cover 也可能覆盖 front cover。

7. ID3 genre 没有解析括号数字或 v2.4 多值格式。
   - 位置：`ReadID3v22Frame()`、`ReadID3v2Frame()`。
   - 影响：`TCON`/`TCO` 为 `(13)`、`13`、`(13)Pop` 或 v2.4 NUL 分隔多值时，当前直接原样输出，不会映射到 ID3 genre 表，也不会拆出用户可读值。

8. ID3v2.4 年份只读 `TDRC`，没有覆盖常见相关帧。
   - 位置：`ReadID3v2Frame()`。
   - 影响：没有处理 `TDOR`、`TDRL`、`TDTG` 等相关日期帧；这不一定是 bug，但作为“年份”字段兼容性不足。

9. ID3v2 frame 文本覆盖策略不一致。
   - 位置：`ReadID3v22Frame()`、`ReadID3v2Frame()`。
   - 影响：ID3v2 会无条件覆盖字段，而 Vorbis 使用“已有值不覆盖”。同一 tag 内重复 frame 或多个别名时，结果取决于扫描顺序，缺少统一规则。

## ID3 Lyrics 问题

1. ID3v2.2 歌词完全未实现。
   - 位置：`ReadID3v22LyricsFrames()`。
   - 影响：v2.2 的 `ULT` / `SLT` 等歌词帧不会被读取。

2. ID3 歌词 frame flag 处理与 metadata 路径不一致且不完整。
   - 位置：`ReadID3v23Or24LyricsFrames()`。
   - 影响：v2.4 data length indicator 没有剥离；压缩、加密、分组等 flags 没有按版本完整处理；部分 unsupported frame 只跳 10 字节，可能错位。

3. ID3 歌词遇到非法 frame id 直接 break，metadata 路径则尝试 continue。
   - 位置：`ReadID3v23Or24LyricsFrames()`。
   - 影响：一个 padding 前的脏 frame id 会导致后续歌词帧全部漏读。

4. `SYLT` timestamp format 被忽略。
   - 位置：`ReadID3v23Or24LyricsFrames()`。
   - 影响：`timestampFormat == 1` 表示 MPEG frames，`2` 才是 milliseconds。当前全部按毫秒处理，frame-based SYLT 时间轴错误。

5. `USLT` / `SYLT` 只做最小字段跳过，没有验证语言码或内容类型。
   - 位置：`ReadID3v23Or24LyricsFrames()`。
   - 影响：多语言歌词、非歌词内容类型、重复歌词帧的选择策略不明确。

## ID3v1 问题

1. ID3v1 genre 表只有 80 项，不完整。
   - 位置：`Id3v1Genres`。
   - 影响：Winamp 扩展后的常见 genre index 大于 79 时会被忽略，例如 80+ 的常见流派无法映射。

2. ID3v1.1 track number 没有排除 0。
   - 位置：`ReadID3v1Metadata()`。
   - 影响：当 `buffer[125] == 0` 且 `buffer[126] == 0` 时当前仍会写入 0，虽然结果值不变，但语义上未区分“无 track”和“track 0”。

3. ID3v1 字段固定按 Latin-1 解码，无法处理实际使用本地编码写入的历史文件。
   - 位置：`ReadID3v1Metadata()`、`ReadLatin1Text()`。
   - 影响：GBK/Shift-JIS 等 ID3v1 文件会被错误转换成 Latin-1 UTF-8，`NormalizeText()` 仍会认为它是合法 UTF-8，从而保留乱码。

## Vorbis / FLAC / Ogg 问题

1. Vorbis Comment 总数字段被误写入主编号字段。
   - 位置：`ReadVorbisCommentEntry()`。
   - 现状：`tracktotal` / `totaltracks` 会在 `trackNumber == 0` 时写入 `trackNumber`，`disctotal` / `totaldiscs` 会写入 `discNumber`。
   - 影响：只有总曲数而没有当前曲号时，会把总数误当当前曲号；这是字段语义错误。

2. Ogg Vorbis metadata 路径能跨页组包，但 lyrics 路径不能。
   - 位置：`ReadOggVorbisComments()` 与 `ReadVorbisLyrics()` 的 Ogg 分支。
   - 影响：跨页 comment packet 中的歌词字段在 metadata 可读的情况下，lyrics 仍可能漏读。

3. Ogg lyrics 分支识别 Vorbis comment packet 的签名错误。
   - 位置：`ReadVorbisLyrics()` Ogg 分支。
   - 原因：Vorbis comment packet 应以 `0x03 + "vorbis"` 开头；当前比较 payload 前 7 字节是否等于 `"vorbis"`，长度和起点都不对。
   - 影响：Ogg Vorbis 歌词基本无法从 comment packet 读取。

4. Ogg metadata 解析没有校验 BOS/header packet 顺序和 stream serial。
   - 位置：`ReadOggVorbisComments()`。
   - 影响：包含多 logical bitstream 或非 Vorbis Ogg 内容时，可能误读其他流 packet。

5. FLAC metadata 和 picture 分两次从头扫描。
   - 位置：`ReadVorbisCommentMetadata()` 调用 `ReadVorbisCommentBlock()` 后又调用 `ReadFlacPictureBlock()`。
   - 影响：效率问题为主；如果某个损坏块在 comment 之后但 picture 之前，两次扫描的异常路径可能造成行为不一致。

6. FLAC PICTURE description 按字节跳过，没有按规范确认 UTF-8 合法性。
   - 位置：`ReadFlacPictureEntry()`。
   - 影响：当前不使用 description，所以影响较小；但对结构错误和字符串编码错误没有区分。

7. Vorbis Comment entry value 没有 UTF-8 验证前转码能力。
   - 位置：`ReadVorbisCommentEntry()`。
   - 影响：Vorbis 规范要求 UTF-8；当前最终 Normalize 会清空非法 UTF-8，但无法修复历史非 UTF-8 写入。

## MP4 / M4A 问题

1. MP4 metadata 没有读取年份字段。
   - 位置：`ReadMP4AtomTree()`、`ReadMP4DataAtom()`。
   - 影响：`©day` / `date` 等常见年份 atom 不会写入 `metadata.year`。

2. MP4 atom tree 会在任意层级识别 item atom。
   - 位置：`ReadMP4AtomTree()`。
   - 影响：没有严格限制 `moov/udta/meta/ilst` 路径，遇到同名 box 或非 metadata 区域时可能误解析。

3. MP4 `meta` box 处理默认跳过 4 字节 version/flags，但没有确认当前 `meta` 是否 full box。
   - 位置：`ReadMP4AtomTree()`。
   - 影响：QuickTime 风格或非标准布局可能被错位递归。

4. MP4 `trkn` / `disk` payload 长度判断过低，且没有校验 data type。
   - 位置：`ReadMP4DataAtom()`。
   - 影响：当前 `payloadSize >= 4` 就读取 payload[2..3]，但 iTunes `trkn` / `disk` 通常需要至少 6 或 8 字节，并应确认 data type 是整数/implicit 类型；短 payload 可能被误读。

5. MP4 text data type 支持不完整。
   - 位置：`ReadMP4DataAtom()`。
   - 影响：当前只接受 `0` 和 `1`。这符合当前收紧策略，但如果遇到 UTF-16 或其他合法类型，不会解析；是否支持需要明确策略。

6. MP4 cover `covr` 未检查 payload 是否确实是 JPEG/PNG。
   - 位置：`ReadMP4DataAtom()`。
   - 影响：只根据 data type 写扩展名，损坏或错误 data type 会生成错误图片文件。

7. `ReadMP4Lyrics()` 使用局部递归 lambda，违反“入口/分发函数不要塞大段 lambda”的既有设计约束。
   - 位置：`ReadMP4Lyrics()`。
   - 影响：结构维护问题；metadata 入口已拆分，但歌词 MP4 路径仍把 atom tree 扫描塞在函数内部。

8. MP4 lyrics 的 `©lyr` 比较依赖源码字符编码。
   - 位置：`ReadMP4Lyrics()`、`ReadMP4LyricsItem()`。
   - 影响：metadata 已为 `©nam` 等改用 0xA9 字节数组，lyrics 仍使用字符串字面量 `"©lyr"`，在不同源码/编译环境下存在不一致风险。

## 封面提取问题

1. `MakeTempCoverPath()` 仍存在但当前主要路径未使用。
   - 位置：`MakeTempCoverPath()`。
   - 影响：维护噪音；函数固定 `.jpg`，如果后续误用会重新引入扩展名错误。

2. APIC 不按 picture type 选择 front cover。
   - 位置：`ReadID3v2PictureFrame()`、`ReadID3v2ApicPayload()`。
   - 影响：多图片 ID3 文件中结果不稳定，后读图片覆盖前读图片。

3. 封面导出没有图片签名兜底校验。
   - 位置：ID3 APIC/PIC、FLAC PICTURE、MP4 covr 路径。
   - 影响：MIME 或 format 字段错误时会使用错误扩展名写出文件；部分路径能 sniff PNG/JPEG，APIC 和 MP4 不完整。

## 其他边界问题

1. `ParseUInt16()` 没有检查溢出。
   - 位置：`ParseUInt16()`。
   - 影响：大于 65535 的年份、track、disc 会截断为 `uint16_t`，产生错误值。

2. `ReadRange()` 的 `std::uintmax_t` offset 强转 `std::streamoff` 没有边界检查。
   - 位置：`ReadRange()`。
   - 影响：极大文件或异常 offset 下可能溢出并 seek 到错误位置。

3. 容器分发依赖 FFmpeg container name 字符串。
   - 位置：`ReadMetadata()`、`ReadLyrics()`。
   - 影响：虽然字段不从 FFmpeg metadata 读取，但格式分发仍依赖 FFmpeg probe 结果；扩展名或文件签名可作为补充兜底，否则部分容器名变体可能漏分发。

4. `GetDictionaryValue()` 仍保留但当前不应再用于元数据读取。
   - 位置：`GetDictionaryValue()`。
   - 影响：死代码/维护风险；后续可能误用 FFmpeg metadata，违反当前设计约束。
