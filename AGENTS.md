# TagReader Agent Notes

- 先读项目根目录的 `session-ses_2244.md`，这是延续中的 session，不是从零开始；当前 session 应在此基础上继续。
- 这是一个 C++23 音乐标签读取库；保持实现轻量、直接、高性能，不要引入 `TagLib`。
- 公共头文件职责不要混：`include/Tag.hpp` 只放音乐标签数据存储，`include/Lyrics.hpp` 只放歌词存储，`include/TagReader.hpp` 只放读取流程与内部读取接口。
- `MusicTag` 的字段模型已经在 `include/Tag.hpp` 定下；除非任务明确要求，否则不要重构这套公共数据模型。
- 当前唯一库实现文件是 `src/TagReader.cpp`，静态库目标是 `TagReaderCore`；测试/手动验证入口是 `test/main.cpp` 生成的 `TagReaderTest`。
- 构建以 CMake + FFmpeg pkg-config 为准。已验证可用的本地命令是 `cmake --build build`。如果需要从头配置，再用 `cmake -S . -B build`。
- 构建依赖 `libavformat`、`libavcodec`、`libavutil`；不要假设仓库内置了其他标签库或测试框架。
- `src/TagReader.cpp` 现在已经打通 `TagReader::Read()` 主流程：路径校验、FFmpeg 打开与探测、音频流识别、媒体信息读取、元数据读取、歌词读取、UTF-8 归一化、`BuildMusicTag()` 组装都已接上；后续重点是补边界格式、提升正确性和收紧失败策略，而不是继续补骨架。
- 当前源码里元数据读取已经包含直接文件解析逻辑（如 ID3/FLAC/MP4/Vorbis 路径）以及封面导出到系统临时目录；封面文件名按“音频文件同名 + 图片扩展名”生成。修改这部分时优先延续现有按格式分发的小函数结构，不要把格式细节重新塞回一个大函数。
- `ExtractCoverToTempFile()` 目前是空实现；实际封面提取分散在 ID3 APIC、FLAC picture、MP4 `covr` 的格式专有函数里。若要统一封面逻辑，先确认不会重复覆盖现有格式分支。
- `ReadContext` 里同时保留 `std::ifstream` 和 `AVFormatContext`；凡是与音频流无关的 tag/歌词块，优先走文件字节读取通道，FFmpeg 主要用于 probe、流信息和基础媒体信息。
- `test/main.cpp` 不是单元测试框架，只是命令行验收程序：`./build/TagReaderTest <audio-file-path>`。它会打印各字段，适合验证默认值、封面路径和歌词条目数。
- `README.md` 基本为空；行为与架构以 `CMakeLists.txt`、`include/TagReader.hpp`、`src/TagReader.cpp` 为准，补充设计意图再参考 `DESIGN.md` 和 `TASK.md`。
