# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 面向用户的回答和仓库文档修改使用中文；面向用户的文档除 `AGENTS.md` 外放在 `docs/`。
- `README.md` 只有标题；查事实优先看 `CMakeLists.txt`、`CMakePresets.json`、公共头文件、`src/`、`test/`，再看 `docs/DESIGN.md`。若文档和代码/构建脚本冲突，信可执行来源。
- 仓库目前没有 CI、lint、formatter、repo-local OpenCode 配置或其它指令文件；不要编造这些入口。
- 这是 C++23 项目（`CMAKE_CXX_STANDARD 23` 且 required）；不要把实现降到 C++17/C++20 写法。

## API 与主流程

- Public facade 在 `include/TagReader.hpp`：`TagReader::Read(path)`、`Read(path, coverExportDir)`、`ReadCueSheet(path)`、`ReadCueSheet(path, coverExportDir)`。
- `src/TagReader.cpp` 只做转发：单文件读取到 `tagreader_core::ReadTag()`，CUE 读取到 `tagreader_cue::ReadCueSheet()`；不要把 CUE 写成未实现。
- `ReadTag()` 主流程在 `src/core/TagPipeline.cpp`：`ValidatePath()` -> `OpenContext()` -> 封面目录校验/硬化 -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`；不要新增独立 `DetectContainer()` 步骤。
- FFmpeg 只负责 probe、音频流、基础媒体信息和封面解码/PNG 编码；标题、歌手、专辑、歌词和封面块必须从文件原始字节解析，不要改成依赖 `AVDictionary`。
- Parser 共用 `ReadContext::input` 和 `AVFormatContext`；二进制读取用绝对 offset + `ReadRange()`/bounded reader，避免依赖或污染 stream 状态。
- `MusicTag` 文本字段必须是 UTF-8；中间态走 `RawMediaInfo`、`RawMetadata`、`RawLyrics`，最后由 `NormalizeMetadata()`/`NormalizeLyrics()` 规范化。

## 格式与失败边界

- 当前 parser/分发路径覆盖 ID3v1/v2.2/v2.3/v2.4、FLAC/Ogg Vorbis Comment、Ogg OpusTags、MP4 `ilst`、APEv2、RIFF/WAV、AIFF/AIFC、DSF、DFF、ASF/WMA、Matroska/WebM/MKA、CUE；不要把 `docs/DESIGN.md` 的路线图写成当前能力。
- `src/formats/` 按格式分包；新增格式细节放在对应 parser 附近，CUE 相关代码在 `src/formats/cue/`。
- `DetectTagFormat()` 中 APE footer 优先于 ID3；MP3+APE 使用 APE 主字段，并用 ID3v2/ID3v1 补缺。
- 元数据和歌词按 `TagFormat` 分发；局部 malformed 字段、歌词或封面应跳过/清空局部结果，只有输入不可用、无音频流或容器无法建立才让 `Read()` 顶层失败。`cover export`/`cover cache` 错误会继续向外抛，别吞掉。
- 改解析逻辑时同步检查资源上限：通用 `ReadRange()` 64 MiB；ID3/APE/RIFF/AIFF/DSD 文本或 tag 多为 16 MiB 级；Ogg/Opus、MP4、ASF、Matroska 有 64 MiB/对象数/深度等本地上限，先查对应 parser 常量再改。

## 封面副作用

- `Read(path)` 也会导出封面：若文件本身没有可用内嵌封面，则会在同目录尝试 sidecar 图片（`cover/front/folder/album/artwork`），并优先导出到 `XDG_RUNTIME_DIR/tagreader-covers`，否则回退到用户私有临时目录（POSIX 为 `temp_directory_path()/tagreader-covers-$UID`）。默认目录会被创建、拒绝 symlink 并硬化为当前用户私有。
- `Read(path, coverExportDir)` 会创建并探测调用方目录；显式目录 symlink 会被拒绝。sidecar fallback 与内嵌封面导出共用同一 `coverExportDir`。
- 封面缓存是 content-addressed PNG storage，key 基于内嵌图片原始字节；sidecar 图片经解码后同样写入 `coverExportDir / first2hex / rest.png`。已有缓存直接复用，不重复解码或重写。

## 构建与验证

- 依赖由 `pkg-config` 查找 FFmpeg（`libavformat`、`libavcodec`、`libavutil`、`libswscale`）；`Iconv` 默认必需，除非显式 `-DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON`。
- 默认入口：`cmake --preset default`、`cmake --build --preset default`、`ctest --preset default --output-on-failure`；`clangd` 读取 `build/default/compile_commands.json`。
- Sanitizer / fuzz 入口：`cmake --preset sanitize`、`cmake --build --preset sanitize`、`ctest --preset sanitize --output-on-failure`；`cmake --preset fuzz`、`cmake --build --preset fuzz`。`TagReaderFuzz` 只在 Clang/libFuzzer 下生成。
- 单测/回归测试走 Catch2 + CTest；不要要求用 `TagReaderTest` 替代 `ctest`。`TagReaderTest` 是人工验收 CLI：`./build/default/TagReaderTest <audio-file-path> [cover-export-dir]`。
- `TagReaderSecuritySmoke` 由 CTest 包装；样本由 `python3 test/security/generate_samples.py` 生成，默认输出 `/tmp/opencode/tagreader_security_samples`，缺少 `ffmpeg` CLI 或 codec 时相关样本可能跳过或失败。
- fuzz corpus 由 `python3 test/corpus/generate_corpus.py [--out-dir DIR]` 生成，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`；仓库不提交二进制 seed。
