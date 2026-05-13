# TagReader Known Bugs

本文档记录本轮按当前源码重新审查后确认存在的问题，优先列出会导致字段错误、解析失败或行为与格式规范不一致的缺陷。行号基于当前 `src/TagReader.cpp`。

## ID3v1

### 1. `genre` 字段没有被读取

- 位置：`src/TagReader.cpp:898-901`
- 现状：`ReadID3v1Metadata()` 在 `metadata.genre` 为空时只执行 `metadata.genre.clear()`，没有读取 `buffer[127]` 的 genre index，也没有做 genre 表映射。
- 影响：任何仅依赖 ID3v1 的文件都无法从该路径得到流派信息。

### 2. `year` 字段完全未解析

- 位置：`src/TagReader.cpp:855-912`
- 现状：函数读取了 title、artist、album、comment、trackNumber，但没有读取 ID3v1 固定布局中的 year 字段（偏移 `93-96`）。
- 影响：仅依赖 ID3v1 的文件会丢失年份信息。

### 3. 将 ID3v1 comment 错误映射到了 `composer`

- 位置：`src/TagReader.cpp:903-911`
- 现状：函数读取 comment 后，在 `metadata.composer` 为空时直接写入 `comment`。
- 影响：ID3v1 本身没有 composer 字段，这会制造错误元数据，把备注误报成作曲者。

### 4. ID3v1 文本没有按 Latin-1 转成 UTF-8，后续可能被清空

- 位置：`src/TagReader.cpp:882-884`, `src/TagReader.cpp:2215-2251`
- 现状：`ReadID3v1Metadata()` 用 `std::string(buffer.data() + offset, size)` 直接取原始字节；随后 `NormalizeMetadata()` 只接受已经是合法 UTF-8 的文本，不做 Latin-1 到 UTF-8 转换。
- 影响：包含非 ASCII 字符的 ID3v1 字段很容易在归一化阶段被清空。

## ID3v2 元数据

### 5. ID3v2 应同时支持 v2.2 / v2.3 / v2.4，但当前只真正实现了 v2.3 / v2.4 风格解析

- 位置：`src/TagReader.cpp:928-933`, `src/TagReader.cpp:993-1045`, `src/TagReader.cpp:1064-1099`
- 现状：`ReadID3v2Metadata()` 接受 `versionMajor == 2`、`3`、`4`，但后续固定按 10 字节 frame header、4 字节 frame ID、`APIC`/`TIT2` 等 v2.3/v2.4 帧名解析，没有为 v2.2 切换到 6 字节 frame header、3 字节 frame ID、`PIC`/`TT2` 等对应规则。
- 影响：真实 ID3v2.2 标签会被按错误协议解释，可能触发 `invalid ID3v2 frame identifier`、提前中断或解析出错误字段。

### 6. ID3v2.3 extended header 游标推进错误

- 位置：`src/TagReader.cpp:966-971`
- 现状：v2.3 extended header 的 size 字段表示“size 字段之后”的扩展头长度，当前代码读取 `extSize` 后直接 `cursor = extSize`，少跳过了 size 字段自身的 4 字节。
- 影响：包含 ID3v2.3 extended header 的文件会从扩展头中间开始按 frame header 解析，导致后续 frame 错读、抛错或字段丢失。

### 7. `APIC` 图片帧解析多跳了一个字节

- 位置：`src/TagReader.cpp:1123-1147`
- 现状：`ReadID3v2PictureFrame()` 在跳过 MIME 终止符和 picture type 后，又额外执行了一次 `++cursor`，导致 description 的起始位置被错过。
- 影响：封面描述为空时很容易找不到正确 terminator，导致 APIC 图像数据起点判断错误，封面提取失败或不稳定。

### 8. 遇到压缩/加密帧时直接抛异常，单帧问题会导致整个 ID3v2 读取失败

- 位置：`src/TagReader.cpp:1007-1010`
- 现状：只要 frame flags 命中压缩/加密位，代码立即 `throw`。
- 影响：即使文件中其他常规文本帧完全可读，也会因为单个不支持的帧导致整个 `Read()` 失败。这个行为过于激进，和“尽量读取可用字段”的容错目标不一致。

### 9. ID3v2.4 frame flags 仍按 v2.3 的压缩/加密位判断

- 位置：`src/TagReader.cpp:1007-1010`, `src/TagReader.cpp:1038-1040`
- 现状：v2.4 的 format flags 含义与 v2.3 不同，压缩/加密/unsynchronisation/data length indicator 位不应复用 v2.3 的 `0x000C` 判断；当前又用 `0x0002` 处理单帧 unsynchronization，容易与实际标志位不一致。
- 影响：v2.4 文件中合法 frame 可能被误判为不支持，或需要特殊处理的 frame 被当成普通 payload 解析。

## 歌词解析

### 10. `ReadID3Lyrics()` 接受 ID3v2.2，但仍按 v2.3 / v2.4 frame header 解析

- 位置：`src/TagReader.cpp:1731-1734`, `src/TagReader.cpp:1789-1829`
- 现状：歌词路径允许 `versionMajor == 2`，但 frame 循环固定按 10 字节 header、4 字节 frame ID 和 v2.3/v2.4 flags 解析。
- 影响：ID3v2.2 歌词帧会被错读；即使 metadata 路径修复 v2.2，如果歌词路径不同步拆分，仍会保留同类协议 bug。

### 11. `SYLT` 帧读取 timestamp format 的偏移错误

- 位置：`src/TagReader.cpp:1857-1876`
- 现状：SYLT payload 结构为 `encoding + language(3) + timestamp format + content type + descriptor + ...`；当前代码从 `p = 1 + 3` 直接把 timestamp format 字节当作 descriptor 起点，并用 `frameData[1 + 3 + 1]` 读取 timestamp format，实际读到的是 content type。
- 影响：同步歌词 descriptor 定位错误，后续歌词文本和时间戳解析会错位，SYLT 大概率无法正确解析。

### 12. `TXXX` 被无条件当作歌词来源

- 位置：`src/TagReader.cpp:1897-1904`
- 现状：`TXXX` 是通用自定义文本帧，当前代码未检查 description 是否为 lyrics 相关字段，就把第一个文本内容追加为纯文本歌词。
- 影响：普通自定义标签可能被误报成歌词，污染 `MusicTag::lyrics()`。

## FLAC / Vorbis / Ogg

### 13. FLAC 路径整体可工作，但边界与兼容性不足

- 位置：`src/TagReader.cpp:1178-1499`
- 现状：FLAC 使用 `fLaC` 签名检查、metadata block 扫描、Vorbis Comment 解析和 PICTURE 解析，主干逻辑基本成立；但当前只覆盖最常见字段和最常见块结构，没有更完整的字段别名、冲突合并策略和一致的损坏块错误处理。
- 影响：对标准 FLAC 文件大概率可用，但遇到更复杂或更脏的数据时，行为还不够稳健，不能视为完整实现。

### 14. FLAC metadata block 越界时静默返回，和已声明块读失败的 throw 策略不一致

- 位置：`src/TagReader.cpp:1290-1292`, `src/TagReader.cpp:1410-1412`
- 现状：metadata block 声明大小越过扫描范围时直接 `return`，但 Vorbis comment block 实际读取失败时会 `throw`。
- 影响：结构损坏的 FLAC 文件可能被静默当作“无更多标签”处理，隐藏真实文件损坏或解析错误。

### 15. Ogg Vorbis comment 解析没有处理跨页 packet

- 位置：`src/TagReader.cpp:1204-1266`
- 现状：`ReadOggVorbisComments()` 每次只检查单个 Ogg page 的 payload，遇到 `vorbis` comment packet 就在该页内完成解析，没有组装 lacing segments 跨页延续的 packet。
- 影响：comment 较大、跨 Ogg page 的文件会丢失字段或提前返回。

## MP4 / M4A

### 16. MP4 文本字段忽略 `dataType`，非文本 payload 也会按 UTF-8 文本读取

- 位置：`src/TagReader.cpp:1678-1682`
- 现状：表达式 `(dataType == 1 || dataType == 0) ? ReadUtf8Text(...) : ReadUtf8Text(...)` 两边完全相同，等于忽略 `dataType`。
- 影响：如果字段 payload 不是 UTF-8 文本，仍会被按文本解析，可能产生乱码或被后续 UTF-8 校验清空。

## 其他相关问题

### 17. `ExtractCoverToTempFile()` 当前是空实现且仍被调用

- 位置：`src/TagReader.cpp:849`, `src/TagReader.cpp:1712-1716`
- 现状：`ReadMetadata()` 总会调用 `ExtractCoverToTempFile()`，但该函数为空。
- 影响：当前不会破坏已在 ID3 APIC、FLAC PICTURE、MP4 `covr` 分支内完成的封面提取，但这个调用点本身没有任何效果，容易误导后续维护者以为这里还有统一兜底封面逻辑。

### 18. 封面临时文件名按音频 stem 固定生成，容易互相覆盖

- 位置：`src/TagReader.cpp:109-130`, `src/TagReader.cpp:1173-1175`, `src/TagReader.cpp:1496-1498`, `src/TagReader.cpp:1704-1708`
- 现状：格式分支封面统一用 `MakeCoverPathForAudioFile()` 生成 `<temp>/<audio-stem>.<ext>`，没有随机后缀或唯一目录。
- 影响：不同目录下同名音频、同一音频重复读取、并发读取时可能覆盖封面文件，`coverPath` 指向的内容可能不是本次解析得到的图片。

### 19. `NormalizeText()` 只做 UTF-8 校验，不做通用编码探测或转码

- 位置：`src/TagReader.cpp:2215-2229`
- 现状：字段不是合法 UTF-8 时直接清空；除 ID3 文本帧部分路径外，其他格式字段缺少显式非 UTF-8 转 UTF-8 策略。
- 影响：不符合 UTF-8 的标签文本会被丢弃，和 `DESIGN.md` 中“读取阶段完成编码统一”的目标仍有差距。
