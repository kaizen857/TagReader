# TagReader

<div align="center">

**现代、高性能、安全的 C++23 音频标签与内嵌封面提取库**

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-3.21%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-4%2B-007808?logo=ffmpeg&logoColor=white)](https://ffmpeg.org/)
[![Catch2](https://img.shields.io/badge/Catch2-v3-green.svg)](https://github.com/catchorg/Catch2)

[简介](#-简介) • [格式支持](#-格式支持) • [快速开始](#-快速开始) • [测试](#-测试) • [设计理念](#-设计理念) • [许可证](#-许可证)

</div>

---

## 📖 简介

**TagReader** 是专为高性能音频管理打造的现代 C++23 标签与内嵌封面解析库。

它提出**精细化分工与零拷贝解析**理念：各格式标签（ID3v1/v2, Vorbis, MP4 ilst, APEv2 等）由专属 Parser 直接解析原始二进制流，FFmpeg 仅负责底层流探测与封面解码，结合内嵌的 `fpng` 极速生成内容寻址的 PNG 封面缓存与缩略图。

---

## 🎵 格式支持

| 音频格式 / 容器 | 标签标准 | 核心支持特性 |
| :--- | :--- | :--- |
| **MP3** | ID3v1, ID3v2.2 / v2.3 / v2.4, APEv2 | 完整帧解析、同步安全整数、MP3+APE 复合解析 |
| **FLAC** | Vorbis Comment, `PICTURE` 块 | 原生元数据块遍历、高清内嵌封面无损提取 |
| **Ogg Vorbis / Opus** | Vorbis Comment / OpusTags | 物理页连续遍历、原生注释头与同步歌词 |
| **MP4 / M4A / AAC** | MP4 `ilst` (QuickTime Atom) | 递归 Atom 状态机解析、`©lyr` 双语歌词 |
| **APE** | APEv2 | 优先解析文件尾部 APE Header/Footer |
| **WAV / AIFF** | RIFF INFO / AIFF FORM | 原生 Chunk 解析，支持标准 LIST/INFO 元数据 |
| **DSF / DFF (DSD)** | DSF / DFF Chunk | 原生 DSD 块解析，内嵌 ID3 结构复用 |
| **WMA / ASF** | ASF GUID Objects | GUID 树遍历、`WM/Picture` 与 `WM/Lyrics` |
| **MKV / MKA / WebM** | Matroska EBML Tags | EBML 树解析，支持提取 `image/*` 附件封面 |
| **CUE Sheet** | CUE 索引文件 | 智能分轨解析，严格安全沙箱防护 |

---

## 🚀 快速开始

```cpp
#include "TagReader.hpp"
#include <iostream>

int main() {
    // 1. 最简读取：一行代码解析标签并导出封面
    MusicTag tag = TagReader::Read("/home/music/Hotel_California.flac");

    std::cout << "🎵 歌曲: " << tag.title() << "\n"
              << "👤 歌手: " << tag.artist() << "\n"
              << "💿 专辑: " << tag.album() << "\n"
              << "🖼️ 封面: " << tag.coverPath() << "\n";

    // 2. 遍历毫秒级对齐的同步歌词
    for (const auto& line : tag.lyrics().lyrics()) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(line.timestamp()).count();
        std::cout << "[" << ms << "ms] " << line.text() << "\n";
    }

    // 3. 解析整轨 CUE 分轨歌单
    std::vector<MusicTag> tracks = TagReader::ReadCueSheet("/home/music/Album.cue");
}
```

### 构建项目

```bash
# 默认构建（Debug 模式）
cmake --preset default
cmake --build --preset default -j$(nproc)
```

---

## 🧪 测试

```bash
# 运行完整 Catch2 测试套件
ctest --preset default --output-on-failure

# 运行特定测试用例
ctest --preset default -R 'TR-AUDIT-001' --output-on-failure
```

---

## 💡 设计理念

1. **原始字节零拷贝直接解析**：绕过重量级中间层，各格式专有二进制 Parser 直接提速。
2. **智能内容寻址封面缓存**：优先提取内嵌封面，无内嵌时自动搜寻同级 Sidecar（`cover.jpg` 等）；基于哈希命名落盘，全盘同张封面只存一份。
3. **安全沙箱化**：严格防范路径逃逸（CUE 索引安全校验，拒绝绝对路径与 Symlink 绕过），封面上限 64MB 内存预算硬约束。

---

## 📄 许可证

本项目基于 [GPL-3.0](./LICENSE) 许可证开源。
