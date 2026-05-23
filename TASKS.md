# Phase 0 - 安全基线与测试入口固定

目标：
先建立可重复验证入口，避免后续 P0 修复只靠手工观察。该阶段不改变 parser 行为，只增加测试样本生成约定、构建命令和失败判定，使每个后续阶段都能独立验收和回滚。

修改内容：
修改 `CMakeLists.txt`、新增 `test/security/` 下的样本生成脚本和最小验证程序；不修改 `TagReader::Read()`、parser 或公共数据模型。

步骤：
Step 1:

- 修改位置：`CMakeLists.txt`。
- 修改内容：增加可选测试目标 `TagReaderSecuritySmoke`，源文件为 `test/security/security_smoke.cpp`，链接 `TagReaderCore`。
- 示例代码结构：

```cmake
add_executable(TagReaderSecuritySmoke test/security/security_smoke.cpp)
target_link_libraries(TagReaderSecuritySmoke PRIVATE TagReaderCore)
```

- 风险：新增目标可能在老环境缺少测试文件时导致配置失败。
- 回归影响：不影响 `TagReaderCore` 和 `TagReaderTest` 的编译产物。
- 验证方式：运行 `cmake -S . -B build` 和 `cmake --build build`。
- 测试样本：无输入文件，仅验证构建系统。
- 验收标准：`TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke` 全部生成。
- 失败标准：CMake 配置失败、原有 `TagReaderTest` 不再生成、链接缺少 FFmpeg 符号。
- 回滚：删除新增 executable 配置，保留原有 CMake 内容。

Step 2:

- 修改位置：新增 `test/security/security_smoke.cpp`。
- 修改内容：实现命令行程序，参数为 `coverExportDir` 和一个或多个音频样本路径；对每个样本调用 `TagReader::Read(sample, coverExportDir)`，打印 `title`、`lyricsCount`、`coverPath`，捕获 C++ 异常并返回非零。
- 示例代码结构：

```cpp
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const std::filesystem::path coverDir = argv[1];
    for (int i = 2; i < argc; ++i) {
        MusicTag tag = TagReader::Read(argv[i], coverDir);
        std::cout << tag.coverPath() << '\n';
    }
}
```

- 风险：该步骤依赖 Phase 2 新 API，若先实现会编译失败。
- 回归影响：仅测试目标受影响。
- 验证方式：Phase 2 完成后构建并运行。
- 测试样本：Phase 2、Phase 3、Phase 4 生成的样本。
- 验收标准：合法样本返回 0，畸形样本不 crash；解析失败时只输出明确异常并返回非零。
- 失败标准：进程段错误、未捕获异常导致 abort、输出 cover 路径不在传入目录下。
- 回滚：删除 `security_smoke.cpp` 和 CMake 目标。

Step 3:

- 修改位置：新增 `test/security/generate_samples.py`。
- 修改内容：生成后续阶段使用的最小样本，全部写入 `/tmp/opencode/tagreader_security_samples`；脚本只构造当前仓库需要的 MP3/ID3、Ogg、MP4 atom、嵌入图片字节，不依赖上级目录。
- 示例代码结构：

```python
def write_id3v24_apic(path, image_bytes): ...
def write_deep_mp4(path, depth): ...
def write_ogg_continuation(path, pages): ...
def write_lrc_id3(path, text): ...
```

- 风险：脚本生成的容器可能无法通过 FFmpeg probe，导致 `OpenContext()` 先失败而无法覆盖目标 parser。
- 回归影响：无运行时影响。
- 验证方式：对每个样本运行 `./build/TagReaderTest <file>` 或 `./build/TagReaderSecuritySmoke <coverDir> <file>`。
- 测试样本：最小 MP3 底座使用 ffmpeg CLI 生成 0.2 秒静音音频，再拼接 ID3；MP4 样本用合法 `ftyp`/`moov`/`mdat` 结构；Ogg 样本保留 `OggS` page header。
- 验收标准：每个后续 Phase 明确列出的样本文件都能由脚本生成。
- 失败标准：样本文件为空、FFmpeg 完全不能 probe 所有样本、样本未命中目标 parser。
- 回滚：删除脚本，不影响库代码。

# Phase 1 - 统一资源预算与受限读取

目标：
优先封住 OOM 和大分配入口。后续 ID3、MP4、Ogg、封面修复都依赖统一大小上限，因此该阶段必须先做。

修改内容：
修改 `src/TagReader.cpp` 中 `ReadRange()` 及所有由文件长度驱动的调用点；新增文件内常量和 helper；必要时在 `include/TagReader.hpp` 增加私有 helper 声明。

步骤：
Step 1:

- 修改位置：`src/TagReader.cpp` 文件内匿名命名空间，靠近 `ReadRange()`。
- 修改内容：新增统一预算常量和错误类型，不进入公共 API。
- 示例代码结构：

```cpp
constexpr std::size_t kMaxGenericReadBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxId3TagBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxTextFieldBytes = 1 * 1024 * 1024;
constexpr std::size_t kMaxLyricsBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxCoverInputBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxOggPacketBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxMp4AtomPayloadBytes = 64 * 1024 * 1024;
```

- 风险：过低上限会跳过真实大封面或大歌词。
- 回归影响：超大标签从“尝试解析直到 OOM”变为“安全跳过或抛解析错误”。
- 验证方式：构建通过后用合法普通样本确认字段仍可读；用超大声明样本确认内存不上涨到危险值。
- 测试样本：ID3v2.4 header 声明 32 MiB tag、MP4 `data` atom 声明 80 MiB payload、FLAC `PICTURE` 声明 80 MiB 图片。
- 验收标准：合法小样本行为不变；超限样本不会 `std::bad_alloc`、不会 RSS 持续增长、不会 crash。
- 失败标准：普通 500 KiB 封面无法导出、普通歌词丢失、超限样本进程被 OOM killer 终止。
- 回滚：删除常量和调用点上限参数，恢复原 `ReadRange()` 调用。

Step 2:

- 修改位置：`src/TagReader.cpp:963` 附近的 `ReadRange()`。
- 修改内容：保留原函数签名作为默认受限读取，新增重载 `ReadRange(input, offset, size, maxSize)`；任何失败返回前调用 `input.clear()`，修复 stream 状态泄漏。
- 示例代码结构：

```cpp
std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size, std::size_t maxSize)
{
    if (size > maxSize) return {};
    input.clear();
    ...
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        input.clear();
        return {};
    }
    return buffer;
}

std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size)
{
    return ReadRange(input, offset, size, kMaxGenericReadBytes);
}
```

- 风险：现有调用点把空 vector 同时视为 EOF 和超限，需要后续调用点按语义处理。
- 回归影响：直接使用 `context.input` 的旧代码不再受上一次 partial read 的 failbit 影响。
- 验证方式：构建；用截断文件连续触发多个 parser，确认后续 parser 不因 failbit 残留异常。
- 测试样本：截断 ID3 header、截断 FLAC block、截断 MP4 atom header。
- 验收标准：所有截断样本返回安全错误或空字段；后续 parser 可继续尝试，不出现异常 stream 状态污染。
- 失败标准：合法文件读不到 header、截断文件导致无限循环或 crash。
- 回滚：恢复原 `ReadRange()` 实现。

Step 3:

- 修改位置：`ReadID3v2Metadata()`、`ReadID3Lyrics()`。
- 修改内容：在 `tagSize = ReadSyncSafe32(...)` 后先检查 `tagSize <= kMaxId3TagBytes`，再调用 `ReadRange(context.input, 10, tagSize, kMaxId3TagBytes)`；超限时 metadata 路径跳过 ID3v2，lyrics 路径跳过歌词，不进入 frame parser。
- 示例代码结构：

```cpp
if (tagSize > kMaxId3TagBytes) {
    return;
}
std::vector<uint8_t> tagBytes = ReadRange(context.input, 10, tagSize, kMaxId3TagBytes);
```

- 风险：真实超大 ID3 tag 会被跳过。
- 回归影响：合法 ID3v2.2/2.3/2.4 小标签不变。
- 验证方式：运行 ID3 title/APIC/USLT/SYLT 样本；运行 32 MiB 声明 tag 样本。
- 测试样本：一个 ID3v2.4 带 `TIT2` 的 MP3；一个 ID3v2.4 带 APIC 的 MP3；一个 syncsafe size 为 32 MiB 的畸形 MP3。
- 验收标准：小样本 title、cover、lyrics 仍读出；畸形样本 RSS 峰值低于 128 MiB 且不 crash。
- 失败标准：小样本 title 为空、畸形样本抛 `std::bad_alloc`。
- 回滚：移除 `tagSize` 上限检查。

Step 4:

- 修改位置：`ReadMP4ItemAtom()`、`ReadMP4LyricsItem()`、`ReadMP4FreeformLyricsItem()`、`ReadFlacMetadataBlocks()`、`ReadFlacPictureEntry()`、`ReadOggVorbisCommentEntries()`。
- 修改内容：按字段类型使用不同上限调用 `ReadRange()`；文本使用 `kMaxTextFieldBytes`，歌词使用 `kMaxLyricsBytes`，封面使用 `kMaxCoverInputBytes`，未知 MP4 payload 使用 `kMaxMp4AtomPayloadBytes`。
- 示例代码结构：

```cpp
const auto payloadSize = atom.atomEnd - atom.payloadOffset;
if (payloadSize > kMaxLyricsBytes) return;
auto data = ReadRange(context.input, atom.payloadOffset, static_cast<std::size_t>(payloadSize), kMaxLyricsBytes);
```

- 风险：字段类型判断错误会把封面按文本上限裁掉。
- 回归影响：各格式合法字段不变；超大字段被安全跳过。
- 验证方式：分别运行 MP4 普通 metadata、MP4 歌词、FLAC picture、Ogg Vorbis comment 样本。
- 测试样本：MP4 `©nam` 1 KiB、MP4 `©lyr` 10 KiB、MP4 `covr` 200 KiB、FLAC `PICTURE` 200 KiB、Ogg comment 10 KiB，以及对应超限变体。
- 验收标准：合法样本字段存在；超限样本不分配超限 buffer、不 crash。
- 失败标准：合法 cover 丢失、合法 Ogg comment 全部为空、超限 payload 仍进入 `ReadRange()` 大分配。
- 回滚：逐调用点恢复原无上限 `ReadRange()`。

# Phase 2 - Cover API 与 content-addressed PNG 导出

目标：
修复不安全临时文件和生命周期契约，同时满足既定设计决策：调用方提供封面导出目录、封面路径由 embedded image bytes hash 决定、PNG content-addressed cache、已存在则禁止重复转码、写入必须 atomic。

修改内容：
修改 `include/TagReader.hpp`、`src/TagReader.cpp`、`test/main.cpp`；替换 `MakeCoverPathForAudioFile()`、`WriteBinaryFile()`、`WriteCoverAsPng()` 调用链；新增 hash、分片路径、atomic write helper。

步骤：
Step 1:

- 修改位置：`include/TagReader.hpp` 的 public 区域和 `ReadContext`。
- 修改内容：新增公共入口 `static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);`；保留 `Read(filePath)` 作为单入口 facade 的便捷重载，但其内部必须调用新重载并使用空 coverExportDir 表示“不导出封面”。在 `ReadContext` 增加 `std::filesystem::path coverExportDir;`。
- 示例代码结构：

```cpp
static MusicTag Read(const std::filesystem::path &filePath);
static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
```

- 风险：如果直接移除旧签名，会破坏现有 `TagReaderTest` 和调用方。
- 回归影响：旧调用仍可读取 metadata/lyrics；只有传入目录时才导出封面。
- 验证方式：构建；分别调用旧 `Read(path)` 和新 `Read(path, dir)`。
- 测试样本：ID3v2.4 APIC MP3。
- 验收标准：旧调用不崩溃；新调用返回 cover path；旧调用的 cover path 为空或保持明确无导出语义。
- 失败标准：旧调用编译失败、新调用未进入 cover 导出目录。
- 回滚：恢复原单签名并移除 `coverExportDir` 字段。

Step 2:

- 修改位置：`src/TagReader.cpp` 的 `TagReader::Read()`、`OpenContext()`。
- 修改内容：将 `Read(filePath, coverExportDir)` 作为完整实现；`OpenContext()` 增加参数或在 `Read()` 后写入 `context.coverExportDir`；如果 `coverExportDir` 非空，要求它存在且是目录，或使用 `std::filesystem::create_directories()` 创建。
- 示例代码结构：

```cpp
MusicTag TagReader::Read(const std::filesystem::path &filePath)
{
    return Read(filePath, {});
}

MusicTag TagReader::Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    ValidatePath(filePath);
    ReadContext context = OpenContext(filePath);
    context.coverExportDir = coverExportDir;
    ValidateCoverExportDir(context.coverExportDir);
    ...
}
```

- 风险：目录创建失败会让原本可读的音频失败。
- 回归影响：只有显式请求导出封面时会新增目录错误。
- 验证方式：传入存在目录、不存在但可创建目录、普通文件路径、无权限目录四种情况。
- 测试样本：任意带 APIC 的 MP3。
- 验收标准：可创建目录成功；普通文件路径抛明确异常；旧 `Read(path)` 不写临时文件。
- 失败标准：传入普通文件时覆盖文件、无权限目录导致未捕获异常类型不清晰。
- 回滚：移除目录校验并恢复旧 `Read()` 实现。

Step 3:

- 修改位置：替换 `MakeCoverPathForAudioFile()`。
- 修改内容：删除基于音频文件名、时间戳、counter 的路径生成；新增 `HashEmbeddedImageBytes()`、`BuildCoverCachePath()`。hash 输入必须是原始 embedded image bytes，即 APIC/PICTURE/covr 中的图片 payload，不能是音频文件内容、歌曲名或歌手名。优先使用 FFmpeg 已依赖的 libavutil hash API 或实现文件内 SHA-256；输出 hex，路径为 `coverExportDir / hex.substr(0,2) / (hex.substr(2) + ".png")`。
- 示例代码结构：

```cpp
std::string HashEmbeddedImageBytes(const uint8_t *data, std::size_t size);

std::filesystem::path BuildCoverCachePath(const std::filesystem::path &dir, std::string_view hex)
{
    return dir / std::string(hex.substr(0, 2)) / (std::string(hex.substr(2)) + ".png");
}
```

- 风险：hash 实现错误会导致 cache miss 或碰撞概率异常。
- 回归影响：同一封面跨音频文件 dedup；不同音频同图返回同一路径。
- 验证方式：同一图片嵌入两个 MP3，比较 `coverPath` 完全一致；不同图片路径不同。
- 测试样本：两个带相同 APIC bytes 的 MP3；一个带不同 APIC bytes 的 MP3。
- 验收标准：相同 bytes 输出同一路径；路径包含两级分片；文件名不包含歌曲名、歌手名、音频 stem。
- 失败标准：路径随音频名变化、路径没有 `ab/cdef...png` 结构、hash 输入为转码后 PNG bytes。
- 回滚：恢复旧 `MakeCoverPathForAudioFile()`，但只用于临时排障，不可作为最终方案。

Step 4:

- 修改位置：替换 `WriteBinaryFile()`。
- 修改内容：新增 `AtomicWriteFileIfAbsent(finalPath, data, size)`。流程：创建父目录；如果 final 已存在则直接返回 true；在同一目录创建唯一临时文件；使用 `open(..., O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)`，可用时加 `O_NOFOLLOW`；写满、fsync 文件、close；用 `std::filesystem::rename(temp, final)` 原子发布；若 rename 因 final 已存在失败，则删除 temp 并视为成功。
- 示例代码结构：

```cpp
bool AtomicWriteFileIfAbsent(const std::filesystem::path &finalPath, const uint8_t *data, std::size_t size)
{
    if (std::filesystem::exists(finalPath)) return true;
    const auto tempPath = MakeSiblingTempPath(finalPath);
    int fd = ::open(tempPath.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    ...
    if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        if (errno == EEXIST || std::filesystem::exists(finalPath)) { unlink(tempPath.c_str()); return true; }
        ...
    }
    return true;
}
```

- 风险：跨平台 open flags 在非 POSIX 环境不可用；当前环境为 Linux，可先按 POSIX 实现。
- 回归影响：不再覆盖已有 cache；并发进程只会留下一个最终文件。
- 验证方式：并发启动 16 个 `TagReaderSecuritySmoke` 读取同一 APIC 文件到同一 coverExportDir。
- 测试样本：同一个带 JPEG APIC 的 MP3。
- 验收标准：最终只有一个 `.png`；无残留 `.tmp`；所有进程返回同一路径；没有截断 PNG。
- 失败标准：出现多个不同 PNG、出现 0 字节 final、普通 `ofstream` 仍被调用写 cover。
- 回滚：只回滚 atomic helper 的实现，不回滚 hash path 设计。

Step 5:

- 修改位置：`WriteCoverAsPng()` 及所有调用点：`ReadID3v2ApicPayload()`、`ReadID3v22PictureFrame()`、`ReadFlacPictureEntry()`、`ReadMP4DataAtom()`。
- 修改内容：函数签名改为 `WriteCoverAsPng(const ReadContext &context, const uint8_t *data, std::size_t size)`；如果 `context.coverExportDir.empty()` 直接返回空路径；先计算 hash 和 final path；若 final path 已存在，禁止调用 `ConvertImageToPng()`，直接返回 final path；否则转码为 PNG 后 atomic write。
- 示例代码结构：

```cpp
std::filesystem::path WriteCoverAsPng(const ReadContext &context, const uint8_t *data, std::size_t size)
{
    if (context.coverExportDir.empty() || size == 0 || size > kMaxCoverInputBytes) return {};
    const auto hex = HashEmbeddedImageBytes(data, size);
    const auto finalPath = BuildCoverCachePath(context.coverExportDir, hex);
    if (std::filesystem::exists(finalPath)) return finalPath;
    std::vector<uint8_t> png = DecodeAndEncodeCoverPng(data, size);
    AtomicWriteFileIfAbsent(finalPath, png.data(), png.size());
    return finalPath;
}
```

- 风险：现有调用点传 `context.filePath`，需要全部替换，否则编译失败。
- 回归影响：封面导出从临时目录副作用改为显式目录副作用。
- 验证方式：先读一次生成 PNG；第二次读同样文件时用日志计数或临时 instrumentation 确认 `ConvertImageToPng()` 未调用。
- 测试样本：JPEG APIC MP3、PNG APIC MP3、FLAC PICTURE、MP4 covr。
- 验收标准：四类 cover 都进入 content-addressed path；第二次读取同一 raw bytes 不重复转码；旧 `Read(path)` 不写任何 cover 文件。
- 失败标准：第二次读取仍生成新文件、hash path 基于 PNG 输出而非原始 bytes、旧 `Read(path)` 写 `/tmp`。
- 回滚：恢复旧 `WriteCoverAsPng()` 签名和调用点，但保留新 API 的编译兼容分支以便继续开发。

Step 6:

- 修改位置：`test/main.cpp`。
- 修改内容：支持可选第二参数 `coverExportDir`；如果提供则调用新 API，否则调用旧便捷 API。
- 示例代码结构：

```cpp
MusicTag tag = argc >= 3 ? TagReader::Read(argv[1], argv[2]) : TagReader::Read(argv[1]);
```

- 风险：命令行兼容性改变。
- 回归影响：原 `./build/TagReaderTest <audio>` 保持可用。
- 验证方式：分别用一个参数和两个参数运行。
- 测试样本：带 APIC 的 MP3。
- 验收标准：一个参数不导出封面或 coverPath 为空；两个参数导出 PNG。
- 失败标准：原单参数调用失败。
- 回滚：恢复原 `test/main.cpp`。

# Phase 3 - 图像解码与 PNG 输出资源限制

目标：
封住 embedded cover 造成的 CPU/内存 DoS 和伪 PNG 直写风险。该阶段依赖 Phase 1 的大小上限和 Phase 2 的 atomic cache path。

修改内容：
修改 `DetectImageFormat()`、`ConvertImageToPng()`、`WriteCoverAsPng()`；新增 `CoverDecodeLimits`、`DecodeAndEncodeCoverPng()`、PNG 输出大小限制。

步骤：
Step 1:

- 修改位置：`src/TagReader.cpp` 匿名命名空间。
- 修改内容：新增 `CoverDecodeLimits`，限制输入字节、宽、高、总像素、输出 PNG 字节。
- 示例代码结构：

```cpp
struct CoverDecodeLimits {
    std::size_t maxInputBytes{64 * 1024 * 1024};
    int maxWidth{8192};
    int maxHeight{8192};
    std::int64_t maxPixels{32LL * 1024 * 1024};
    std::size_t maxOutputBytes{64 * 1024 * 1024};
};
```

- 风险：极高分辨率真实封面被拒绝。
- 回归影响：普通专辑封面不受影响。
- 验证方式：运行 600x600、3000x3000、10000x10000 声明尺寸样本。
- 测试样本：正常 JPEG APIC；伪造超大 BMP APIC；截断 PNG APIC。
- 验收标准：正常封面导出；超限封面 coverPath 为空但 metadata 仍读出；截断 PNG 不直写。
- 失败标准：超限封面导致 RSS 激增、截断 PNG 被原样导出。
- 回滚：恢复默认 FFmpeg `av_image_check_size()` 逻辑。

Step 2:

- 修改位置：`ConvertImageToPng()`。
- 修改内容：解码后检查 `decodedFrame->width`、`height`、`width * height` 不超过 `CoverDecodeLimits`；乘法用 `int64_t` 且检查溢出；PNG encode 后检查输出 vector 大小不超过 `maxOutputBytes`。
- 示例代码结构：

```cpp
if (decodedFrame->width <= 0 || decodedFrame->height <= 0) return {};
const auto pixels = static_cast<int64_t>(decodedFrame->width) * decodedFrame->height;
if (decodedFrame->width > limits.maxWidth || decodedFrame->height > limits.maxHeight || pixels > limits.maxPixels) return {};
...
if (png.size() > limits.maxOutputBytes) return {};
```

- 风险：部分 FFmpeg decoder 在返回 frame 前已经消耗资源；业务限制不能完全替代进程级 sandbox。
- 回归影响：合法 JPEG/BMP/WEBP/GIF/TIFF 首帧仍可转 PNG。
- 验证方式：对不同图片格式嵌入 APIC/covr/PICTURE 后运行。
- 测试样本：JPEG、BMP、WEBP、GIF、TIFF、伪 WEBP、截断 GIF。
- 验收标准：合法格式导出 PNG；畸形格式不 crash；超大尺寸拒绝。
- 失败标准：FFmpeg error 导致进程 abort、畸形图片写出损坏 PNG。
- 回滚：恢复 `ConvertImageToPng()` 原尺寸检查。

Step 3:

- 修改位置：`WriteCoverAsPng()`。
- 修改内容：删除 PNG 签名直写路径；所有 cover 包括源 PNG 都必须经过 decode 验证和 PNG encode，再 atomic 写出。hash 仍使用原始 embedded bytes。
- 示例代码结构：

```cpp
std::vector<uint8_t> png = DecodeAndEncodeCoverPng(data, size);
if (png.empty()) return {};
AtomicWriteFileIfAbsent(finalPath, png.data(), png.size());
```

- 风险：源 PNG 也要解码，首次导出成本上升。
- 回归影响：第二次读取命中 cache，不重复 decode。
- 验证方式：伪 PNG 签名样本、合法 PNG APIC 样本各运行两次。
- 测试样本：以 PNG magic 开头但 IHDR 截断的 APIC；合法 1x1 PNG APIC。
- 验收标准：伪 PNG 不导出；合法 PNG 导出；第二次合法 PNG 不转码。
- 失败标准：伪 PNG 文件出现在 cover cache。
- 回滚：暂时恢复 PNG 直写，但保留输入大小上限。

# Phase 4 - MP4 atom walker 收敛与歌词递归移除

目标：
修复 MP4 歌词深递归 crash、任意层级歌词误读、`size == 0` 子 atom 接受、64 位 size 和 offset 处理不一致。该阶段合并修复 ANALYSIS 中 MP4 recursion、path confusion、largesize、size==0 问题，因为它们都集中在 atom header/walker 状态机，拆开修会制造重复逻辑。

修改内容：
修改 `ReadMp4AtomHeader()`、`ReadMP4AtomTree()`、`ReadMP4ItemAtom()`、`ReadMP4Lyrics()`、`ReadMP4LyricsAtomTree()`、`ReadMP4LyricsItem()`、`ReadMP4FreeformLyricsItem()`；新增 `Mp4AtomHeader` 参数字段、`Mp4PathState` 或显式 stack。

步骤：
Step 1:

- 修改位置：`ReadMp4AtomHeader()`。
- 修改内容：函数增加参数 `bool allowSizeZero`，只有 top-level 调用允许 `size == 0` 延伸到 limit；child atom 中遇到 `size == 0` 返回 invalid。保留 `TryAddUintmax()` 计算 `atomEnd`。
- 示例代码结构：

```cpp
std::optional<Mp4AtomHeader> ReadMp4AtomHeader(std::ifstream &input, std::uintmax_t offset, std::uintmax_t limit, bool allowSizeZero)
{
    ...
    if (size32 == 0) {
        if (!allowSizeZero) return std::nullopt;
        atomEnd = limit;
    }
}
```

- 风险：某些非标准但可读的 child size==0 atom 被跳过。
- 回归影响：合法 MP4 metadata 不变；畸形 child atom 不再吞 siblings。
- 验证方式：MP4 普通 `©nam`、`covr`、`©lyr` 样本；child `size==0` 畸形样本。
- 测试样本：合法 M4A metadata；`moov/udta/meta/ilst/©nam` 子 atom size=0 变体。
- 验收标准：合法样本字段可读；畸形 child size=0 不无限循环、不吞掉后续合法 atom。
- 失败标准：普通 M4A 所有 metadata 丢失。
- 回滚：恢复 `size==0` 统一延伸行为。

Step 2:

- 修改位置：`ReadMP4AtomTree()` 和 `ReadMP4ItemAtom()`。
- 修改内容：更新所有 `ReadMp4AtomHeader()` 调用，root 用 `allowSizeZero=true`，bounded child 用 `false`；所有 payload size 在 `ReadRange()` 前与 Phase 1 上限比较。
- 示例代码结构：

```cpp
auto atom = ReadMp4AtomHeader(context.input, cursor, limit, depth == 0);
```

- 风险：depth 条件写错会拒绝 top-level size==0。
- 回归影响：metadata path 与 lyrics path 后续一致。
- 验证方式：原有 MP4 metadata/covr 样本。
- 测试样本：普通 M4A `©nam`、`trkn`、`covr`。
- 验收标准：字段读取不回退；没有新增异常。
- 失败标准：`ReadMP4Metadata()` 不再返回任何字段。
- 回滚：只回滚调用参数，不改 helper。

Step 3:

- 修改位置：`ReadMP4LyricsAtomTree()`。
- 修改内容：移除递归实现，改为严格路径状态机或显式固定深度 traversal，只在 `moov/udta/meta/ilst` 下处理 `©lyr` 和 `----`。如果保留函数名，签名改为增加 `depth` 和 path 参数；更稳妥是内部显式 stack。
- 示例代码结构：

```cpp
enum class Mp4PathState { Root, Moov, Udta, Meta, Ilst };
struct PendingAtomRange { std::uintmax_t offset; std::uintmax_t limit; Mp4PathState state; std::uint32_t depth; };

while (!stack.empty()) {
    if (++visitedAtoms > kMaxMp4Atoms) return;
    ...
    if (state == Mp4PathState::Ilst && (atom.type == "©lyr" || atom.type == "----")) ...
}
```

- 风险：状态转移漏掉 `meta` full box 4 字节 payload offset。
- 回归影响：非标准位置歌词不再被读出，这是安全修复。
- 验证方式：合法 `moov/udta/meta/ilst/©lyr` 读出；非标准 root `©lyr` 不读；深度嵌套不 crash。
- 测试样本：合法 `©lyr` M4A；root 下直接放 `©lyr` 的恶意 MP4；深度 10000 嵌套 `moov` 样本。
- 验收标准：合法歌词 `lyricsCount > 0`；root 假歌词 `lyricsCount == 0`；深嵌套样本无栈溢出、RSS 稳定。
- 失败标准：深嵌套样本 segfault、合法 MP4 歌词丢失。
- 回滚：恢复递归函数，但保留临时 depth guard 用于止血。

Step 4:

- 修改位置：`ReadMP4LyricsItem()`、`ReadMP4FreeformLyricsItem()`。
- 修改内容：统一使用 `ReadMp4AtomHeader(..., false)` 读取 child；正确处理 `size == 1` largesize；所有 `cursor + size` 改为 `TryAddUintmax()`；`ReadRange()` 前检查 `kMaxLyricsBytes` 或 `kMaxTextFieldBytes`。
- 示例代码结构：

```cpp
auto atom = ReadMp4AtomHeader(context.input, cursor, limit, false);
if (!atom) return;
const auto payloadSize = atom->atomEnd - atom->payloadOffset;
if (payloadSize > kMaxLyricsBytes) return;
```

- 风险：freeform `mean`/`name`/`data` 顺序多样，严格 walker 不能假定顺序。
- 回归影响：64 位 atom 歌词开始可读，畸形 atom 安全跳过。
- 验证方式：构造 `©lyr` data atom 使用 `size==1` largesize；构造 `----:com.apple.iTunes:LYRICS`。
- 测试样本：普通 `©lyr`、largesize `©lyr`、freeform lyrics、截断 largesize header。
- 验收标准：普通和 largesize 歌词均可读；截断样本不 crash；freeform lyrics 可读。
- 失败标准：largesize 仍被 `size < 8` 逻辑跳过。
- 回滚：恢复原 item parser，但保留 `ReadMp4AtomHeader()` 修复。

Step 5:

- 修改位置：新增 `kMaxMp4Atoms` 常量，应用在 metadata 和 lyrics walker。
- 修改内容：限制单文件扫描 atom 数量，例如 `100000`；超过后停止当前 MP4 parser。
- 示例代码结构：

```cpp
if (++visitedAtoms > kMaxMp4Atoms) return;
```

- 风险：极端碎片化但合法文件 metadata 被提前停止。
- 回归影响：普通 M4A 无影响。
- 验证方式：构造 200000 个空 sibling atom 的 MP4。
- 测试样本：大量 sibling atom 样本；普通 M4A。
- 验收标准：大量 atom 样本在限定时间内返回；普通样本正常。
- 失败标准：大量 atom 样本 CPU 长时间占用。
- 回滚：提高上限或仅对 lyrics path 应用。

# Phase 5 - Ogg Vorbis packet 聚合与 page 状态校验

目标：
修复 Ogg continuation packet 无上限导致的 OOM，并提高 malformed page recovery 一致性。

修改内容：
修改 `ReadOggVorbisCommentEntries()`；新增 Ogg page 状态变量和上限计数。

步骤：
Step 1:

- 修改位置：`ReadOggVorbisCommentEntries()`。
- 修改内容：新增 `totalScannedBytes`、`pageCount`、`expectedSerial`、`expectedSequence`、`packet` 上限。每次 `packet.insert()` 前检查 `packet.size() + segmentSize <= kMaxOggPacketBytes`。
- 示例代码结构：

```cpp
if (segmentSize > kMaxOggPacketBytes - packet.size()) return false;
packet.insert(packet.end(), payload.begin() + payloadCursor, payload.begin() + payloadCursor + segmentSize);
```

- 风险：真实超长 comment packet 被拒绝。
- 回归影响：普通 Ogg/FLAC Vorbis comment 不变。
- 验证方式：正常 Ogg Vorbis comment、连续 255 lacing 恶意样本。
- 测试样本：一个含 `TITLE=Ok` 的 Ogg；一个 10000 页 continuation packet 不结束的 Ogg。
- 验收标准：正常 title 读出；恶意样本不超过内存预算且返回 false。
- 失败标准：恶意样本 RSS 持续增长或运行超时。
- 回滚：只移除新增上限检查。

Step 2:

- 修改位置：`ReadOggVorbisCommentEntries()` page header 读取后。
- 修改内容：校验 capture pattern `OggS`、version 为 0、segment count 与 payload offset 不越界、bitstream serial 连续、page sequence number 递增；当 packet 非空时要求 continuation flag 合理。
- 示例代码结构：

```cpp
const bool continuation = (pageHeader[5] & 0x01) != 0;
if (!packet.empty() && !continuation) return false;
if (haveSerial && serial != expectedSerial) return false;
```

- 风险：部分容错文件被拒绝。
- 回归影响：安全性提高，非法跨页不再拼接。
- 验证方式：正常多页 Ogg；serial 改变样本；sequence 跳号样本。
- 测试样本：合法 Ogg comment 跨两页；第二页 serial 改变；第二页 missing continuation flag。
- 验收标准：合法跨页可读；非法跨页返回 false 且不 crash。
- 失败标准：合法跨页 comment 丢失。
- 回滚：放宽 serial/sequence 校验，但保留 packet size 上限。

Step 3:

- 修改位置：`ReadVorbisCommentEntry()`、`ReadVorbisLyricsEntry()` 调用链。
- 修改内容：确保 Ogg parser 返回 false 时 metadata/lyrics 路径只跳过 Vorbis comment，不影响 `BuildMusicTag()` 的媒体信息。
- 示例代码结构：

```cpp
const bool ok = ReadOggVorbisCommentEntries(...);
(void)ok;
```

- 风险：错误传播过强会让整个 `Read()` 失败。
- 回归影响：畸形 Ogg 文件仍能返回基础媒体信息。
- 验证方式：畸形 Ogg continuation 样本运行 `TagReaderTest`。
- 测试样本：有音频流但 comment 损坏的 Ogg。
- 验收标准：不 crash；若 FFmpeg 可识别音频，基础媒体信息仍返回；metadata 可为空。
- 失败标准：comment 损坏导致未捕获异常。
- 回滚：恢复静默跳过策略。

# Phase 6 - ID3 tag 边界、extended header 与 flags 处理

目标：
修复 ID3 大 tag、extended header 边界不精确、v2.2 flags 语义混用和重复读取放大。Phase 1 已加上限，本阶段修 parser recovery。

修改内容：
修改 `ReadID3v2Metadata()`、`ReadID3Lyrics()`、`ReadID3v22Frames()`、`ReadID3v23Or24Frames()`、`ReadID3v22LyricsFrames()`、`ReadID3v23Or24LyricsFrames()`；新增 `ReadId3TagBytes()` 和 `ParseId3FrameStart()`。

步骤：
Step 1:

- 修改位置：`include/TagReader.hpp` 私有区、`src/TagReader.cpp`。
- 修改内容：新增内部 helper `ReadId3TagBytes(ReadContext&, uint8_t &versionMajor, uint8_t &flags, std::size_t &frameCursor, std::size_t &frameLimit)`，统一读取 header、tagSize、extended header 跳过逻辑；metadata 和 lyrics 调用同一个 helper。
- 示例代码结构：

```cpp
struct Id3TagView { uint8_t versionMajor; uint8_t flags; std::size_t cursor; std::size_t limit; std::vector<uint8_t> bytes; };
std::optional<Id3TagView> ReadId3TagBytes(ReadContext &context);
```

- 风险：改变 metadata 和 lyrics 的入口 cursor。
- 回归影响：减少重复读入同一 tag body，但本阶段可以先不缓存，只统一逻辑。
- 验证方式：ID3v2.2、v2.3、v2.4 样本分别验证 title 和 lyrics。
- 测试样本：v2.2 ULT/SLT，v2.3 USLT/SYLT，v2.4 TIT2/APIC。
- 验收标准：所有旧字段仍可读；超限 tag 被跳过。
- 失败标准：某一版本 ID3 全部失效。
- 回滚：恢复 metadata/lyrics 各自读取逻辑。

Step 2:

- 修改位置：`ReadID3v2Metadata()`、`ReadID3Lyrics()` extended header 分支。
- 修改内容：v2.3 校验 `4 + extSize <= frameLimit`；v2.4 syncsafe ext size 校验不超过 frameLimit；所有加法使用 `TryAddSize()` 或等价 helper。
- 示例代码结构：

```cpp
std::size_t extendedEnd{};
if (!TryAddSize(cursor, 4 + extSize, extendedEnd) || extendedEnd > frameLimit) return std::nullopt;
cursor = extendedEnd;
```

- 风险：一些异常 extended header 文件被跳过。
- 回归影响：合法 extended header 文件行为更准确。
- 验证方式：构造 `extSize == tagBytes.size()`、`extSize == frameLimit - 4`、合法 ext header 三类样本。
- 测试样本：ID3v2.3 带合法 extended header；ID3v2.3 extSize 越界。
- 验收标准：合法样本 title 可读；越界样本不越界、不 crash。
- 失败标准：越界样本 cursor wrap 或 title 误读。
- 回滚：恢复旧 extSize 判断。

Step 3:

- 修改位置：`ReadID3Lyrics()` 和 `ReadID3v22LyricsFrames()`。
- 修改内容：明确 v2.2 不支持当前 v2.3/v2.4 extended header flag；遇到 v2.2 flags 中未知位时统一跳过 ID3v2 lyrics，不复用 `flags & 0x40` 分支。
- 示例代码结构：

```cpp
if (versionMajor == 2 && flags != 0) return;
```

- 风险：带非标准 v2.2 flags 的文件歌词被跳过。
- 回归影响：正常 v2.2 flags=0 样本不变。
- 验证方式：v2.2 ULT/SLT flags=0 和 flags=0x40 样本。
- 测试样本：合法 v2.2 ULT；异常 v2.2 flags 置位。
- 验收标准：合法 ULT 读出；异常 flags 不 crash、不误读。
- 失败标准：合法 v2.2 歌词丢失。
- 回滚：恢复旧 flags 分支。

# Phase 7 - 文本解码严格化与 normalization provenance

目标：
修复非法 UTF 被兼容回退、Vorbis Comment 违反 UTF-8 规范、重复 normalization 状态污染、iconv/Latin-1 扩容缺少上限。该阶段属于 P1，但会降低 malformed 输入被伪装成合法字段的风险。

修改内容：
修改 `ReadId3ByteString()`、`ReadVorbisCommentEntry()`、`ReadVorbisLyricsEntry()`、`NormalizeMetadata()`、`NormalizeLyrics()`、`ConvertTextWithIconv()`、`ReadLatin1Text()`；新增 `DecodedText` 或在 `RawMetadata`/`RawLyrics` 内增加 provenance 字段。

步骤：
Step 1:

- 修改位置：`ReadId3ByteString()`。
- 修改内容：encoding byte 为 `3` 时只调用 `ReadUtf8Text()`；失败直接返回 false，不回退 Latin-1。encoding byte 为 `0` 才允许 Latin-1。
- 示例代码结构：

```cpp
case 3:
    return ReadUtf8Text(raw, value);
```

- 风险：历史上错误写入 UTF-8 标记但实际 Latin-1 的文件字段会丢失。
- 回归影响：符合 ID3v2.4 声明语义。
- 验证方式：合法 UTF-8 TIT2、非法 UTF-8 TIT2、Latin-1 encoding=0 TIT2。
- 测试样本：ID3v2.4 `TIT2` encoding=3 合法中文；encoding=3 非法 `0xFF`；encoding=0 Latin-1 `Cafe`。
- 验收标准：合法 UTF-8 读出；非法 UTF-8 不写乱码；Latin-1 encoding=0 仍可读。
- 失败标准：合法 UTF-8 title 丢失。
- 回滚：恢复 `ReadUtf8Text() || ReadLatin1Text()`，仅用于兼容排障。

Step 2:

- 修改位置：`ReadVorbisCommentEntry()`、`ReadVorbisLyricsEntry()`。
- 修改内容：Vorbis key/value 严格按 UTF-8 解码，调用 `DecodeTextToUtf8(..., "utf-8")`；失败则丢弃该 entry。禁止调用 `DecodeRawText()` 做本地编码回退。
- 示例代码结构：

```cpp
DecodedField decoded = DecodeTextToUtf8(value, "utf-8");
if (!decoded.success) return;
```

- 风险：非规范 legacy Vorbis comment 不再兼容。
- 回归影响：规范 FLAC/Ogg comment 不变。
- 验证方式：合法 UTF-8 Vorbis comment、非法 UTF-8 comment。
- 测试样本：FLAC `TITLE=合法`；Ogg `TITLE=\xff\xfe`。
- 验收标准：合法字段读出；非法字段丢弃且不回退成乱码。
- 失败标准：合法 UTF-8 被拒绝。
- 回滚：恢复 `DecodeRawText()` 调用。

Step 3:

- 修改位置：`RawMetadata`、`RawLyrics`、`NormalizeMetadata()`、`NormalizeLyrics()`。
- 修改内容：渐进式增加 provenance，不全面重写结构。新增 helper `AssignDecoded(std::string &field, std::string value)` 和 `NormalizeAlreadyUtf8Field()`；已由 parser 解码成功的字段只做 trim 和 UTF-8 validation，不再嗅探 UTF-16/legacy。
- 示例代码结构：

```cpp
struct TextValue { std::string value; bool decodedUtf8{}; };
```

- 风险：改动 `RawMetadata` 字段类型会牵连大量赋值点。
- 回归影响：若类型改动过大，容易引入编译错误；因此先用最小 helper 和注释标记，后续 P2 再完整结构化。
- 验证方式：ID3/Vorbis/MP4 三种来源同一 title；短文本边界样本。
- 测试样本：`A`、UTF-16 BOM title、Vorbis UTF-8 title、MP4 UTF-8 title。
- 验收标准：已解码 UTF-8 字段不被二次误判；输出仍为 UTF-8。
- 失败标准：UTF-16 ID3 title 未被转换、Vorbis title 被错误清空。
- 回滚：恢复原 `NormalizeText()` 全字段调用。

Step 4:

- 修改位置：`ConvertTextWithIconv()`、`ReadLatin1Text()`。
- 修改内容：加入输出最大值 `kMaxDecodedTextBytes`；`output.resize(output.size() * 2)` 前检查乘法溢出和最大值；`ReadLatin1Text()` 的 `reserve(size * 2)` 改为安全上限或渐进 push。
- 示例代码结构：

```cpp
if (output.size() > kMaxDecodedTextBytes / 2) return false;
const auto nextSize = output.size() * 2;
if (nextSize > kMaxDecodedTextBytes) return false;
```

- 风险：极长文本字段被截断或丢弃。
- 回归影响：普通 metadata 文本无影响。
- 验证方式：1 KiB Latin-1、2 MiB Latin-1、iconv E2BIG 压力输入。
- 测试样本：ID3 encoding=0 超大 text frame；MP4 超大 UTF-16 data。
- 验收标准：小文本正常；超大文本安全失败，无 OOM。
- 失败标准：`std::length_error` 未捕获或 RSS 激增。
- 回滚：恢复原扩容逻辑。

# Phase 8 - LRC parser 严格化

目标：
修复非法 LRC 时间戳被解析为 0、多时间戳行文本污染。该阶段独立且低风险，可在文本严格化后执行。

修改内容：
修改 `ParseLrcTimestamp()`、`ReadLyricsFromPlainText()`；新增严格数字解析 helper。

步骤：
Step 1:

- 修改位置：`ParseLrcTimestamp()`。
- 修改内容：新增 `ParseDecimalU16Strict(std::string_view, uint16_t&)`，要求至少消费一位数字且全部字符为数字；seconds 必须 `< 60`；毫秒部分只允许 1 到 3 位数字，按 LRC 规则补齐到毫秒。
- 示例代码结构：

```cpp
std::optional<std::uint16_t> ParseDecimalU16Strict(std::string_view text);
if (!minutes || !seconds || *seconds >= 60) return false;
```

- 风险：宽松格式 `[1:2]` 是否接受需要明确；本计划接受，因为 minutes/seconds 至少一位数字。
- 回归影响：非法 timestamp 不再产生 0 时间歌词。
- 验证方式：合法和非法 token 单独嵌入 USLT/TXXX lyrics。
- 测试样本：`[00:01.500]ok`、`[1:2]ok`、`[abc:def]bad`、`[00:xx]bad`、`[00:60]bad`。
- 验收标准：合法输出 timed line；非法作为普通文本或跳过 timestamp，不生成 `00:00` timed line。
- 失败标准：`[abc:def]` 生成 timestamp 0。
- 回滚：恢复原 `ParseUInt16()` 路径。

Step 2:

- 修改位置：`ReadLyricsFromPlainText()`。
- 修改内容：行首循环收集所有连续 timestamp，最后从最后一个 `]` 后截取歌词文本，为每个 timestamp 添加同一文本。非行首 timestamp 不作为时间轴解析。
- 示例代码结构：

```cpp
std::vector<std::chrono::microseconds> timestamps;
std::size_t scan = 0;
while (scan < line.size() && line[scan] == '[') { ... }
std::string text = TrimText(line.substr(scan));
for (auto ts : timestamps) AppendTimedLyrics(lyrics, ts, text);
```

- 风险：历史上行中 timestamp 的容错行为被移除。
- 回归影响：标准 LRC 多时间戳行正确。
- 验证方式：多时间戳样本。
- 测试样本：`[00:01.00][00:02.00]same lyric`。
- 验收标准：生成两条 timed line，文本均为 `same lyric`，不包含第二个 timestamp 字符串。
- 失败标准：第一条歌词文本为 `[00:02.00]same lyric`。
- 回滚：恢复原逐个 `find('[')` 逻辑。

# Phase 9 - Dispatch 统一到 DetectedContainer

目标：
修复 signature 与 `containerName` 二次检查不一致导致的 parser 跳过和状态污染。该阶段不拆 God Object，只把格式判定收敛到 `ReadContext`。

修改内容：
修改 `include/TagReader.hpp` 的 `ReadContext`；修改 `DetectContainerFromSignature()`、`ReadMetadata()`、`ReadLyrics()`、`ReadVorbisCommentMetadata()`、`ReadMP4Metadata()`、`ReadVorbisLyrics()`、`ReadMP4Lyrics()`。

步骤：
Step 1:

- 修改位置：`include/TagReader.hpp`。
- 修改内容：新增私有 enum `DetectedContainer { Unknown, Mp3, Flac, OggVorbis, Mp4 }`，在 `ReadContext` 中保存 `detectedContainer`。
- 示例代码结构：

```cpp
enum class DetectedContainer { Unknown, Mp3, Flac, OggVorbis, Mp4 };
DetectedContainer detectedContainer{DetectedContainer::Unknown};
```

- 风险：头文件私有区变化导致实现签名同步工作量。
- 回归影响：外部 ABI 源码层面无 public 字段变化。
- 验证方式：构建。
- 测试样本：无。
- 验收标准：编译通过。
- 失败标准：public API 依赖新增 enum。
- 回滚：删除 enum 和字段。

Step 2:

- 修改位置：`DetectContainerFromSignature()` 或新增 `DetectContainer(ReadContext&)`。
- 修改内容：将字符串返回改为 enum；优先签名，必要时参考 `containerName` 别名；在 `Read()` 的 `DetectStream()` 后写入 `context.detectedContainer`。
- 示例代码结构：

```cpp
context.detectedContainer = DetectContainer(context);
```

- 风险：现有调用 `DetectContainerFromSignature()` 的地方需要同步。
- 回归影响：格式判断更稳定。
- 验证方式：MP3/FLAC/Ogg/M4A 四类样本。
- 测试样本：扩展名错误但签名正确的文件；FFmpeg `containerName` 为空或别名样本。
- 验收标准：四类样本进入正确 parser；扩展名不影响解析。
- 失败标准：FLAC 被识别为 Unknown。
- 回滚：恢复字符串 dispatch。

Step 3:

- 修改位置：`ReadMetadata()`、`ReadLyrics()` 及具体 parser 入口。
- 修改内容：分发只 switch `context.detectedContainer`；删除具体 parser 入口中重复 `containerName.find()` 拒绝逻辑。
- 示例代码结构：

```cpp
switch (context.detectedContainer) {
case DetectedContainer::Mp4: ReadMP4Metadata(context, metadata); break;
...
}
```

- 风险：删除二次检查后错误 enum 会直接进入错误 parser。
- 回归影响：依赖 Phase 9 Step 2 的准确识别。
- 验证方式：所有格式 smoke。
- 测试样本：MP3 ID3、FLAC Vorbis、Ogg Vorbis、M4A MP4 metadata。
- 验收标准：四类 metadata 和 lyrics 行为不回退。
- 失败标准：某格式 parser 不再执行。
- 回滚：恢复 parser 入口二次检查。

# Phase 10 - MP4 metadata 兼容性与 numeric data 校验

目标：
修复 MP4 text data type 支持不完整、`trkn`/`disk` payload 校验过弱。该阶段在 MP4 walker 稳定后执行。

修改内容：
修改 `ReadMP4DataAtom()`、`ReadMP4ItemAtom()`；新增 `DecodeMp4TextData()` 和 `ParseMp4TrackDiskNumber()`。

步骤：
Step 1:

- 修改位置：`ReadMP4DataAtom()`。
- 修改内容：新增 `DecodeMp4TextData(dataType, payload, payloadSize)`；支持 `dataType == 1` UTF-8、`dataType == 0` raw UTF-8 fallback、常见 UTF-16BE/UTF-16LE 类型；所有解码必须受 `kMaxTextFieldBytes` 限制。
- 示例代码结构：

```cpp
std::optional<std::string> DecodeMp4TextData(std::uint32_t dataType, const uint8_t *payload, std::size_t size);
```

- 风险：MP4 data type 数值映射错误会误解 payload。
- 回归影响：UTF-8 旧样本不变，UTF-16 样本开始可读。
- 验证方式：MP4 `©nam` UTF-8 和 UTF-16 样本。
- 测试样本：`©nam` dataType=1 UTF-8；`©nam` UTF-16BE；非法 UTF-16 odd length。
- 验收标准：UTF-8/UTF-16 合法 title 读出；非法 UTF-16 丢弃。
- 失败标准：UTF-8 title 回归为空。
- 回滚：恢复只处理 dataType 0/1。

Step 2:

- 修改位置：`ReadMP4DataAtom()` 的 `trkn`/`disk` 分支。
- 修改内容：按 Apple metadata 结构解析 reserved/index/total；要求 payload 至少 6 或 8 字节，reserved 为 0；index 为 0 时不写入；total 可忽略但不能越界。
- 示例代码结构：

```cpp
std::optional<std::uint16_t> ParseMp4TrackDiskNumber(const uint8_t *payload, std::size_t size)
{
    if (size < 6 || ReadBE16(payload) != 0) return std::nullopt;
    const auto index = ReadBE16(payload + 2);
    return index == 0 ? std::nullopt : std::optional(index);
}
```

- 风险：非标准 payload 被拒绝。
- 回归影响：合法 track/disk 不变。
- 验证方式：合法 `trkn`、reserved 非零、index 0、截断 payload。
- 测试样本：M4A track 3/10；reserved 非零；payload 4 字节。
- 验收标准：合法 trackNumber=3；异常 payload 不写错误编号。
- 失败标准：截断 payload 读越界或输出随机编号。
- 回滚：恢复旧 `[2..3]` 读取。

# Phase 11 - 媒体信息窄化与格式名稳定

目标：
修复 `bitDepth`、`format`、`channels`、`bitRate` 输出不稳定和窄化截断。该阶段不触碰 tag parser。

修改内容：
修改 `ReadMediaInfo()`；新增 `ClampToUint32()`、`ClampToUint8()`、`NormalizeContainerFormatName()`。

步骤：
Step 1:

- 修改位置：`ReadMediaInfo()`。
- 修改内容：声道数写入前检查 `0 <= channels <= UINT8_MAX`，否则写 0；bitRate 检查 `0 <= bit_rate <= UINT32_MAX`，否则写 0；bitDepth 优先读取 codecpar 可用字段并对 FLAC 使用更可靠字段。
- 示例代码结构：

```cpp
mediaInfo.channels = InUint8Range(channels) ? static_cast<uint8_t>(channels) : 0;
mediaInfo.bitRate = InUint32Range(bitRate) ? static_cast<uint32_t>(bitRate) : 0;
```

- 风险：极端参数由截断值变成 0。
- 回归影响：普通音频不变。
- 验证方式：普通 WAV/FLAC/M4A/MP3。
- 测试样本：FLAC 16-bit、FLAC 24-bit、M4A、MP3。
- 验收标准：FLAC bitDepth 输出 16/24；M4A format 稳定；极端值不截断。
- 失败标准：channels 由 2 变为 0。
- 回滚：恢复原 cast。

Step 2:

- 修改位置：`ReadMediaInfo()` format 赋值。
- 修改内容：新增 `NormalizeContainerFormatName()`，将 `mov,mp4,m4a,3gp,3g2,mj2` 映射为 `m4a` 或 `mp4`，FLAC 输出 `flac`，MP3/MPEG 输出 `mp3`，Ogg 输出 `ogg`。
- 示例代码结构：

```cpp
mediaInfo.format = NormalizeContainerFormatName(context);
```

- 风险：显示层依赖原 FFmpeg name 列表时行为变化。
- 回归影响：输出更稳定。
- 验证方式：四类格式样本。
- 测试样本：`.m4a`、`.mp4`、`.flac`、`.mp3`、`.ogg`。
- 验收标准：format 为单一稳定值，不是 FFmpeg 候选列表。
- 失败标准：format 为空或误报。
- 回滚：恢复 `iformat->name`。

# Phase 12 - Malformed recovery 策略收敛

目标：
统一 P0/P1 修复后的错误语义：单个 tag/parser 损坏不导致无关字段失败；输入路径、FFmpeg probe、无音频流仍明确失败。

修改内容：
修改 `ReadMetadata()`、`ReadLyrics()`、各格式 parser 的异常边界；新增文件内 helper `IgnoreMalformedTag()` 或局部 try/catch 包裹格式分支。

步骤：
Step 1:

- 修改位置：`ReadMetadata()`。
- 修改内容：对 ID3/FLAC/Ogg/MP4 metadata 分支分别局部捕获 parser 内抛出的 `std::runtime_error`/`std::filesystem_error`，只跳过当前格式 metadata；不要吞掉 `OpenContext()`、`DetectStream()` 的失败。
- 示例代码结构：

```cpp
try { ReadID3v2Metadata(context, metadata); }
catch (const std::exception &) { /* malformed tag: keep media info */ }
```

- 风险：真实编程错误被误吞。
- 回归影响：畸形标签更鲁棒。
- 验证方式：截断 ID3、截断 FLAC block、截断 MP4 atom。
- 测试样本：每种格式一个合法样本和一个截断 tag 样本。
- 验收标准：合法样本正常；畸形 tag 不 crash，基础媒体信息可返回。
- 失败标准：路径不存在也被吞掉并返回空 `MusicTag`。
- 回滚：移除局部 catch。

Step 2:

- 修改位置：`ReadLyrics()`。
- 修改内容：歌词 parser 失败只清空 lyrics，不影响 metadata；同步歌词条目级 malformed 只停止当前帧，不影响其他帧。
- 示例代码结构：

```cpp
RawLyrics lyrics;
try { ... } catch (...) { lyrics = {}; }
```

- 风险：隐藏歌词 parser 的开发错误。
- 回归影响：主标签读取更稳定。
- 验证方式：损坏 USLT/SYLT/SLT、损坏 MP4 lyrics、损坏 Vorbis lyrics。
- 测试样本：timestamp 截断 SYLT、MP4 `©lyr` data 截断、Vorbis LYRICS 非法 UTF-8。
- 验收标准：title/artist 仍可读；lyricsCount 为 0 或保留其他合法歌词。
- 失败标准：损坏歌词导致整个 `Read()` 失败。
- 回滚：恢复原异常传播。

# Phase 13 - Fuzz harness 与 sanitizer 回归集

目标：
把已修复的 P0/P1 问题固化为可重复安全回归，防止 God Object 后续修改重新引入 OOM、递归、越界和 unsafe cover 行为。

修改内容：
新增 `test/fuzz/` harness；修改 `CMakeLists.txt` 增加可选 sanitizer/fuzzer targets；不改变库行为。

步骤：
Step 1:

- 修改位置：`CMakeLists.txt`。
- 修改内容：新增选项 `TAGREADER_ENABLE_SANITIZERS` 和 `TAGREADER_ENABLE_FUZZING`；sanitizer 编译参数为 `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all`。
- 示例代码结构：

```cmake
option(TAGREADER_ENABLE_SANITIZERS "Enable ASAN/UBSAN" OFF)
```

- 风险：非 clang/gcc 环境参数不兼容。
- 回归影响：默认 OFF，不影响普通构建。
- 验证方式：默认构建和 sanitizer 构建各跑一次。
- 测试样本：Phase 0 生成的 smoke 样本。
- 验收标准：默认构建无变化；sanitizer 构建通过。
- 失败标准：默认构建被 sanitizer flags 污染。
- 回滚：删除选项和 flags。

Step 2:

- 修改位置：新增 `test/fuzz/tagreader_fuzz.cpp`。
- 修改内容：libFuzzer harness 将 fuzz bytes 写入私有临时音频文件，调用 `TagReader::Read(tempPath, coverDir)`；捕获 C++ 异常但不捕获 sanitizer 崩溃；每轮清理临时文件和 cover dir。
- 示例代码结构：

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    auto path = WriteTempInput(data, size);
    try { (void)TagReader::Read(path, CoverDir()); } catch (...) {}
    Cleanup(path);
    return 0;
}
```

- 风险：每轮调用 FFmpeg probe 成本高。
- 回归影响：仅 fuzz target。
- 验证方式：运行 60 秒 smoke fuzz。
- 测试样本：ID3、MP4、Ogg、FLAC corpus seed。
- 验收标准：60 秒内无 ASAN/UBSAN 崩溃、无无限增长 cover 文件。
- 失败标准：fuzzer 在已知深嵌套 MP4 上 stack overflow。
- 回滚：删除 fuzz target。

Step 3:

- 修改位置：新增 `test/corpus/README.md` 和 seed 生成脚本。
- 修改内容：固定 corpus 类别：最小 MP3+ID3v2.2/2.3/2.4、最小 FLAC block chain、最小 Ogg Vorbis page、最小 MP4 atom tree、图片 payload corpus、文本编码 corpus。
- 示例代码结构：无。
- 风险：仓库存二进制样本会增大体积；优先生成脚本而非提交大样本。
- 回归影响：无。
- 验证方式：脚本生成 corpus 后运行 fuzzer。
- 测试样本：上述六类。
- 验收标准：每类至少 3 个 seed，覆盖合法、截断、超限变体。
- 失败标准：corpus 不能生成或路径依赖上级目录。
- 回滚：删除 corpus 脚本。

# Phase 14 - 渐进式结构清理边界

目标：
在安全稳定后做最低风险结构清理，降低后续维护成本。禁止一次性拆 God Object；只提取无状态 helper 和小类型，不改变 public API 和 parser 顺序。

修改内容：
仅在 `src/TagReader.cpp` 内重排匿名命名空间 helper；必要时新增 `src/TagReaderLimits.hpp` 或 `src` 私有头，但不拆公共头。

步骤：
Step 1:

- 修改位置：`src/TagReader.cpp` 匿名命名空间。
- 修改内容：把 Phase 1 到 Phase 13 新增的预算常量、hash、atomic write、MP4 atom header、Ogg page helper 分区排列；不移动 parser 函数主体。
- 示例代码结构：

```cpp
// Resource limits
// Byte readers
// Cover cache helpers
// MP4 atom helpers
// Ogg helpers
```

- 风险：纯移动代码可能引入前置声明缺失。
- 回归影响：无行为变化。
- 验证方式：构建和全量 smoke 样本。
- 测试样本：前面所有合法样本和畸形样本。
- 验收标准：git diff 只显示移动和局部前置声明；所有 smoke 通过。
- 失败标准：行为 diff 或新增 parser 逻辑变化。
- 回滚：恢复原 helper 顺序。

Step 2:

- 修改位置：可选新增 `src/TagReaderInternal.hpp`。
- 修改内容：只放内部 enum、limit struct、小 POD，例如 `DetectedContainer`、`CoverDecodeLimits`、`Mp4AtomHeader`；不放 parser 实现，不改变 `include/` public API。
- 示例代码结构：

```cpp
namespace tagreader_internal {
struct Mp4AtomHeader { ... };
}
```

- 风险：新增私有头会影响 include 路径和编译依赖。
- 回归影响：无行为变化。
- 验证方式：构建。
- 测试样本：全量 smoke。
- 验收标准：编译通过；公共 `include/` 不暴露内部类型。
- 失败标准：外部用户需要包含私有头才能编译。
- 回滚：把小类型移回 `TagReader.cpp`。

Step 3:

- 修改位置：`TASKS.md` 和 `ANALYSIS.md`。
- 修改内容：每完成一个 Phase，在 `TASKS.md` 对应阶段标记完成样本、命令、结果；不要删除原验收标准。若发现新 bug，追加到 `BUGS.md`，不要混入实施计划。
- 示例代码结构：无。
- 风险：文档与代码不同步。
- 回归影响：无。
- 验证方式：每次提交前读 `TASKS.md` 当前 Phase。
- 测试样本：无。
- 验收标准：每个 Phase 都有构建命令、样本路径、实际输出摘要。
- 失败标准：只改代码不记录验收结果。
- 回滚：恢复上一版文档内容。

# 稳定化完成验收

目标：
确认 P0/P1 风险已被代码和测试同时覆盖，且没有全面架构重写。

步骤：
Step 1:

- 修改位置：无。
- 修改内容：运行标准构建。
- 示例代码结构：

```text
cmake -S . -B build
cmake --build build
```

- 风险：无。
- 回归影响：无。
- 验证方式：命令返回 0。
- 测试样本：无。
- 验收标准：`TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke` 构建成功。
- 失败标准：任一目标构建失败。
- 回滚：回滚最近 Phase 的代码修改。

Step 2:

- 修改位置：无。
- 修改内容：运行安全 smoke 样本。
- 示例代码结构：

```text
./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_cache <samples...>
```

- 风险：样本生成脚本缺依赖 ffmpeg CLI。
- 回归影响：无。
- 验证方式：检查返回码、输出字段、cover cache 目录。
- 测试样本：ID3v2.4 APIC MP3、FLAC PICTURE、Ogg Vorbis comment、M4A `©lyr`、freeform lyrics、深嵌套 MP4、超大 Ogg continuation、超大 ID3 size、截断 PNG APIC、非法 UTF-8 Vorbis、非法 LRC。
- 验收标准：合法样本字段正确；畸形样本不 crash、不 OOM、不 stack overflow；cover dedup 成功；已有 hash PNG 不重复 transcode。
- 失败标准：任何畸形样本 crash、RSS 无界增长、cover 写到非调用方目录、重复生成同 hash PNG。
- 回滚：回滚触发失败的 Phase；若无法定位，优先回滚最近涉及该格式 parser 的 Phase。

Step 3:

- 修改位置：无。
- 修改内容：运行 sanitizer smoke。
- 示例代码结构：

```text
cmake -S . -B build-asan -DTAGREADER_ENABLE_SANITIZERS=ON
cmake --build build-asan
./build-asan/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_cache_asan <samples...>
```

- 风险：sanitizer 下 FFmpeg 第三方库可能有环境噪声。
- 回归影响：无。
- 验证方式：ASAN/UBSAN 无报告。
- 测试样本：Phase 13 corpus smoke 子集。
- 验收标准：无 sanitizer crash、无 undefined behavior 报告。
- 失败标准：ASAN heap-buffer-overflow、stack-overflow、UBSAN integer overflow 出现在本库代码。
- 回滚：回滚对应 parser 修改并添加最小复现样本。

# 稳定化实施验收记录

记录日期：2026-05-23。

构建命令：

```text
cmake -S . -B build
cmake --build build
cmake -S . -B build-sanitizers -DTAGREADER_ENABLE_SANITIZERS=ON
cmake --build build-sanitizers
cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DTAGREADER_ENABLE_FUZZING=ON -DTAGREADER_ENABLE_SANITIZERS=ON
cmake --build build-fuzz-clang
```

样本生成命令：

```text
python3 test/security/generate_samples.py
python3 test/corpus/generate_corpus.py
```

关键样本目录：

```text
/tmp/opencode/tagreader_security_samples
/tmp/opencode/tagreader_fuzz_corpus
```

## Phase 0

- 完成内容：新增 `TagReaderSecuritySmoke`、`test/security/security_smoke.cpp`、`test/security/generate_samples.py`。
- 验证命令：`cmake --build build`；`python3 test/security/generate_samples.py`。
- 样本：`base.mp3`、`base.m4a`、`base.ogg`、`id3v24_apic_png.mp3`、`id3v24_uslt_lrc.mp3`。
- 结果摘要：安全 smoke 目标构建成功，样本生成目录为 `/tmp/opencode/tagreader_security_samples`。

## Phase 1

- 完成内容：接入全局读取预算、字段预算、歌词/封面/MP4/Ogg/ID3 限制和受限 `ReadRange()`。
- 验证命令：`cmake --build build`；`./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_phase12 ...`。
- 样本：`id3v24_declared_17m_padded.mp3`、`ogg_continuation_packet.ogg`、`base.flac`、`base.ogg`。
- 结果摘要：超限 tag 或 packet 不触发无界分配；基础媒体信息仍可返回或在 FFmpeg probe 阶段明确失败。

## Phase 2

- 完成内容：新增 `TagReader::Read(filePath, coverExportDir)`，封面导出由调用方目录控制，旧入口保持兼容。
- 验证命令：`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/id3v24_apic_png.mp3 /tmp/opencode/tagreader_cover_phase13_san`。
- 样本：`id3v24_apic_png.mp3`。
- 结果摘要：封面导出到调用方指定目录，旧 `Read(filePath)` 不导出封面。

## Phase 3

- 完成内容：封面统一 decode 后 re-encode PNG，增加尺寸、像素数、输入和输出大小限制。
- 验证命令：`./build-sanitizers/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_phase13_san ... id3v24_apic_png.mp3`。
- 样本：`id3v24_apic_png.mp3`、`image_apic_truncated.mp3`、`image_apic_large_payload.mp3` corpus seed。
- 结果摘要：合法 APIC PNG 导出为 content-addressed PNG；截断或超限图片被跳过，无 sanitizer 报告。

## Phase 4

- 完成内容：MP4 lyrics walker 改为显式 stack，支持 largesize，限制 atom 访问数，路径限定到 `moov/udta/meta/ilst`。
- 验证命令：`./build-fuzz-clang/TagReaderFuzz -runs=64 -max_len=8192 /tmp/opencode/tagreader_fuzz_corpus/mp4`。
- 样本：`mp4_deep_atoms.m4a` corpus seed、`mp4_truncated_atom.m4a` corpus seed。
- 结果摘要：深嵌套 MP4 corpus 短跑无 stack overflow 和 ASAN/UBSAN 报告。

## Phase 5

- 完成内容：Ogg page、serial、sequence、continuation、扫描字节和 packet 拼接预算收敛。
- 验证命令：`./build-fuzz-clang/TagReaderFuzz -runs=64 -max_len=8192 /tmp/opencode/tagreader_fuzz_corpus/ogg`。
- 样本：`ogg_valid_vorbis_pages.ogg`、`ogg_truncated_page.ogg`、`ogg_bad_continuation.ogg` corpus seeds。
- 结果摘要：畸形 Ogg corpus 不 crash，不无界增长。

## Phase 6

- 完成内容：ID3 tag header/body/extended header 读取统一到 `ReadId3TagBytes()`。
- 验证命令：`./build/TagReaderSecuritySmoke ... /tmp/opencode/tagreader_security_samples/id3v24_uslt_lrc.mp3 /tmp/opencode/tagreader_security_samples/id3v24_declared_17m_padded.mp3`。
- 样本：`id3v24_uslt_lrc.mp3`、`id3v24_declared_17m_padded.mp3`。
- 结果摘要：合法 ID3 继续可读；oversized padded ID3 metadata 被跳过，媒体信息返回。

## Phase 7

- 完成内容：严格 UTF-8 provenance，Vorbis/MP4/ID3 已解码字段不再重复宽松嗅探；文本解码输出受限。
- 验证命令：`./build-fuzz-clang/TagReaderFuzz -runs=64 -max_len=8192 /tmp/opencode/tagreader_fuzz_corpus/encoding`。
- 样本：`encoding_utf8.mp3`、`encoding_utf16.mp3`、`encoding_invalid_utf8.mp3` corpus seeds。
- 结果摘要：合法 UTF-8/UTF-16 seed 正常；非法 UTF-8 不污染字段，不触发 sanitizer。

## Phase 8

- 完成内容：LRC timestamp 严格解析；多时间戳行只解析行首连续 timestamp，并为每个 timestamp 添加同一文本。
- 验证命令：`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_lrc_multi_timestamp.ogg`；`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/ogg_lrc_bad_alpha.ogg`。
- 样本：`ogg_lrc_multi_timestamp.ogg`、`ogg_lrc_valid_00500.ogg`、`ogg_lrc_valid_short.ogg`、`ogg_lrc_bad_alpha.ogg`。
- 结果摘要：`[00:01.00][00:02.00]same lyric` 输出两条 timed line，非法行首 timestamp 不回退为 `0us` plain lyric。

## Phase 9

- 完成内容：新增 `DetectedContainer`，在 `DetectStream()` 后写入 `ReadContext`，metadata/lyrics dispatch 切换为 `switch (detectedContainer)`。
- 验证命令：`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/title_m4a.wrong` 等错误扩展名样本。
- 样本：`title_mp3.wrong`、`title_flac.wrong`、`title_ogg.wrong`、`title_m4a.wrong`。
- 结果摘要：错误扩展名但签名正确的 MP3/FLAC/Ogg/M4A 均进入正确 parser，title 正常读取。

## Phase 10

- 完成内容：MP4 text data type 支持 UTF-8、raw UTF-8、UTF-16BE、UTF-16LE；`trkn`/`disk` 校验 reserved/index/total。
- 验证命令：`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_text_utf16be.m4a`；`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/mp4_track_valid.m4a`。
- 样本：`mp4_text_utf8.m4a`、`mp4_text_utf16be.m4a`、`mp4_text_utf16le.m4a`、`mp4_text_utf16be_odd.m4a`、`mp4_track_valid.m4a`、`mp4_track_reserved_nonzero.m4a`、`mp4_track_index_zero.m4a`、`mp4_track_truncated4.m4a`。
- 结果摘要：合法 UTF-8/UTF-16 title 读出；奇数字节 UTF-16 丢弃；合法 `track=3/10` 输出 `trackNumber: 3`，异常 payload 不写随机编号。

## Phase 11

- 完成内容：媒体信息窄化改为 clamp；format 输出稳定化为 `mp3`、`flac`、`ogg`、`m4a`、`mp4`。
- 验证命令：`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/p11_flac24.flac`；`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/p11_format_m4a.wrong`。
- 样本：`p11_mp3.mp3`、`p11_m4a.m4a`、`p11_flac16.flac`、`p11_flac24.flac`、`p11_format.mp3`、`p11_format.flac`、`p11_format.ogg`、`p11_format.m4a`、`p11_format.mp4`、`p11_format_m4a.wrong`。
- 结果摘要：FLAC 输出 `bitDepth: 16/24`；普通 stereo 输出 `channels: 2`；M4A 错误扩展名仍输出 `format: m4a`。

## Phase 12

- 完成内容：metadata 和 lyrics parser malformed recovery 局部捕获，前置路径/FFmpeg/无音频流失败仍明确失败。
- 验证命令：`./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_phase12_step2 ...`；`./build/TagReaderTest /tmp/opencode/tagreader_security_samples/does_not_exist.mp3`。
- 样本：`id3v24_declared_17m_padded.mp3`、`id3_bad_uslt_utf8.mp3`、`mp4_bad_lyrics_utf8.m4a`。
- 结果摘要：损坏 metadata/lyrics 不影响媒体信息和 title；路径不存在仍输出错误并返回失败。

## Phase 13

- 完成内容：新增 sanitizer/fuzzing CMake 选项、`TagReaderFuzz`、corpus 生成脚本和 corpus README。
- 验证命令：`./build-sanitizers/TagReaderSecuritySmoke ...`；`./build-fuzz-clang/TagReaderFuzz -runs=128 -max_len=8192 /tmp/opencode/tagreader_fuzz_corpus/id3 ...`。
- 样本：`/tmp/opencode/tagreader_fuzz_corpus` 下 `id3`、`flac`、`ogg`、`mp4`、`image`、`encoding` 六类 seed。
- 结果摘要：默认构建不带 sanitizer flags；Clang fuzz target 构建成功；corpus 短跑无 ASAN/UBSAN 崩溃，fuzzer 临时 input 和 cover 目录无残留。

## Phase 14

- 完成内容：匿名命名空间 helper 分区整理；新增 `src/TagReaderInternal.hpp`，只迁移内部 POD/limit 类型；公共 `include/` 未暴露私有头。
- 验证命令：`cmake --build build`；`./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_cover_phase14_step2 ...`；`./build-fuzz-clang/TagReaderFuzz -runs=64 -max_len=8192 ...`。
- 样本：`base.mp3`、`base.flac`、`base.ogg`、`base.m4a`、`id3v24_uslt_lrc.mp3`、`id3v24_declared_17m_padded.mp3`、`mp4_bad_lyrics_utf8.m4a`、完整 fuzz corpus。
- 结果摘要：构建、smoke、fuzz corpus 短跑均通过；公共 `include/` 未引用 `TagReaderInternal.hpp`。
