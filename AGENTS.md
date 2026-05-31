# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 仓库内所有面向用户的回答和文档修改都使用中文。
- 先读 `DESIGN.md`、`CMakeLists.txt`、`include/TagReader.hpp`、`src/TagReader.cpp`、`test/main.cpp`；`README.md` 目前只有标题，信息量很低。
- 这是轻量 C++23 音乐元数据读取库；除非任务明确要求改依赖策略，不要引入 TagLib 等外部标签库。

## 代码结构

- 对外入口只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`。
- 主流程固定为：`ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- 公共头文件分工固定：`include/Lyrics.hpp` 放歌词类型，`include/Tag.hpp` 放 `MusicTag`，`include/TagReader.hpp` 放读取入口和内部接口声明。
- 解析实现集中在 `src/TagReader.cpp`；新增格式细节优先放到现有格式小函数或同区域新增小函数，不要把大段 lambda 塞回入口。

## 解析约束

- FFmpeg 只负责 probe、容器识别、主音频流、基础媒体信息，以及封面图像解码/PNG 编码。
- 标题、歌手、专辑、歌词、封面块等标签必须直接读文件原始字节解析，不要把 `AVDictionary` 当元数据来源。
- `ReadContext` 同时保存 `std::ifstream input` 和 `AVFormatContext`；标签和歌词解析优先消费 `input`。
- `ReadMetadata()` 和 `ReadLyrics()` 只做容器分发和容错；具体格式解析代码要留在各自的专用函数里。
- 元数据按 ID3v1/ID3v2、Vorbis/FLAC、Ogg Vorbis、MP4 atom 分支维护；歌词也保持 ID3、Vorbis、MP4 分支。
- 封面块在 ID3 `PIC/APIC`、FLAC `PICTURE`、MP4 `covr` 等分支中解析；只有传入 `coverExportDir` 时才导出 PNG。
- `MusicTag` 最终文本字段必须是 UTF-8，中间态留在 `RawMediaInfo`、`RawMetadata`、`RawLyrics` 之类内部结构。

## 构建与验证

- 普通构建：`cmake -S . -B build`，然后 `cmake --build build`。
- 目标：静态库 `TagReaderCore`，人工验收程序 `TagReaderTest`，安全 smoke 程序 `TagReaderSecuritySmoke`。
- `TagReaderTest` 不是单元测试框架，而是字段打印程序：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`。
- `TagReaderSecuritySmoke` 用法：`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [audio-file-path ...]`。
- `TAGREADER_ENABLE_SANITIZERS=ON` 只在 Clang/GNU 下生效；`TAGREADER_ENABLE_FUZZING=ON` 需要 Clang，且只会生成 `TagReaderFuzz`。
- 依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`、`libswscale`；`Iconv` 可选。
- fuzz corpus 由 `python3 test/corpus/generate_corpus.py` 生成，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`，仓库不提交二进制 seed。
- 仓库没有配置 lint、formatter、CI workflow 或单元测试框架；不要声称跑过这些不存在的检查。

## 工作习惯

- 修改解析逻辑前，先确认对应格式分支和当前支持字段，不要凭印象扩展支持面。
- 只在需要时导出封面；不要让非封面路径产生文件系统副作用。
- 如果需要更完整的架构背景，优先看 `DESIGN.md` 和 `ANALYSIS.md`，但不要把它们里的推测性风险评估写回 AGENTS。
