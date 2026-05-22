## 已确认缺陷

### 1. 多音轨文件会选错音频流
- 位置: `src/TagReader.cpp:601-614`, `src/TagReader.cpp:1635-1697`
- 问题: `DetectStream()` 只取第一个 `AVMEDIA_TYPE_AUDIO` 流作为主音频流，没有参考 FFmpeg 的 default stream / disposition / stream order。
- 影响: 多音轨文件、带评论音轨或导演音轨的文件，基础媒体信息可能来自错误流，导致时长、码率、采样率、声道数和位深读错。

### 2. ID3v2.2 的同步歌词被直接丢弃
- 位置: `src/TagReader.cpp:2984-3026`
- 问题: `ReadID3v22LyricsFrames()` 只处理 `ULT`，`SLT` 明确写成“暂不支持”并直接跳过。
- 影响: 这类文件即使其他标签正常，时间轴歌词也会永久丢失。

### 3. MP4 歌词读取无法处理 64 位 atom 尺寸
- 位置: `src/TagReader.cpp:3645-3700`
- 问题: `ReadMP4LyricsItem()` 遇到 `size == 1` 的扩展尺寸 atom 时，没有像元数据路径那样读取 64 位 size，而是直接按 `size < 8` 提前返回。
- 影响: 任何使用 64 位 atom 尺寸的 MP4/M4A 歌词项都会被漏读。

### 4. MP4 歌词只认 `©lyr`，忽略常见 freeform 歌词 atom
- 位置: `src/TagReader.cpp:3295-3344`, `src/TagReader.cpp:3645-3699`
- 问题: MP4 歌词路径只扫描 `©lyr`，没有处理 `----:com.apple.iTunes:LYRICS` 这类常见自由格式歌词字段。
- 影响: 大量 Apple 系文件或第三方写入的歌词会读不到，表现为 `lyricsCount: 0`。

### 5. UTF-16 原始字节长度为奇数时会被静默截断
- 位置: `src/TagReader.cpp:1176-1257`, `src/TagReader.cpp:3517-3555`
- 问题: `TryReadUtf16Text()` 只按 `i + 1 < size` 步进，遇到奇数长度时会忽略末尾半个字节并返回成功，而不是判定为损坏输入。
- 影响: 损坏的 UTF-16 标签可能被错误接受为有效文本，造成字段污染或误判。

### 6. 封面导出只支持 PNG/JPEG，不能处理 BMP 等其他格式
- 位置: `src/TagReader.cpp:269-294`, `src/TagReader.cpp:434-462`, `src/TagReader.cpp:2801-2904`
- 问题: `DetectImageFormat()` 只识别 PNG/JPEG；`WriteCoverAsPng()` 遇到 BMP、WEBP、GIF、TIFF 等其他图像格式会直接返回空路径，不会尝试通过 FFmpeg/OpenCV 等图像后端解码后统一转 PNG。MP4 `covr` 还额外限制 `dataType == 13` 必须对应 JPEG、`dataType == 14` 必须对应 PNG。
- 影响: 音频文件内封面只要不是 PNG/JPEG，就无法导出；部分不规范但可解码的 MP4 封面也会被跳过，最终 `coverPath` 为空。

### 7. `bitDepth` 与 `format` 的媒体信息输出不稳定
- 位置: `src/TagReader.cpp:1829-1891`
- 问题: `ReadMediaInfo()` 之前只读取 `codecpar->bits_per_coded_sample` 作为位深，导致 FLAC 等格式明明存在原始位深信息却仍可能输出 `0`；同时直接使用 `iformat->name` 作为格式名，会让 `m4a` 这类文件输出 `mov,mp4,m4a,3gp,3g2,mj2` 这类候选列表，而不是单一格式名。
- 影响: 最终 `MusicTag` 的 `bitDepth` 与 `format` 字段不符合用户预期，尤其是 FLAC 位深丢失和 M4A 格式名过宽泛会直接影响展示层。
