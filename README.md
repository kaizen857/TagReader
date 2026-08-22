# TagReader

高性能 C++23 音频标签与元数据读取库。从常见音频容器中读取标题、艺术家、专辑、歌词与封面，统一输出 `MusicTag`。

## 项目简介

TagReader 是一个 C++23 实现的音频标签读取库，设计目标是在保持解析正确性的同时提供可预测的资源占用与安全边界：

- **标签由原始字节 parser 读取**。ID3、Vorbis Comment、MP4 `ilst`、APEv2 等标签结构均由各格式自带的 parser 直接解析字节，不依赖 FFmpeg 的 `AVDictionary`。
- **FFmpeg 只负责探测与媒体层**。FFmpeg 仅用于流探测（probe）、音频流选择、基础媒体信息（采样率、位深度、码率、时长等）以及封面解码与像素转换，通过 pkg-config 链接 `libavformat`、`libavcodec`、`libavutil`、`libswscale`。
- **PNG 封面由内嵌的 fpng 编码**。`third_party/fpng` 随仓库提供，x86 下启用 SSE4.1/PCLMUL 编译选项以获得更快编码速度。
- **输出统一为 `MusicTag`**，文本终态一律 UTF-8；歌词以 `Lyrics` 容器承载，支持带微秒级 `std::chrono` 时间戳的同步歌词与非同步歌词。
- **封面导出为内容寻址 PNG 缓存**（全尺寸图 + 缩略图），并支持同目录 sidecar 图片（如 `cover.jpg`）回退。
- 附命令行演示工具 `TagReaderTest` 与基于 Catch2 的测试套件。

## 功能特性

### 格式支持

| 格式 | 标签类型 | 说明 |
|------|----------|------|
| MP3 | ID3v1、ID3v2.2/2.3/2.4 | 帧解析、同步安全整数；MP3+APE 时以 APE 为主、ID3 补缺 |
| FLAC | Vorbis Comment + PICTURE 块 | 内嵌封面块与元数据块解析 |
| Ogg Vorbis | Vorbis Comment | Ogg 页遍历 |
| Ogg Opus | OpusTags | Opus 标签与歌词 |
| MP4 / M4A | MP4 `ilst` | atom 状态机解析；`©nam`/`©ART`/`aART`/`©alb`/`©wrt`/`©gen`/`©day`/`date`/`trkn`/`disk`/`covr` 等键；`©lyr` 与 freeform 歌词 |
| APE | APEv2 | footer 检测优先于 ID3 |
| WAV | RIFF/WAV | LIST/INFO 块 |
| AIFF / AIFC | AIFF | FORM/COMM 块 |
| DSF / DFF | DSF/DFF | 块解析，内嵌 ID3 复用 |
| WMA / ASF | ASF | GUID 对象遍历；WM/Picture 封面、WM/Lyrics 歌词 |
| Matroska / WebM / MKA | Matroska Tags | EBML 解析、Attachments `image/*` 附件封面 |

### 元数据与媒体信息

- 标题、艺术家、专辑、专辑艺术家、作曲家、流派、年份、音轨号、碟号。
- 媒体信息：采样率、位深度、比特率、声道数、时长与偏移（微秒精度）、文件修改时间。
- 局部损坏的元数据字段会被跳过或清空，不影响其余字段的读取。

### 歌词

- 同步歌词（带 `std::chrono::microseconds` 时间戳）与非同步歌词。
- ID3、FLAC、Ogg Vorbis、Opus、APE、ASF 等格式提供歌词解析入口。

### 封面处理

- 内嵌封面自动提取；内嵌不可用时回退到同目录 sidecar 图片（`cover.jpg`、`folder.jpg` 等）。
- 封面导出为内容寻址 PNG 缓存（全尺寸 + 缩略图）；缓存命中时直接复用，不重复解码或改写。
- 缩略图尺寸、缩放质量、PNG 压缩级别均可配置。
- `ExportFolderCover`：仅查找并导出文件夹自身目录中的封面图像，不读取音频标签。

### 安全设计

- 输入路径经过校验；CUE sheet 的引用文件解析拒绝绝对路径、目录逃逸（`..`）、symlink 与 CUE 自引用。
- 封面导出目录会创建、拒绝 symlink 并硬化为当前用户私有。
- 资源上限集中管理：封面编码输入与 PNG 输出各 64 MiB、单边 8192 像素、总像素 32M（`tagreader_internal::CoverDecodeLimits`）；`ReadRange` 默认 64 MiB；MP4 atom 数上限 100000；sidecar 候选上限默认 4096。

## API 用法

公共接口共 4 个头文件，位于 `include/`：

| 头文件 | 内容 |
|--------|------|
| `TagReader.hpp` | 门面 `TagReader`、`CoverProcessingOptions`、`CoverProcessingError`、`CoverErrorCode` |
| `Tag.hpp` | `MusicTag` |
| `Lyrics.hpp` | `Lyric` / `Lyrics`（chrono 时间戳同步歌词） |
| `TagReaderInternal.hpp` | 内部常量（如 `CoverDecodeLimits`），位于公共 include 目录仅供内部使用 |

### 快速上手

```cpp
#include "TagReader.hpp"
#include <iostream>

int main()
{
    // 基本读取：标签 + 默认封面导出
    // （内嵌封面不可用时自动回退到同目录 sidecar）
    MusicTag tag = TagReader::Read("/path/to/song.flac");

    std::cout << tag.title() << " - " << tag.artist() << '\n';
    std::cout << "专辑: " << tag.album() << '\n';
    std::cout << "封面: " << tag.coverPath() << '\n';
    std::cout << "缩略图: " << tag.thumbnailPath() << '\n';

    // 自定义封面处理选项 + 显式封面导出目录
    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail;
    options.thumbnailSize.width = 128;
    options.thumbnailSize.height = 128;
    options.scalingQuality = CoverProcessingOptions::ScalingQuality::Good;
    options.pngCompression = CoverProcessingOptions::PngCompressionLevel::Balanced;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;

    MusicTag tag2 = TagReader::Read("/path/to/song.mp3", "/cache/covers", options);
    std::cout << "采样率: " << tag2.sampleRate()
              << " 时长(微秒): " << tag2.duration() << '\n';
}
```

### 入口函数

`TagReader` 提供三个静态入口，各有三重重载：

```cpp
// 读取单个音频文件
static MusicTag Read(const std::filesystem::path &filePath);
static MusicTag Read(const std::filesystem::path &filePath,
                     const std::filesystem::path &coverExportDir);
static MusicTag Read(const std::filesystem::path &filePath,
                     const std::filesystem::path &coverExportDir,
                     const CoverProcessingOptions &options);

// 读取 CUE sheet，返回按轨拆分的多个 MusicTag
static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath);
static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath,
                                          const std::filesystem::path &coverExportDir);
static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath,
                                          const std::filesystem::path &coverExportDir,
                                          const CoverProcessingOptions &options);

// 仅导出文件夹封面：只查找 folderPath 自身目录中的封面图像，
// 命中时返回仅携带封面路径、无元数据的 MusicTag；
// 无候选或全部失败时返回空 MusicTag（不抛错）
static MusicTag ExportFolderCover(std::string folderPath,
                                  std::string coverExportDir,
                                  const CoverProcessingOptions &options);
```

说明：

- `Read` / `ReadCueSheet` 省略 `coverExportDir` 时，封面默认导出到 `$XDG_RUNTIME_DIR/tagreader-covers`（POSIX 回退为 `temp_directory_path()/tagreader-covers-$UID`）。默认目录会创建、拒绝 symlink 并硬化为当前用户私有；显式指定的目录同样会创建、探测读写并拒绝 symlink。
- `ReadCueSheet` 中 CUE 引用文件的解析拒绝绝对路径、目录逃逸、symlink 与 CUE 自引用。

### `CoverProcessingOptions`

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `generateThumbnail` | `true` | 是否生成缩略图 |
| `thumbnailSize.width` / `height` | `256` / `256` | 缩略图目标尺寸 |
| `thumbnailSize.maintainAspectRatio` | `true` | 是否保持宽高比 |
| `scalingQuality` | `Fast` | `Fast` / `Good` / `Best` 三档缩放质量 |
| `pngCompression` | `Fast` | `Fast`(1) / `Balanced`(3) / `Best`(6) 三档 PNG 压缩级别 |
| `mode` | `FullAndThumbnail` | `Disabled`（不输出封面）/ `ThumbnailOnly` / `FullOnly` / `FullAndThumbnail` |
| `failurePolicy` | `Propagate` | 封面失败时 `Propagate`（原样抛出 `CoverProcessingError`）或 `Ignore`（吞掉失败、清空 artwork、继续返回元数据与歌词） |
| `maxSidecarEntries` | `4096` | 单次读取检查的 sidecar 候选上限 |
| `maxSourceCoverBytes` | `64 MiB` | 内嵌封面与 sidecar 共用的一次读取源数据字节预算（0 表示完全禁止读取源封面） |

### 错误处理

封面处理失败抛出 `CoverProcessingError`（继承 `std::runtime_error`），携带类型化错误码与可选的出错路径：

| `CoverErrorCode` | 含义 |
|------------------|------|
| `ExportDirectoryUnavailable` | 封面导出目录无法创建/校验/硬化 |
| `SidecarDiscoveryFailed` | sidecar 候选发现失败 |
| `SidecarEntryLimitExceeded` | 超过 `maxSidecarEntries` 个候选 |
| `SourceReadFailed` | 源封面字节读取失败 |
| `SourceBudgetExceeded` | 累计源封面字节超过 `maxSourceCoverBytes` |
| `DecodeFailed` | 源封面数据解码失败 |
| `CacheReadFailed` | 既有缓存条目读取/校验失败 |
| `CacheWriteFailed` | 缓存条目写入失败 |
| `PublicationFailed` | 最终封面文件原子发布失败 |

注意：无内嵌也无 sidecar 封面属于"无封面"，返回空 `coverPath`，不是 `CoverProcessingError`。

### `MusicTag` 访问器

`MusicTag` 的访问器与设置器成对出现（`xxx()` / `setXxx()`），按类别列举：

| 类别 | 访问器（返回类型） |
|------|---------------------|
| 元数据 | `title()`、`genre()`、`artist()`、`album()`、`albumArtist()`、`composer()`（`const std::string&`）；`year()`、`trackNumber()`、`discNumber()`（`uint16_t`） |
| 歌词 | `lyrics()`（`const Lyrics&` / `Lyrics&`） |
| 文件信息 | `filePath()`、`coverPath()`、`thumbnailPath()`（`std::filesystem::path`）；`duration()`、`offset()`（`int64_t`，微秒）；`lastModified()`（`file_time_type`）；`sampleRate()`、`bitDepth()`、`bitRate()`（`uint32_t`）；`channels()`（`uint8_t`）；`format()`（`const std::string&`） |
| 播放统计 | `playCount()`（`uint32_t`）、`rating()`（`uint8_t`）、`lastPlayed()`（`std::chrono::system_clock::time_point`） |

### 歌词类型（`Lyrics.hpp`）

```cpp
class Lyric
{
public:
    std::chrono::microseconds timestamp() const noexcept; // 微秒级时间戳
    std::string_view text() const noexcept;
};

class Lyrics
{
public:
    const std::vector<Lyric> &lyrics() const noexcept;
    void addLyric(const Lyric &lyric);   // 也提供右值重载
    void clear() noexcept;
    bool empty() const noexcept;
    std::size_t size() const noexcept;
};
```

同步歌词的时间戳以 `std::chrono::microseconds` 表示；非同步歌词的时间戳为 `0`。

## 构建要求

### 工具链

- 支持 C++23 的编译器（GCC / Clang，MSVC 亦受支持）。
- CMake 3.21+（`CMakePresets.json` 为 version 3，推荐使用 preset 构建；`CMakeLists.txt` 的 `cmake_minimum_required` 为 3.10，但 preset 构建需要更高版本）。
- Unix Makefiles 生成器（preset 默认），Linux/macOS 类环境。

### 依赖

| 依赖 | 说明 |
|------|------|
| FFmpeg（`libavformat`、`libavcodec`、`libavutil`、`libswscale`） | 必需，经 pkg-config 解析 |
| Iconv | 默认必需；仅在显式设置 `-DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON` 时允许回退到 Latin-1 方案 |
| Catch2（v3） | 仅测试需要；默认优先系统包（`TAGREADER_USE_SYSTEM_CATCH2=ON`），缺失时 FetchContent 下载 v3.7.1，离线环境首次配置需要网络 |
| Tracy Profiler | 可选（`TAGREADER_ENABLE_PROFILING`），需要系统安装的 `TracyClient` 与 `/usr/include/Tracy` |

示例（Arch Linux）：

```bash
sudo pacman -S ffmpeg icu  # Iconv 由 glibc 提供，一般无需单独安装
```

## 构建与测试

推荐使用 preset，仓库提供 `default`、`release`、`sanitize`、`fuzz`、`profile` 五个 preset（配置/构建/测试各有同名入口）：

```bash
# 默认构建（Debug 风格，含 compile_commands.json）
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure

# 聚焦运行特定测试（正则匹配）
ctest --preset default -R 'TR-AUDIT-001|cue file resolver' --output-on-failure
```

### 其它 preset

| Preset | 用途 |
|--------|------|
| `release` | Release 构建（`-O3 -march=native -DNDEBUG`），强制关闭性能分析 |
| `sanitize` | ASAN/UBSAN 消毒器构建（`-fsanitize=address,undefined`） |
| `fuzz` | 模糊测试构建；仅 Clang/libFuzzer 下生成 `TagReaderFuzz`，并注册 corpus 生成与有界冒烟测试 |
| `profile` | `RelWithDebInfo` + Tracy 性能分析（`-O3 -march=native -DNDEBUG -g`） |

`release` 与 `profile` 的编译参数包含 `-march=native`，产物只适用于本机，不可跨机器分发。

### 测试覆盖

| 类别 | 说明 |
|------|------|
| Catch2 冒烟测试 | `TagReaderCatch2SmokeTests`（编译冒烟用例） |
| 回归测试 | TR-AUDIT-001 至 TR-AUDIT-056 编号用例 |
| 封面处理 | 封面处理契约（`CoverContract:`）、模式矩阵、sidecar、文件夹封面（`TagReaderFolderCover.` 前缀） |
| CUE | 解析、路径安全、映射、计时、封面选项 |
| 其它 | 歌词归一化复杂度、FLAC 损坏元数据、默认封面导出目录 |
| 安全测试 | `TagReaderSecurityGenerateSamples`（Python 生成样本）与 `TagReaderSecuritySmoke`；缺少 ffmpeg CLI 或编解码器导致无样本时返回 77，CTest 记为 skip |

另有手动命令行工具 `TagReaderTest`（非 CTest 目标）：

```bash
./build/default/TagReaderTest <audio-file-path> [cover-export-dir]
```

## 目录结构

```
TagReader/
├── include/                 # 公共头文件（4 个）
│   ├── TagReader.hpp        #   门面：Read / ReadCueSheet / ExportFolderCover 及封面选项与错误类型
│   ├── Tag.hpp              #   MusicTag
│   ├── Lyrics.hpp           #   Lyric / Lyrics
│   └── TagReaderInternal.hpp#   内部常量（CoverDecodeLimits 等）
├── src/
│   ├── TagReader.cpp        # 门面转发层：Read → tagreader_core::ReadTag；ReadCueSheet → tagreader_cue::ReadCueSheet
│   ├── common/              # 通用解析辅助（ParseHelpers.hpp）
│   ├── core/                # 核心读取管线（TagPipeline、ReadContext、RawTagData、TagFormat 等）
│   ├── media/               # FFmpeg 会话、容器探测、媒体信息读取
│   ├── io/                  # 文件输入与字节读取（ByteReader）
│   ├── text/                # 文本编解码与归一化（TextCodec、TextNormalize）
│   ├── cover/               # 封面缓存、解码与 sidecar（CoverCache、CoverDecoder、SidecarCover）
│   ├── formats/             # 各格式原始字节 parser（aiff/ape/asf/cue/dsd/flac/id3/matroska/mp4/
│   │                        #   ogg-vorbis/opus/riff/vorbis + common/BoundedReader）
│   └── profiling/           # Tracy 性能分析支持（TracyClient.cpp）
├── test/                    # Catch2 测试（catch2/、regression/、security/、fuzz/）、main.cpp、CMakeLists.txt
├── third_party/             # 内嵌第三方代码：fpng（PNG 编码）
├── CMakeLists.txt
├── CMakePresets.json
└── docs/                    # 设计文档与历史分析（DESIGN.md 等）
```

## 许可证

本项目以 GPL-3.0 许可证发布，详见仓库根目录的 [LICENSE](LICENSE) 文件。
