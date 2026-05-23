# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描、读取或推断上级目录内容。
- 所有面向用户的回答和仓库文档修改都使用中文。
- 先读根目录 `DESIGN.md`、`TASKS.md`、`BUGS.md`；这是延续中的修复项目，不要按全新项目重新定方向。
- `README.md` 只有标题；真实行为以 `CMakeLists.txt`、`include/TagReader.hpp`、`src/TagReader.cpp`、`test/main.cpp` 为准。
- 这是轻量 C++23 音乐元数据读取库；不要引入 TagLib 这类标签库，除非任务明确改变依赖策略。

## 架构

- 对外入口固定为 `TagReader::Read(const std::filesystem::path&)`；当前流程是 `ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `NormalizeMetadata()`/`NormalizeLyrics()` -> `BuildMusicTag()`。
- 公共头在 `include/`：`Lyrics.hpp` 放歌词类型，`Tag.hpp` 放 `MusicTag`，`TagReader.hpp` 放读取入口和内部接口声明。
- `MusicTag` 对外字段模型已定，文本字段最终必须是 UTF-8；不要把解析中间状态塞进 `MusicTag`。
- 实现集中在 `src/TagReader.cpp`；新增解析细节优先进入现有格式小函数或同区域新小函数。

## 解析约束

- FFmpeg 只用于 probe、容器识别、主音频流选择、基础媒体信息，以及封面图像解码/PNG 编码；标题、歌手、专辑、歌词、封面块等标签必须直接读文件原始字节解析。
- `ReadMetadata()` 和 `ReadLyrics()` 只能做分发；不要把具体格式解析代码或大段 lambda 塞回入口。
- `ReadContext` 同时保存 `std::ifstream input` 和 `AVFormatContext`；标签/歌词解析优先消费 `input` 的原始字节，不能用 `AVDictionary` 当元数据来源。
- 元数据解析按格式维护：ID3v1/ID3v2、Vorbis/FLAC、Ogg Vorbis、MP4 atom；歌词解析也保持 ID3、Vorbis、MP4 等分支函数。
- 封面块在 ID3 `PIC/APIC`、FLAC `PICTURE`、MP4 `covr` 等格式分支中解析；导出入口集中到 PNG 输出，不要新增会覆盖已解析封面的通用兜底。
- 当前任务计划要求本地无音频样本时自行构造最小样本，再用 `TagReaderTest` 验证。

## 构建验证

- 配置：`cmake -S . -B build`。
- 构建：`cmake --build build`；每个修复阶段完成后都运行。
- 手动验收：`./build/TagReaderTest <audio-file-path>`；它是字段打印程序，不是单元测试框架，会打印 `lyricsCount` 和逐行歌词。
- CMake 目标：静态库 `TagReaderCore`，验证程序 `TagReaderTest`。
- 依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`、`libswscale`；可选 `Iconv` 会定义 `TAGREADER_HAS_ICONV=1`。
- 仓库没有配置 lint、formatter、CI workflow 或测试框架；不要声称已运行这些不存在的检查。
