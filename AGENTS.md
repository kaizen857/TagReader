# TagReader Agent Notes

- 当前目录就是项目根目录；不要扫描或推断上级目录内容。
- 先读根目录 `session-ses_2244.md`、`BUGS.md`、`TASK.md`。这是延续中的修复项目，不要按全新项目重新定方向。
- 这是轻量 C++23 音乐元数据读取库；不要引入 `TagLib` 这类标签库，除非任务明确改变依赖策略。

## Sources

- `README.md` 只有标题；行为和命令优先信 `CMakeLists.txt`、`include/TagReader.hpp`、`src/TagReader.cpp`、`test/main.cpp`。
- `DESIGN.md` 约束架构：FFmpeg 只用于 probe、容器识别、音频流和基础媒体信息；歌名、歌手、专辑、歌词、封面等标签必须直接读文件原始字节解析。
- `BUGS.md` 是当前已确认缺陷清单；`TASK.md` 给出修复顺序，优先 ID3v2 协议级问题，再 ID3v1，再 FLAC，再空封面兜底。

## Layout

- 公共头固定在 `include/`：`Lyrics.hpp` 只放歌词类型，`Tag.hpp` 只放 `MusicTag` 数据模型，`TagReader.hpp` 只放读取入口和内部接口。
- `MusicTag` 公共字段模型已定，除非任务明确要求，不要重构对外数据模型或把中间解析状态塞进它。
- 实现集中在 `src/TagReader.cpp`；主入口是 `TagReader::Read(const std::filesystem::path&)`。

## Build And Verify

- 目标以 `CMakeLists.txt` 为准：静态库 `TagReaderCore`，手动验证程序 `TagReaderTest`。
- 常规构建：`cmake --build build`。
- 如果 `build/` 不存在：`cmake -S . -B build`，再 `cmake --build build`。
- 手动验收：`./build/TagReaderTest <audio-file-path>`；它只打印字段，不是单元测试框架。
- 依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`。仓库没有配置 lint、format、CI 或测试框架。

## Parser Rules

- `ReadMetadata()` 和 `ReadLyrics()` 应保持分发函数；不要把具体格式解析代码或大段 lambda 塞回入口。
- 元数据解析按格式小函数维护：ID3v1/ID3v2、Vorbis/FLAC、Ogg Vorbis、MP4 atom。新增格式细节应进入对应小函数或新小函数。
- `ReadContext` 同时保留 `std::ifstream` 和 `AVFormatContext`；标签和歌词优先从 `std::ifstream` 读取原始字节，FFmpeg 不作为标签字段来源。
- 封面提取已在 ID3 APIC、FLAC PICTURE、MP4 `covr` 分支各自落盘；不要新增会覆盖这些结果的通用兜底。`ExtractCoverToTempFile()` 当前是空实现。
- 当前封面文件按音频文件 stem 加图片扩展名写到系统临时目录，可能被同名音频覆盖。