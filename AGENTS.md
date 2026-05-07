# TagReader Agent Notes

- 先读根目录 `session-ses_2244.md`。这个仓库当前是延续中的 session，不要按全新项目重新判断上下文。
- 这是一个小型 C++23 高性能音乐元数据读取库。保持实现轻量、直接，不要引入 `TagLib` 这类额外标签库。

## Layout

- 公共头文件都在 `include/`，源文件都在 `src/`。
- 头文件职责固定：`include/Lyrics.hpp` 只放歌词类型，`include/Tag.hpp` 只放音乐 tag 数据类型，`include/TagReader.hpp` 只放读取入口和内部读取接口。
- `MusicTag` 的公共字段模型已经在 `include/Tag.hpp` 定下；除非任务明确要求，不要重构这套对外数据模型。
- 当前库实现集中在 `src/TagReader.cpp`；主入口是 `TagReader::Read(const std::filesystem::path&)`。

## Build And Verify

- 可执行真相以 `CMakeLists.txt` 为准：静态库目标是 `TagReaderCore`，手动验证程序是 `TagReaderTest`，入口在 `test/main.cpp`。
- 已验证的本地构建命令是 `cmake --build build`。
- 如果 `build/` 不可用，再运行 `cmake -S . -B build`，然后再 `cmake --build build`。
- 运行手动验收程序用 `./build/TagReaderTest <audio-file-path>`。它不是单元测试框架，只会打印解析出的字段，适合核对默认值、封面路径和歌词条目数。
- 构建依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`。不要假设仓库里有别的测试框架、lint、format 或标签解析依赖。

## Codepath Notes

- `TagReader::Read()` 的主流程已经接通：路径校验、FFmpeg 打开与 probe、音频流识别、媒体信息读取、元数据读取、歌词读取、UTF-8 归一化、`BuildMusicTag()` 组装。后续工作通常应补正确性和边界，不是再搭一套新骨架。
- `ReadContext` 同时保留 `std::ifstream` 和 `AVFormatContext`。与音频流无关的 tag/歌词块优先直接读文件字节；FFmpeg 主要用于 probe、流信息和基础媒体信息。
- 现有元数据读取已按格式拆小函数，包含 ID3、Vorbis/FLAC、MP4 路径；修改时优先延续这种按格式分发的结构，不要把格式细节重新塞回一个大函数。
- 封面提取不是统一走一个中心实现：ID3 APIC、FLAC picture、MP4 `covr` 都各自直接落盘到系统临时目录。改封面逻辑时先检查是否会和这些格式分支重复覆盖。
- 当前封面文件路径按“音频文件同名 stem + 图片扩展名”生成到系统临时目录，不是随机名。

## Source Priority

- `README.md` 基本没有信息。行为、边界和可运行命令优先相信 `CMakeLists.txt`、`include/TagReader.hpp`、`src/TagReader.cpp`、`test/main.cpp`。
- `DESIGN.md` 和 `TASK.md` 主要提供设计意图与历史任务拆分；如果和当前可执行源码冲突，以源码和构建配置为准。
