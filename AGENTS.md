# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 所有面向用户的回答和仓库文档修改都使用中文。
- `README.md` 只有标题；优先读 `DESIGN.md`、`CMakeLists.txt`、`include/TagReader.hpp`、`src/TagReader.cpp`、`test/main.cpp`。
- 这是轻量 C++23 音乐元数据读取库；不要引入 TagLib 等标签库，除非任务明确改变依赖策略。

## 架构

- 对外入口是 `TagReader::Read(path)` 和可选封面导出目录的 `TagReader::Read(path, coverExportDir)`。
- 当前主流程：`ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- 公共头在 `include/`：`Lyrics.hpp` 放歌词类型，`Tag.hpp` 放 `MusicTag`，`TagReader.hpp` 放读取入口和内部接口声明。
- `MusicTag` 文本字段最终必须是 UTF-8；解析中间状态留在 `RawMediaInfo`、`RawMetadata`、`RawLyrics` 等内部结构，不要塞进 `MusicTag`。
- 实现集中在 `src/TagReader.cpp`；新增格式细节优先放进现有格式小函数或同区域新小函数。

## 解析约束

- FFmpeg 用于 probe、容器识别、主音频流、基础媒体信息，以及封面图像解码/PNG 编码；标题、歌手、专辑、歌词、封面块等标签必须直接读文件原始字节解析。
- `ReadContext` 同时保存 `std::ifstream input` 和 `AVFormatContext`；标签/歌词解析优先消费 `input`，不要用 `AVDictionary` 当元数据来源。
- `ReadMetadata()` 和 `ReadLyrics()` 只做容器分发和容错；不要把具体格式解析代码或大段 lambda 塞回入口。
- 元数据按 ID3v1/ID3v2、Vorbis/FLAC、Ogg Vorbis、MP4 atom 分支维护；歌词也保持 ID3、Vorbis、MP4 分支。
- 封面块在 ID3 `PIC/APIC`、FLAC `PICTURE`、MP4 `covr` 等格式分支中解析；只有传入 `coverExportDir` 时才导出 PNG。

## 构建验证

- 普通配置和构建：`cmake -S . -B build`，然后 `cmake --build build`。
- 目标：静态库 `TagReaderCore`，人工验收程序 `TagReaderTest`，安全 smoke 程序 `TagReaderSecuritySmoke`；`TAGREADER_ENABLE_FUZZING=ON` 且使用 Clang 时才生成 `TagReaderFuzz`。
- 依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`、`libswscale`；可选 `Iconv` 会定义 `TAGREADER_HAS_ICONV=1`。
- `TagReaderTest` 是字段打印程序，不是单元测试框架：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`。
- 安全 smoke 用法：`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [audio-file-path ...]`。
- ASAN/UBSAN 构建：`cmake -S . -B build-asan -DTAGREADER_ENABLE_SANITIZERS=ON`，然后 `cmake --build build-asan`。
- fuzz corpus 由脚本生成，不提交二进制 seed：`python3 test/corpus/generate_corpus.py`，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`。
- fuzz 构建示例：`cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON`，然后 `cmake --build build-fuzz-clang`。
- 仓库没有配置 lint、formatter、CI workflow 或测试框架；不要声称已运行这些不存在的检查。
