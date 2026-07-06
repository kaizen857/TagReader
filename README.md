# TagReader

高性能 C++23 音频元数据读取库，支持多种音频格式、封面提取与缩略图生成。

## 特性

### 元数据读取
- 支持 10+ 种音频格式（MP3, FLAC, MP4, Ogg, APE, WAV, AIFF, DSF/DFF, WMA, Matroska）
- 完整的标签支持（ID3v1/v2, Vorbis Comments, MP4 atoms, APEv2）
- 歌词提取（同步与非同步）
- 媒体信息（采样率、位深度、码率、时长）

### 封面处理
- 嵌入封面自动提取
- Sidecar 封面支持（cover.jpg/png/folder.jpg 等）
- 自动缩略图生成（可配置尺寸与质量）
- 内容寻址缓存（相同封面只存储一次）
- 智能缩放（保持宽高比，小图不放大）

### 性能优化
- 基于 fpng 的快速 PNG 编码（支持 SSE4.1 与 PCLMUL）
- 并行封面编码（原图与缩略图）
- 缓存命中快速返回
- 最小化内存分配与拷贝

## 快速开始

### 基本用法

```cpp
#include "TagReader.hpp"

// 读取标签，封面导出到指定目录
MusicTag tag = TagReader::Read("/path/to/song.mp3", "/cache/covers");

std::cout << "Title: " << tag.title() << std::endl;
std::cout << "Artist: " << tag.artist() << std::endl;
std::cout << "Album: " << tag.album() << std::endl;
std::cout << "Cover: " << tag.coverPath() << std::endl;
```

### 生成缩略图

```cpp
#include "TagReader.hpp"

// 配置封面处理选项
CoverProcessingOptions options;
options.generateThumbnail = true;
options.thumbnailSize.width = 256;
options.thumbnailSize.height = 256;
options.thumbnailSize.maintainAspectRatio = true;
options.scalingQuality = CoverProcessingOptions::ScalingQuality::High;

MusicTag tag = TagReader::Read("/path/to/song.mp3", "/cache/covers", options);

std::cout << "Full cover: " << tag.coverPath() << std::endl;
std::cout << "Thumbnail: " << tag.thumbnailPath() << std::endl;
```

### CUE Sheet 支持

```cpp
#include "TagReader.hpp"

// 解析 CUE sheet，返回多个虚拟轨道
std::vector<MusicTag> tracks = TagReader::ReadCueSheet("/path/to/album.cue", "/cache/covers");

for (const auto& track : tracks) {
    std::cout << "Track " << track.trackNumber() << ": " << track.title() << std::endl;
}
```

## 支持的格式

| 格式 | 扩展名 | 标签类型 | 封面 | 歌词 |
|------|--------|----------|------|------|
| MP3 | .mp3 | ID3v1/v2 | ✓ | ✓ |
| FLAC | .flac | Vorbis Comments | ✓ | ✓ |
| Ogg Vorbis | .ogg | Vorbis Comments | ✓ | ✓ |
| Ogg Opus | .opus | Vorbis Comments | ✓ | ✓ |
| MP4/M4A | .mp4, .m4a | MP4 atoms | ✓ | ✓ |
| APE | .ape | APEv2 | ✓ | ✓ |
| WAV | .wav | RIFF INFO | ✓ | - |
| AIFF | .aiff, .aif | ID3v2 | ✓ | - |
| DSF | .dsf | ID3v2 | ✓ | - |
| DFF | .dff | ID3v2 | ✓ | - |
| WMA | .wma, .asf | ASF | ✓ | - |
| Matroska | .mka | Matroska tags | ✓ | - |

## API 参考

### `TagReader::Read()`

```cpp
// 基础读取（无封面导出）
MusicTag Read(const std::filesystem::path& filePath);

// 读取并导出封面
MusicTag Read(const std::filesystem::path& filePath,
              const std::filesystem::path& coverExportDir);

// 读取并处理封面（支持缩略图）
MusicTag Read(const std::filesystem::path& filePath,
              const std::filesystem::path& coverExportDir,
              const CoverProcessingOptions& options);
```

### `CoverProcessingOptions`

```cpp
struct CoverProcessingOptions {
    bool generateThumbnail = false;
    
    struct ThumbnailSize {
        uint32_t width = 256;
        uint32_t height = 256;
        bool maintainAspectRatio = true;
    } thumbnailSize;
    
    enum class ScalingQuality {
        Fast,      // 快速双线性插值
        High,      // 高质量 Lanczos
        Balanced   // 平衡质量与速度
    };
    ScalingQuality scalingQuality = ScalingQuality::Balanced;
    
    enum class PngCompression {
        Fast,      // 最快编码，文件稍大
        Balanced,  // 平衡速度与大小
        Best       // 最小文件，速度较慢
    };
    PngCompression pngCompression = PngCompression::Fast;
};
```

### `MusicTag` 访问器

```cpp
// 基础元数据
std::string title() const;
std::string artist() const;
std::string album() const;
std::string albumArtist() const;
std::string genre() const;
std::string composer() const;
std::string comment() const;
uint32_t year() const;
uint32_t trackNumber() const;
uint32_t trackTotal() const;
uint32_t discNumber() const;
uint32_t discTotal() const;

// 歌词
std::string lyrics() const;
bool hasLyrics() const;

// 封面
std::filesystem::path coverPath() const;
std::filesystem::path thumbnailPath() const;
bool hasCover() const;

// 媒体信息
uint32_t sampleRate() const;
uint32_t bitDepth() const;
uint32_t bitrate() const;
uint32_t channels() const;
uint64_t durationMs() const;
std::string codec() const;
```

## 构建要求

### 编译器
- C++23 支持（GCC 11+, Clang 14+, MSVC 2022+）
- CMake 3.20+

### 依赖库
- **FFmpeg** (libavformat, libavcodec, libavutil, libavfilter, libswresample, libswscale)
- **libxxhash** - 快速哈希
- **spdlog** - 日志
- **SQLite3** - 可选（如果上层应用使用）

### 安装依赖（Arch Linux）
```bash
sudo pacman -S ffmpeg xxhash spdlog sqlite
```

### 安装依赖（Ubuntu/Debian）
```bash
sudo apt install libavformat-dev libavcodec-dev libavutil-dev \
                 libavfilter-dev libswresample-dev libswscale-dev \
                 libxxhash-dev libspdlog-dev libsqlite3-dev
```

## 构建与安装

```bash
# 配置
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build -j$(nproc)

# 运行测试
ctest --test-dir build --output-on-failure

# 安装（可选）
sudo cmake --install build
```

## 性能

基于 5000+ 真实音乐文件的测试（32 线程，Intel Xeon）：

- **平均处理速度**: 2.6 ms/文件（墙上时间）
- **缩略图生成**: +1.85 ms/文件（可选）
- **缓存命中**: <0.1 ms/文件
- **并行效率**: 22x 加速（32 核）

## 架构设计

### 模块结构

```
TagReader/
├── core/           # 主管道与格式检测
├── formats/        # 格式解析器（FLAC, ID3, APE 等）
├── media/          # FFmpeg 集成与媒体信息读取
├── cover/          # 封面提取、解码与缩略图生成
├── io/             # 文件 I/O 与字节读取
└── third_party/    # 嵌入的第三方库（fpng, doctest）
```

### 缓存结构

```
/cache/covers/
├── ab/
│   └── cdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890.png  # 原图
└── thumbnails/
    └── ab/
        └── cdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890.png  # 缩略图
```

文件名为封面内容的 SHA-256 哈希，确保相同封面只存储一次。

## 测试

```bash
# 运行全部测试
ctest --test-dir build --output-on-failure

# 运行特定测试
ctest --test-dir build -R TagReaderFlac --output-on-failure

# 显示详细输出
ctest --test-dir build -V
```

测试覆盖率：
- 单元测试：89+ 个测试用例
- 集成测试：真实音频文件测试
- 性能测试：大规模扫描基准测试
