# TagReader Agent Notes

- 当前目录就是项目根目录，不要扫描、读取或推断上级目录内容。
- 仓库内面向用户的文档除了 `AGENTS.md` 外都放在 `docs/`；当前高信号设计说明是 `docs/DESIGN.md`，`README.md` 仅有标题。
- 所有面向用户的回答和文档修改都使用中文。

## 先看这些文件

- `CMakeLists.txt`：真实构建目标、依赖和编译开关的唯一来源。
- `include/TagReader.hpp`、`include/Tag.hpp`、`include/Lyrics.hpp`：公共 API 和数据结构边界。
- `src/TagReader.cpp`：解析实现集中地；新增格式细节优先放在现有格式小函数或其相邻位置。
- `test/main.cpp`、`test/security/security_smoke.cpp`：人工验收程序和安全 smoke 程序的真实用法。
- `docs/DESIGN.md`：当前分工、失败策略、封面缓存和构建资产约束。

## 代码边界

- 对外入口只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`。
- 主流程固定为 `ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- `FFmpeg` 只负责 probe、容器识别、主音频流、基础媒体信息，以及封面解码/PNG 编码；标题、歌手、专辑、歌词、封面块要直接读文件原始字节解析，不要把 `AVDictionary` 当元数据来源。
- `ReadContext` 同时保存 `std::ifstream input` 和 `AVFormatContext`；标签和歌词解析优先消费 `input`。
- 元数据按 `ID3v1/ID3v2`、`Vorbis/FLAC`、`Ogg Vorbis`、`MP4 atom` 分支维护，歌词也保持 `ID3`、`Vorbis`、`MP4` 分支。
- `MusicTag` 最终文本字段必须是 UTF-8；中间态留在 `RawMediaInfo`、`RawMetadata`、`RawLyrics` 等内部结构。

## 封面与副作用

- 只有传入 `coverExportDir` 才导出 PNG；非封面路径不要产生文件系统副作用。
- 封面缓存是 content-addressed PNG 存储，已存在路径应直接复用，不要重复解码或重写。

## 构建与验证

- 普通构建：`cmake -S . -B build`，然后 `cmake --build build`。
- 目标：静态库 `TagReaderCore`，字段打印程序 `TagReaderTest`，安全 smoke 程序 `TagReaderSecuritySmoke`。
- `TagReaderTest` 用法：`./build/TagReaderTest <audio-file-path> [cover-export-dir]`。
- `TagReaderSecuritySmoke` 用法：`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path> [audio-file-path ...]`。
- `TAGREADER_ENABLE_SANITIZERS=ON` 只在 Clang/GNU 下生效；`TAGREADER_ENABLE_FUZZING=ON` 需要 Clang，且只会生成 `TagReaderFuzz`。
- 依赖通过 `pkg-config` 查找 FFmpeg：`libavformat`、`libavcodec`、`libavutil`、`libswscale`；`Iconv` 可选。
- fuzz corpus 由 `python3 test/corpus/generate_corpus.py` 生成，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`，仓库不提交二进制 seed。
- 仓库没有配置 lint、formatter、CI workflow 或单元测试框架；不要声称跑过这些不存在的检查。

## 修改习惯

- 修改解析逻辑前，先确认对应格式分支和当前支持字段，不要凭印象扩展支持面。
- 如果需要更完整的架构背景，优先看 `docs/DESIGN.md`，不要把推测性分析写回本文件。
