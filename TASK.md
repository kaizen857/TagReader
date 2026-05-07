# TagReader 修复任务拆分

本文件覆盖旧的实现阶段计划，改为只跟踪 `BUGS.md` 中已确认问题的修复任务。任务按“先修协议级错读，再修字段映射，再补边界和收尾”的顺序排列。

## 目标

- 修复 `ID3v1` 字段缺失、错误映射和编码问题。
- 让 `ID3v2` 真正同时支持 `v2.2`、`v2.3`、`v2.4`。
- 修复 `ID3v2` 封面帧解析和过于激进的单帧失败策略。
- 收紧 `FLAC` 路径的边界处理和字段兼容性。
- 清理空的封面兜底调用点，避免误导后续维护。

## 执行顺序

1. 先修 `ID3v2` 的协议级问题。
2. 再修 `ID3v1` 的字段与编码问题。
3. 然后补 `FLAC` 的一致性和兼容性。
4. 最后处理 `ExtractCoverToTempFile()` 的空实现与回归验证。

## 第一组：ID3v2 主体修复

### 1.1 拆分 `ReadID3v2Metadata()` 的版本分发

- 在 `src/TagReader.cpp` 内保留 ID3 tag header 的公共读取流程：签名检查、版本读取、flags、tag size、tag body 读取、tag 级 unsynchronization。
- 将 frame 遍历逻辑从 `ReadID3v2Metadata()` 主体中拆出，避免把 `v2.2` 与 `v2.3/v2.4` 混在一套固定 header 里。
- 新增内部辅助函数，名称可直接贴近现有风格：
- `ReadID3v22Frames(ReadContext &, RawMetadata &, const std::vector<uint8_t> &)`
- `ReadID3v23Or24Frames(ReadContext &, RawMetadata &, const std::vector<uint8_t> &, uint8_t versionMajor)`
- `ReadID3v2Metadata()` 只负责：读取 tag body、处理扩展头、根据版本把剩余数据分发给对应 frame 解析函数。

### 1.2 为 ID3v2.2 实现正确的 frame header 解析

- 在 `ReadID3v22Frames(...)` 中按 6 字节 frame header 解析：
- 3 字节 frame ID
- 3 字节 big-endian frame size
- 不要在 `v2.2` 路径里复用 `frameFlags`、10 字节 header 或 4 字节 frame ID 逻辑。
- 为 `v2.2` 增加最小合法 frame ID 校验函数，避免直接复用只适配 4 字节 ID 的 `IsLikelyId3FrameId()`。
- 新增 24-bit 大小读取如果现有 `ReadBE24()` 已可复用，则直接复用，不新增重复工具。

### 1.3 为 ID3v2.2 建立字段帧映射

- 新增 `ReadID3v22Frame(...)`，专门处理 3 字节 frame ID。
- 将下列 v2.2 帧映射到现有 `RawMetadata` 字段：
- `TT2` -> `title`
- `TP1` -> `artist`
- `TAL` -> `album`
- `TP2` -> `albumArtist`
- `TCM` -> `composer`
- `TCO` -> `genre`
- `TYE` -> `year`
- `TRK` -> `trackNumber`
- `TPA` -> `discNumber`
- `PIC` -> 封面提取
- 文本 payload 继续复用 `ReadId3TextFrame()`，不要重写一套编码入口。

### 1.4 保留并收紧 v2.3 / v2.4 现有解析路径

- 现有 `ReadID3v2Frame(...)` 继续保留用于 4 字节 frame ID 的版本。
- 将当前 frame 循环里与 `v2.3/v2.4` 无关的逻辑留在新拆出的 `ReadID3v23Or24Frames(...)` 中。
- 保持现有字段映射不变，除非需要顺带修明显 bug。

### 1.5 修复 `APIC` 图片帧的游标推进错误

- 检查 `ReadID3v2PictureFrame()` 中 MIME 终止符、picture type、description 的解析顺序。
- 删除多余的一次 `++cursor`，确保游标推进顺序严格符合 APIC 结构：
- text encoding
- mime type
- mime terminator
- picture type
- description
- description terminator
- image data
- 修完后确认 description 为空字符串时也能正确落到 image data 起点。

### 1.6 为 ID3v2.2 的 `PIC` 实现独立封面解析

- 新增 `ReadID3v22PictureFrame(...)`，不要把 `PIC` 强塞进 `APIC` 逻辑。
- `PIC` 的头部字段按 v2.2 规则处理：
- 1 字节 encoding
- 3 字节 image format，例如 `PNG` / `JPG`
- 1 字节 picture type
- description
- image data
- 图片落盘仍复用现有 `WriteBinaryFile()` 和 `MakeCoverPathForAudioFile()`。
- `PNG` / `JPG` / `JPEG` 到扩展名的映射应在该函数内部完成，保持最小实现。

### 1.7 调整单帧失败策略，避免整段 ID3v2 直接失败

- 对不支持的压缩/加密 frame，不再 `throw` 终止整个读取过程。
- 改为跳过该 frame，继续扫描后续 frame。
- 只有以下情况继续保留 `throw`：
- tag header 非法
- tag size 非法
- 扩展头尺寸非法
- frame size 越界
- 明确截断到无法继续安全扫描
- 目标是“坏一个 frame 不影响整段标签中其他可读字段”。

### 1.8 检查歌词路径里的 ID3 版本兼容

- `ReadID3Lyrics()` 当前也接受 `versionMajor == 2`，但 frame 结构同样是按 10 字节 header 处理。
- 如果本轮修复范围允许，至少把这个问题记为代码同步点：
- 要么让歌词路径和 metadata 路径复用同样的版本分发
- 要么明确先只修 metadata，再单独补歌词
- 如果不在本轮实现，至少不要让任务执行者遗漏这条同类问题。

## 第二组：ID3v1 修复

### 2.1 用 Latin-1 解码 ID3v1 文本字段

- 修改 `ReadID3v1Metadata()` 中的 `readField` 局部逻辑。
- 不再直接构造 `std::string(buffer.data() + offset, size)`。
- 改为复用现有 `ReadLatin1Text()`，把 ID3v1 原始字节显式转成 UTF-8 字符串。
- 修复完成后，`NormalizeMetadata()` 不应再把常见 Latin-1 文本清空。

### 2.2 补充 `year` 字段读取

- 在 `ReadID3v1Metadata()` 中增加对偏移 `93-96` 的读取。
- 使用与现有数值字段一致的路径：取文本 -> `TrimText`/等价逻辑 -> `ParseUInt16()`。
- 只在 `metadata.year == 0` 时回填，避免覆盖优先级更高的来源。

### 2.3 实现 `genre` index 到字符串的映射

- 在匿名命名空间中增加一个最小可用的 ID3v1 genre 表。
- `ReadID3v1Metadata()` 从 `buffer[127]` 读取 index。
- 如果 index 落在表范围内，写入对应 genre 字符串。
- 如果 index 超出表范围，保持空值，不要伪造数字字符串。

### 2.4 删除错误的 `comment -> composer` 映射

- 从 `ReadID3v1Metadata()` 中移除“comment 非空且 composer 为空时写入 composer”的逻辑。
- 当前公共模型没有 comment 字段，就不要把它塞进无关字段。

### 2.5 保持并复核 ID3v1.1 的 track number 逻辑

- 保留 `buffer[125] == '\0'` 时从 `buffer[126]` 读取 track number 的逻辑。
- 顺带确认 comment 区段的理解与 v1.1 结构一致，避免后续有人误把 comment 的 30 字节布局当成总是成立。

## 第三组：FLAC 路径补强

### 3.1 保持 FLAC 现有分层，不做结构性重写

- 继续保留：
- `ReadVorbisCommentMetadata()`
- `ReadVorbisCommentBlock()`
- `ReadVorbisCommentEntry()`
- `ReadFlacPictureBlock()`
- `ReadFlacPictureEntry()`
- 后续改动都在这些函数内部或附近补强，不把它们重新合并成大函数。

### 3.2 统一 FLAC 块损坏时的失败策略

- 逐个核对 `ReadVorbisCommentBlock()` 与 `ReadFlacPictureBlock()` 当前是 `return` 还是 `throw`。
- 统一原则：
- 文件签名错误、块大小越界、已声明块无法完整读出时，抛异常。
- 非关键字段无法识别时，跳过并继续。
- 保持“结构损坏失败，内容未知可跳过”的一致风格。

### 3.3 补充常见 Vorbis Comment 别名

- 在 `ReadVorbisCommentEntry()` 中补全最常见别名，但只补公共模型需要的字段。
- 至少检查并考虑这些 key：
- `tracktotal`
- `totaltracks`
- `disctotal`
- `totaldiscs`
- 如果 `MusicTag` 没有总数位，就只保证主值字段不被这些别名干扰，不新增公共字段。

### 3.4 统一 Vorbis Comment 的覆盖策略

- 检查当前各字段是“后写覆盖先写”还是“仅在空值时回填”。
- 选定一个一致策略并贯彻到 `title`、`artist`、`albumArtist`、`composer`、`genre`、`year`、`trackNumber`、`discNumber`。
- 优先建议：
- 对明显同义别名只在目标字段为空时回填
- 对主字段本身第一次命中后不要被弱别名覆盖

### 3.5 复核 FLAC 封面优先级逻辑

- 保留 `pictureType == 3` 的 front cover 优先级。
- 确认“已有封面但新图片不是 front cover 时不覆盖”的逻辑继续成立。
- 不要让后续兼容性修改破坏现有封面选择规则。

## 第四组：空封面兜底逻辑清理

### 4.1 处理 `ExtractCoverToTempFile()` 的空实现

- 当前 `ReadMetadata()` 总会调用 `ExtractCoverToTempFile()`，但该函数为空。
- 推荐最小修复方案：
- 删除 `ReadMetadata()` 中对 `ExtractCoverToTempFile()` 的调用
- 或者保留该函数但明确注释“当前无通用兜底封面实现”
- 更推荐删除调用点，减少误导。

### 4.2 避免与格式专有封面逻辑重复覆盖

- 无论采用删除调用还是实现兜底，都要确保不会覆盖：
- ID3 `APIC` / `PIC`
- FLAC picture block
- MP4 `covr`
- 当前默认方向是不新增新的通用封面提取通道，只清理空逻辑。

## 第五组：验证任务

### 5.1 基础构建验证

- 使用 `cmake --build build` 验证 `TagReaderCore` 和 `TagReaderTest` 能通过编译。
- 如果 `build/` 不存在，先运行 `cmake -S . -B build`。

### 5.2 ID3v1 样本验证

- 准备至少一份仅依赖 ID3v1 的 MP3 样本。
- 验证输出字段：
- `title`
- `artist`
- `album`
- `year`
- `genre`
- `trackNumber`
- `composer` 不应再被 comment 污染
- 至少准备一份包含 Latin-1 非 ASCII 字符的样本，确认文本不会被归一化清空。

### 5.3 ID3v2.2 样本验证

- 准备真实 `ID3v2.2` 文件。
- 验证：
- 基本文本帧可读
- `PIC` 封面可落盘
- 不会再因 10 字节 header 假设导致解析失败

### 5.4 ID3v2.3 / v2.4 样本验证

- 准备 `v2.3` 和 `v2.4` 文件各至少一份。
- 验证：
- `TIT2/TPE1/TALB/TCON/TDRC/TRCK/TPOS` 仍能正确读取
- `APIC` 修复后可稳定导出封面
- 含不支持 frame 的文件不会整段读取失败

### 5.5 FLAC 样本验证

- 准备标准 FLAC + Vorbis Comment + PICTURE 样本。
- 如有条件，再准备包含字段别名和脏数据边界的样本。
- 验证：
- 常见字段映射正确
- front cover 优先规则不变
- 损坏块时的行为符合新的失败策略

### 5.6 手动程序输出核对

- 使用 `./build/TagReaderTest <audio-file-path>` 逐个检查输出。
- 重点关注：
- `year`
- `genre`
- `trackNumber`
- `discNumber`
- `coverPath`
- `lyricsCount`

## 完成标准

- `BUGS.md` 中 `ID3v1`、`ID3v2`、`FLAC`、空封面兜底相关问题都已有对应代码任务。
- `ID3v2` 能按版本正确分发并同时支持 `v2.2`、`v2.3`、`v2.4` 的 metadata 解析。
- `ID3v1` 不再丢失 `year` / `genre`，也不再把 `comment` 错写到 `composer`。
- `ID3v1` 常见 Latin-1 文本不会在归一化阶段被错误清空。
- `APIC` / `PIC` / FLAC PICTURE / MP4 `covr` 的封面提取逻辑不会互相误覆盖。
- 构建成功，且手动样本验证结果与修复目标一致。
