# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 面向用户的回答和仓库文档修改使用中文；面向用户的文档除 `AGENTS.md` 外放在 `docs/`。
- 查事实先看可执行来源：`CMakeLists.txt`、`CMakePresets.json`、公共头文件、`src/`、`test/`；`README.md` 和 `docs/DESIGN.md` 可参考，但和代码/构建脚本冲突时以后者为准。
- 仓库目前没有 CI、lint、formatter、pre-commit 或 repo-local OpenCode 配置；不要编造这些入口。
- 这是 C++23 项目（`CMAKE_CXX_STANDARD 23` 且 required）；不要把实现降到 C++17/C++20 写法。
- `.clangd` 写的是 `CompilationDatabase: build`，但 CMake presets 生成到 `build/<preset>/compile_commands.json`；不要假设 LSP 已自动连到 `build/default`。

## API 与主流程

- Public facade 在 `include/TagReader.hpp`：`Read` 和 `ReadCueSheet` 都有 `(path)`、`(path, coverExportDir)`、`(path, coverExportDir, CoverProcessingOptions)` 重载。
- `src/TagReader.cpp` 只做转发：单文件到 `tagreader_core::ReadTag()`，CUE 到 `tagreader_cue::ReadCueSheet()`；不要把 CUE 当未实现或塞进 `Read()`。
- `ReadTag()` 主流程在 `src/core/TagPipeline.cpp`：`ValidatePath()` -> `OpenContext()` -> 封面目录校验/硬化 -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> sidecar cover fallback -> `ReadLyrics()` -> `BuildMusicTag()`；不要新增独立 `DetectContainer()` 步骤。
- FFmpeg 只负责 probe、音频流、基础媒体信息和封面解码/PNG 编码；标题、歌手、专辑、歌词和封面块从原始字节 parser 读取，不要改成依赖 `AVDictionary`。
- Parser 共用 `ReadContext::input` 和 `AVFormatContext`；二进制读取用绝对 offset + `ReadRange()`/bounded reader，避免依赖或污染 stream 状态。
- `MusicTag` 文本字段必须是 UTF-8；中间态走 `RawMediaInfo`、`RawMetadata`、`RawLyrics`，最后由 `NormalizeMetadata()`/`NormalizeLyrics()` 规范化。

## 格式与失败边界

- 当前 parser/分发路径覆盖 ID3v1/v2.2/v2.3/v2.4、FLAC/Ogg Vorbis Comment、Ogg OpusTags、MP4 `ilst`、APEv2、RIFF/WAV、AIFF/AIFC、DSF、DFF、ASF/WMA、Matroska/WebM/MKA、CUE；不要把设计文档路线图写成当前能力。
- `src/formats/` 按格式分包；新增格式细节放在对应 parser 附近，CUE 代码在 `src/formats/cue/`。
- `DetectTagFormat()` 中 APE footer 优先于 ID3；MP3+APE 使用 APE 主字段，并用 ID3v2/ID3v1 补缺。
- 元数据/歌词按 `TagFormat` 分发；局部 malformed 字段、歌词或封面应跳过/清空局部结果。只有输入不可用、无音频流或容器无法建立才让 `Read()` 顶层失败；`cover export`/`cover cache` 错误会继续向外抛，别吞掉。
- 改解析逻辑时同步查资源上限：通用 `ReadRange()` 默认 64 MiB；MP4 atom payload 64 MiB 且最多 100000 atoms；其它格式先查对应 parser 常量再改。

## 封面副作用

- `Read(path)` 也会导出封面：没有可用内嵌封面时会查同目录 sidecar（`cover/front/folder/album/artwork`），并优先写 `XDG_RUNTIME_DIR/tagreader-covers`，否则 POSIX 回退到 `temp_directory_path()/tagreader-covers-$UID`。
- 默认封面目录会创建、拒绝 symlink 并硬化为当前用户私有；显式 `coverExportDir` 也会创建、探测读写并拒绝 symlink。
- 封面缓存是 content-addressed PNG storage；已有缓存直接复用，不重复解码或重写。`CoverProcessingOptions` 默认会生成缩略图。

## 构建与验证

- 默认验证顺序：`cmake --preset default` -> `cmake --build --preset default` -> `ctest --preset default --output-on-failure`。
- 聚焦单测用 CTest regex：`ctest --preset default -R <regex> --output-on-failure`；不要用人工 CLI `TagReaderTest` 替代单元/回归测试。
- Release：`cmake --preset release`、`cmake --build --preset release`、`ctest --preset release --output-on-failure`；Release 会强制关闭 profiling。
- Sanitizer：`cmake --preset sanitize`、`cmake --build --preset sanitize`、`ctest --preset sanitize --output-on-failure`。
- Fuzz：`cmake --preset fuzz`、`cmake --build --preset fuzz`；`TagReaderFuzz` 只在 Clang/libFuzzer 下生成，相关 CTest 会先跑 `test/corpus/generate_corpus.py`。
- Profile：`cmake --preset profile`、`cmake --build --preset profile`；需要系统 TracyClient 库和 `/usr/include/Tracy`，不是 pkg-config 入口。
- 依赖由 `pkg-config` 查找 FFmpeg（`libavformat`、`libavcodec`、`libavutil`、`libswscale`）；`Iconv` 默认必需，除非显式 `-DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON`。
- `TagReaderSecuritySmoke` 由 CTest 包装；样本在构建目录由 `test/security/generate_samples.py` 生成，缺少 `ffmpeg` CLI 或 codec 时可能返回 skip。
- `TagReaderTest` 只是人工验收 CLI：`./build/default/TagReaderTest <audio-file-path> [cover-export-dir]`。
