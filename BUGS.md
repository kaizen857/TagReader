# TagReader Known Bugs

本文档只记录已从当前源码确认的问题，优先列出会导致字段错误、解析失败或行为与格式规范不一致的缺陷。

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

## ID3v2

### 5. ID3v2 应同时支持 v2.2 / v2.3 / v2.4，但当前只真正实现了 v2.3 / v2.4 风格解析

- 位置：`src/TagReader.cpp:928-933`, `src/TagReader.cpp:993-1045`, `src/TagReader.cpp:1064-1099`
- 现状：`ReadID3v2Metadata()` 接受 `versionMajor == 2`、`3`、`4`，但后续固定按 10 字节 frame header、4 字节 frame ID、`APIC`/`TIT2` 等 v2.3/v2.4 帧名解析，没有为 v2.2 切换到 6 字节 frame header、3 字节 frame ID、`PIC`/`TT2` 等对应规则。
- 影响：当前实现没有真正做到“同时支持解析 ID3v2.2、v2.3 和 v2.4”。真实的 ID3v2.2 标签会被按错误协议解释，可能触发 `invalid ID3v2 frame identifier`、提前中断或解析出错误字段。

### 6. `APIC` 图片帧解析多跳了一个字节

- 位置：`src/TagReader.cpp:1123-1147`
- 现状：`ReadID3v2PictureFrame()` 在跳过 MIME 终止符和 picture type 后，又额外执行了一次 `++cursor`，导致 description 的起始位置被错过。
- 影响：封面描述为空时很容易找不到正确 terminator，导致 APIC 图像数据起点判断错误，封面提取失败或不稳定。

### 7. 遇到压缩/加密帧时直接抛异常，单帧问题会导致整个 ID3v2 读取失败

- 位置：`src/TagReader.cpp:1007-1010`
- 现状：只要 frame flags 命中压缩/加密位，代码立即 `throw`。
- 影响：即使文件中其他常规文本帧完全可读，也会因为单个不支持的帧导致整个 `Read()` 失败。这个行为过于激进，和“尽量读取可用字段”的容错目标不一致。

## FLAC

### 8. FLAC 路径整体可工作，但仍是最小实现，边界与兼容性不足

- 位置：`src/TagReader.cpp:1178-1499`
- 现状：FLAC 使用 `fLaC` 签名检查、metadata block 扫描、Vorbis Comment 解析和 PICTURE 解析，主干逻辑基本成立；但当前只覆盖最常见字段和最常见块结构，没有更完整的字段别名、冲突合并策略和更一致的损坏块错误处理。
- 影响：对标准 FLAC 文件大概率可用，但遇到更复杂或更脏的数据时，行为还不够稳健，不能视为完整实现。
- 说明：这一项是实现完整性缺口，不是像 ID3v1 / ID3v2 那样的明确协议级错读。

## 其他相关问题

### 9. `ExtractCoverToTempFile()` 当前是空实现

- 位置：`src/TagReader.cpp:1712-1716`
- 现状：`ReadMetadata()` 总会调用 `ExtractCoverToTempFile()`，但该函数为空。
- 影响：当前不会破坏已在 ID3 APIC、FLAC PICTURE、MP4 `covr` 分支内完成的封面提取，但这个调用点本身没有任何效果，容易误导后续维护者以为这里还有统一兜底封面逻辑。
