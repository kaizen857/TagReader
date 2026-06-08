# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 面向用户的回答和文档修改使用中文；面向用户的仓库文档除 `AGENTS.md` 外放在 `docs/`。
- 这是 C++23 项目（`CMAKE_CXX_STANDARD 23` 且 `CMAKE_CXX_STANDARD_REQUIRED ON`），不要把实现降到 C++17/C++20 写法。
- `README.md` 只有标题；架构事实优先看 `CMakeLists.txt`、公共头文件、`src/` 实现、`test/` 用法和 `docs/DESIGN.md`。

## 入口与边界

- 对外入口只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`；`src/TagReader.cpp` 只转发到 `tagreader_core::ReadTag()`。
- `Read()` 主流程在 `src/core/TagPipeline.cpp`：`ValidatePath()` -> `OpenContext()` -> `ValidateCoverExportDir()` -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()`（实现在 `src/media/ContainerDetector.cpp`）-> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`；不要再写独立 `DetectContainer()` 步骤。
- `FFmpeg` 负责 probe、音频流、基础媒体信息和封面解码/PNG 编码；标题、歌手、专辑、歌词和封面块必须从文件原始字节解析，不要改成依赖 `AVDictionary`。
- `ReadContext` 同时持有共享 `std::ifstream input` 和 `AVFormatContext`；parser 使用绝对 offset，并通过 `ReadRange()` 读取二进制数据，避免直接操作 stream 状态。
- 返回的 `MusicTag` 文本必须是 UTF-8；中间态保存在 `RawMediaInfo`、`RawMetadata`、`RawLyrics`，最后由 `NormalizeMetadata()`/`NormalizeLyrics()` 规范化。

## 格式分支

- `src/formats/` 按格式分包：`id3/`、`vorbis/`、`flac/`、`ogg-vorbis/`、`mp4/`、`ape/`；新增格式细节放在对应 parser 附近。
- 当前支持 ID3v1/v2.2/v2.3/v2.4、Vorbis Comment（FLAC/Ogg Vorbis）、MP4 `ilst` 和 APEv2；`DetectTagFormat()` 中 APE footer 优先于 ID3，MP3+APE 会用 APE 主字段并用 ID3 补缺。
- 元数据和歌词分支保持按 `TagFormat` 分发；局部 malformed tag 应跳过或清空对应局部结果，顶层媒体不可用才让 `Read()` 失败。
- 修改解析逻辑时同步检查对应资源上限：ID3 tag 16 MiB、APE tag 16 MiB/4096 items/单项 1 MiB、Vorbis comments 4096、Ogg 扫描 64 MiB/100000 pages、MP4 atoms 100000/payload 64 MiB、封面输入/输出 64 MiB。

## 封面与副作用

- 只有调用 `Read(path, coverExportDir)` 才允许写封面文件；非封面路径不要产生文件系统副作用。
- 封面缓存是 content-addressed PNG storage，路径为 `coverExportDir / first2hex / rest.png`；已有缓存直接复用，不重复解码或重写。
- 缓存 key 基于内嵌图片原始字节；缓存污染或封面缓存错误要保留 `cover cache` 相关失败信号。

## 构建与验证

- 普通构建：`cmake -S . -B build`，再 `cmake --build build`。
- Sanitizer 构建：`cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON`，再 `cmake --build build-sanitize`；只对 Clang/GNU 配置 ASAN/UBSAN。
- Fuzz 构建：`cmake -S . -B build-fuzz -DTAGREADER_ENABLE_FUZZING=ON`，再 `cmake --build build-fuzz`；`TagReaderFuzz` 需要 Clang/libFuzzer。
- 可执行目标：`TagReaderTest` 字段打印，`TagReaderSecuritySmoke` 封面缓存 smoke，`TagReaderRegressionTests` 回归程序，`TagReaderFuzz` 仅 fuzz 构建生成。
- 用法容易猜错：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`；`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [...]`；`./build/TagReaderRegressionTests --list|<TR-AUDIT-case-id>`。
- fuzz corpus：`python3 test/corpus/generate_corpus.py [--out-dir DIR]`，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`，仓库不提交二进制 seed。
- security smoke 样本：`python3 test/security/generate_samples.py`，默认输出 `/tmp/opencode/tagreader_security_samples`；会调用 `ffmpeg` CLI，缺失时跳过音频样本。
- 依赖由 `pkg-config` 查找 FFmpeg（`libavformat`、`libavcodec`、`libavutil`、`libswscale`）；`Iconv` 可选。仓库没有 CI workflow、lint、formatter 或单元测试框架，不要声称跑过这些检查。
