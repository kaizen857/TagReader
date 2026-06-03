# TagReader Agent Notes

- 当前目录就是项目根目录，不要扫描、读取或推断上级目录内容。
- 仓库内面向用户的文档除了 `AGENTS.md` 外都放在 `docs/`；当前高信号设计说明是 `docs/DESIGN.md`，`README.md` 仅有标题。
- 所有面向用户的回答和文档修改都使用中文。
- **C++23 项目**（`CMAKE_CXX_STANDARD 23`，`CMAKE_CXX_STANDARD_REQUIRED ON`），不要用 C++17/C++20 特性替代。

## 先看这些文件

- `CMakeLists.txt`：真实构建目标、依赖和编译开关的唯一来源。
- `include/TagReader.hpp`、`include/Tag.hpp`、`include/Lyrics.hpp`：公共 API 和数据结构边界。
- `src/TagReader.cpp`：解析实现集中地；新增格式细节优先放在现有格式小函数或其相邻位置。
- `test/main.cpp`、`test/security/security_smoke.cpp`：人工验收程序和安全 smoke 程序的真实用法。
- `docs/DESIGN.md`：当前分工、失败策略、封面缓存和构建资产约束。
- `ANALYSIS.md`：第三方安全审计报告，含已知 bug、结构弱点和 fuzz 建议。**注意这是审计快照，部分 bug 可能已修复；作为参考而非权威**。`MusicTag` 当前使用普通 `std::string`（非 `boost::flyweight`），审计中的 TR-AUDIT-015 可能已过时。

## 内部源文件结构

`src/` 按功能分包，新增代码应放入对应目录：

| 目录 | 职责 | 入口文件 |
|---|---|---|
| `src/core/` | 主流程编排、`ReadContext`、中间数据结构 | `TagPipeline.cpp` |
| `src/media/` | FFmpeg 会话管理、容器探测、媒体信息读取 | `FfmpegSession.cpp`, `ContainerDetector.cpp` |
| `src/formats/id3/` | ID3v1/v2.2/v2.3/v2.4 帧解析 | `Id3Parser.cpp`, `Id3Frames.cpp` |
| `src/formats/vorbis/` | Vorbis Comment 通用解析 | `VorbisCommentParser.cpp` |
| `src/formats/flac/` | FLAC metadata block + PICTURE | `FlacParser.cpp` |
| `src/formats/ogg-vorbis/` | Ogg page/packet + Vorbis packet | `OggVorbisParser.cpp` |
| `src/formats/mp4/` | MP4 atom walker + `ilst` 解析 | `Mp4AtomReader.cpp`, `Mp4Parser.cpp` |
| `src/io/` | 统一二进制读取 helper `ReadRange()` | `ByteReader.cpp` |
| `src/text/` | 编码转换、文本规范化、LRC 解析 | `TextCodec.cpp`, `TextNormalize.cpp` |
| `src/cover/` | 封面解码/PNG 编码、content-addressed 缓存 | `CoverDecoder.cpp`, `CoverCache.cpp` |

## 代码边界

- 对外入口只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`，入口实现直接转发到 `tagreader_core::ReadTag()`。
- 主流程固定为 `ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- `FFmpeg` 只负责 probe、容器识别、主音频流、基础媒体信息，以及封面解码/PNG 编码；标题、歌手、专辑、歌词、封面块要直接读文件原始字节解析，不要把 `AVDictionary` 当元数据来源。
- `ReadContext` 是**共享可变对象**，同时保存 `std::ifstream input` 和 `AVFormatContext`；标签和歌词解析优先消费 `input`。所有 parser 复用同一个 stream，`ReadRange()` 会反复 `clear()`/`seekg()`，parser 应使用绝对 offset。
- 二进制读取必须通过 `ReadRange(std::ifstream&, offset, size, maxSize)` 进行，不要在 parser 中直接操作 stream。
- 元数据按 `ID3v1/ID3v2`、`Vorbis/FLAC`、`Ogg Vorbis`、`MP4 atom` 分支维护，歌词也保持 `ID3`、`Vorbis`、`MP4` 分支。
- `MusicTag` 最终文本字段必须是 UTF-8；中间态留在 `RawMediaInfo`、`RawMetadata`、`RawLyrics` 等内部结构，由 `NormalizeMetadata()`/`NormalizeLyrics()` 在写入前校验。
- 失败策略："顶层媒体不可用则抛错，局部 tag malformed 则跳过"。`ReadMetadata()` 吞掉 parser 的 `runtime_error`（封面相关错误除外），`ReadLyrics()` 吞掉错误返回空歌词。

## 关键资源上限

修改解析逻辑时注意这些硬编码上限（分散在各 parser 中）：

- ID3 tag 上限、MP4 atom 数上限（`kMaxMp4Atoms = 100000`）、MP4 payload 上限
- Vorbis Comment 数上限：`kMaxVorbisComments = 4096`（`src/formats/vorbis/VorbisCommentLimits.hpp`）
- Ogg 扫描上限：64 MiB 扫描字节、100000 页上限
- 封面解码上限（`include/TagReaderInternal.hpp`）：输入 64 MiB、像素 32M、宽高各 8192、输出 64 MiB
- LRC 歌词行数上限

## 封面与副作用

- 只有传入 `coverExportDir` 才导出 PNG；非封面路径不要产生文件系统副作用。
- 封面缓存是 content-addressed PNG 存储，缓存路径格式为 `coverExportDir / first2hex / rest.png`；已存在路径应直接复用，不要重复解码或重写。
- 缓存 key 基于内嵌图片原始字节计算；封面损坏或缓存被污染时抛 `cover cache` 相关错误。

## 构建与验证

- 普通构建：`cmake -S . -B build && cmake --build build`。
- Sanitizer 构建：`cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize`（需要 Clang 或 GCC）。
- Fuzz 构建：`cmake -S . -B build-fuzz -DTAGREADER_ENABLE_FUZZING=ON && cmake --build build-fuzz`（需要 Clang）。
- 目标：静态库 `TagReaderCore`，字段打印程序 `TagReaderTest`，安全 smoke 程序 `TagReaderSecuritySmoke`，回归测试程序 `TagReaderRegressionTests`，fuzz 目标 `TagReaderFuzz`（仅 fuzz 构建）。
- `TagReaderTest` 用法：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`。
- `TagReaderSecuritySmoke` 用法：`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [audio-file-path ...]`。**注意 cover-export-dir 是第一个参数，不是最后一个**。
- `TagReaderRegressionTests` 用法：`./build/TagReaderRegressionTests`。
- `TAGREADER_ENABLE_SANITIZERS=ON` 只在 Clang/GNU 下生效；`TAGREADER_ENABLE_FUZZING=ON` 需要 Clang，且只会生成 `TagReaderFuzz`。
- 依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`、`libswscale`；`Iconv` 可选。
- fuzz corpus 由 `python3 test/corpus/generate_corpus.py` 生成，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`，仓库不提交二进制 seed。
- 仓库没有配置 lint、formatter、CI workflow 或单元测试框架；不要声称跑过这些不存在的检查。

## 修改习惯

- 修改解析逻辑前，先确认对应格式分支和当前支持字段，不要凭印象扩展支持面。
- 如果需要更完整的架构背景，优先看 `docs/DESIGN.md`；安全/鲁棒性问题可参考 `ANALYSIS.md`（注意审计快照可能部分过时）。不要把推测性分析写回 AGENTS.md。
