# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 面向用户的回答和文档修改使用中文；面向用户的仓库文档除 `AGENTS.md` 外放在 `docs/`。
- `README.md` 只有标题；查事实优先看 `CMakeLists.txt`、`docs/DESIGN.md`、公共头文件、`src/` 和 `test/`。若文档和代码冲突，信代码和构建脚本。
- 仓库没有 CI、lint、formatter、单元测试框架、repo-local OpenCode 配置或编辑器规则；不要声称跑过这些检查。
- 这是 C++23 项目（`CMAKE_CXX_STANDARD 23` 且 required）；不要把实现降到 C++17/C++20 写法。

## 入口与架构边界

- 对外 API 只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`；`src/TagReader.cpp` 只转发到 `tagreader_core::ReadTag()`。
- `Read()` 主流程在 `src/core/TagPipeline.cpp`：`ValidatePath()` -> `OpenContext()` -> 封面目录校验/硬化 -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`；不要新增独立 `DetectContainer()` 步骤。
- FFmpeg 只负责 probe、音频流、基础媒体信息和封面解码/PNG 编码；标题、歌手、专辑、歌词和封面块必须从文件原始字节解析，不要改成依赖 `AVDictionary`。
- Parser 共用 `ReadContext::input` 和 `AVFormatContext`；二进制读取用绝对 offset + `ReadRange()`/bounded reader，避免依赖或污染 stream 状态。
- `MusicTag` 文本字段必须是 UTF-8；中间态走 `RawMediaInfo`、`RawMetadata`、`RawLyrics`，最后由 `NormalizeMetadata()`/`NormalizeLyrics()` 规范化。

## 格式、失败与资源上限

- 当前代码已有 parser/分发路径：ID3v1/v2.2/v2.3/v2.4、FLAC/Ogg Vorbis Comment、Ogg OpusTags、MP4 `ilst`、APEv2、RIFF/WAV、AIFF/AIFC、DSF、DFF、ASF/WMA、Matroska/WebM/MKA；不要把 `docs/DESIGN.md` 的“最终目标”误写成当前能力。
- `src/formats/` 按格式分包：`id3/`、`vorbis/`、`flac/`、`ogg-vorbis/`、`opus/`、`mp4/`、`ape/`、`riff/`、`aiff/`、`dsd/`、`asf/`、`matroska/`；新增格式细节放在对应 parser 附近。
- `DetectTagFormat()` 中 APE footer 优先于 ID3；MP3+APE 使用 APE 主字段，并用 ID3v2/ID3v1 补缺。
- 元数据和歌词按 `TagFormat` 分发；局部 malformed 字段、歌词或封面应跳过/清空局部结果，只有输入不可用、无音频流或容器无法建立才让 `Read()` 顶层失败。`cover export`/`cover cache` 错误会继续向外抛，别吞掉。
- 改解析逻辑时同步检查资源上限：ID3 tag 16 MiB；APE tag 16 MiB/4096 items/单项 1 MiB/封面项 8 MiB；Vorbis comments 4096；Ogg/Opus 扫描 64 MiB/100000 pages/packet 8 MiB；MP4 atoms 100000/payload 64 MiB；RIFF/AIFF chunk 100000、文本/ID3 chunk 16 MiB；DSD ID3 payload 16 MiB、DFF chunk 100000/depth 8；ASF header/object 64 MiB、objects 100000、descriptors 4096、文本 1 MiB、图片 64 MiB；Matroska scan/payload/attachment 64 MiB、text 1 MiB、elements 100000、depth 16；通用 `ReadRange()` 64 MiB。

## 封面副作用

- `Read(path)` 也会导出封面：优先 `XDG_RUNTIME_DIR/tagreader-covers`，否则回退到用户私有临时目录（POSIX 为 `temp_directory_path()/tagreader-covers-$UID`）；默认目录会被创建、拒绝 symlink 并硬化为当前用户私有。
- `Read(path, coverExportDir)` 会创建并探测调用方目录；显式目录 symlink 会被拒绝。
- 封面缓存是 content-addressed PNG storage，key 基于内嵌图片原始字节，路径为 `coverExportDir / first2hex / rest.png`；已有缓存直接复用，不重复解码或重写。
- 缓存污染或封面缓存错误要保留 `cover cache` 相关失败信号。

## 构建与验证

- 依赖由 `pkg-config` 查找 FFmpeg（`libavformat`、`libavcodec`、`libavutil`、`libswscale`）；`Iconv` 默认必需，除非显式 `-DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON`。
- 普通构建：`cmake -S . -B build`，再 `cmake --build build`；仓库没有 `enable_testing()`/`add_test()`，不要用 `ctest` 代替可执行程序验证。
- Sanitizer 构建：`cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON`，再 `cmake --build build-sanitize`；ASAN/UBSAN 只对 Clang/GNU 配置。
- Fuzz 构建：`cmake -S . -B build-fuzz -DTAGREADER_ENABLE_FUZZING=ON`，再 `cmake --build build-fuzz`；`TagReaderFuzz` 只在 Clang/libFuzzer 下生成，可用 `TAGREADER_FUZZ_ROOT` 改 fuzz 临时目录。
- 构建目标包括 `TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke`、`TagReaderRegressionTests`、`TagReaderFlacMalformedMetadataTests`、`TagReaderDefaultCoverExportDirectoryTests`、`TagReaderLyricsNormalizeComplexityTests`；fuzz 构建才有 `TagReaderFuzz`。
- 常用运行：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`；`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [...]`；`./build/TagReaderRegressionTests --list|<TR-AUDIT-case-id>`（当前 `TR-AUDIT-001` 到 `TR-AUDIT-054` 都已实现）。
- 多数回归 case 和样本脚本会现场调用 `ffmpeg` CLI；缺失或 codec 不可用时相关 case 可能跳过或失败，不要误判为 CMake 链接 FFmpeg 库问题。
- Fuzz corpus：`python3 test/corpus/generate_corpus.py [--out-dir DIR]`，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`，仓库不提交二进制 seed。
- Security smoke 样本：`python3 test/security/generate_samples.py`，默认输出 `/tmp/opencode/tagreader_security_samples`；脚本会调用 `ffmpeg` CLI，缺失时跳过音频样本。
