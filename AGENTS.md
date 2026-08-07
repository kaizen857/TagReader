# TagReader Agent 规则

## 根仓库规则

- 当前目录就是仓库根目录；不得扫描、读取或推断上级目录。`AGENTS.md` 只保留此根文件，不创建子目录版本。
- 面向用户的回答和仓库文档使用中文；除本文件外，面向用户的文档放在 `docs/`。
- 事实以 `CMakeLists.txt`、`CMakePresets.json`、公共头文件、`src/`、`test/` 等可执行来源为准；`README.md`、`docs/DESIGN.md` 冲突时不采信。`docs/DESIGN.md` 是描述长期稳定设计的架构文档（维护规范见下节），不是事实基准，也不是开发日志。
- 仓库没有 CI、独立 lint/formatter 命令、pre-commit 或 repo-local OpenCode 配置；`.vscode/settings.json` 仅通过 clangd 启用 `--clang-tidy`，不得编造其它入口。
- 项目强制 C++23，不能降为 C++17/C++20。`.clangd` 指向 `build`，presets 却生成 `build/<preset>/compile_commands.json`，不能假设 LSP 已连接 `build/default`。

## DESIGN.md 维护规范

- `docs/DESIGN.md` 是描述项目长期稳定设计的架构文档，不是开发日志、变更记录或实现细节文档；它应保持稳定，避免随项目开发逐渐演变为实现文档或变更日志。
- 任何开发任务完成后，必须将"是否需要更新 `DESIGN.md`"作为任务检查项之一（不是可选步骤）：检查本次修改是否使 `DESIGN.md` 已无法准确描述当前项目；若需要，必须在同一次任务内同步更新 `DESIGN.md`。
- 判断唯一依据：一个新开发者只读旧版 `DESIGN.md` 是否会错误理解当前项目。不要仅因修改了代码、或仅因修改涉及架构名称，就决定更新。
- 通常**不应**更新（包括但不限于）：Bug 修复、代码重构、性能优化、实现细节调整、内部接口调整、参数修改、不影响整体设计的小功能开发、代码风格调整、测试补充。确认无需更新时，不在 `DESIGN.md` 留下任何痕迹，也不为完成检查而修改文档。
- 通常**应当**更新（包括但不限于）：整体架构调整、模块新增或删除、模块职责变化、模块协作关系变化、启动流程变化、核心运行流程变化、配置体系变化、扩展机制变化、长期维护方式变化，以及其它会影响开发者理解项目整体设计的重要修改。
- 更新内容必须与当前源码/构建配置一致；`DESIGN.md` 与源码冲突时按源码修正文档，不得反向让源码迁就文档描述。

## API 与主流程

- `include/TagReader.hpp` 中 `Read` 与 `ReadCueSheet` 均有 `(path)`、`(path, coverExportDir)`、`(path, coverExportDir, CoverProcessingOptions)` 重载。
- `src/TagReader.cpp` 只转发：`Read()` 到 `tagreader_core::ReadTag()`，`ReadCueSheet()` 到独立的 `tagreader_cue::ReadCueSheet()` 管线；不要把 CUE 塞入 `Read()`。
- `ReadTag()` 固定顺序：`ValidatePath()` -> `OpenContext()` -> 封面目录解析/校验/硬化 -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> sidecar fallback -> `ReadLyrics()` -> `BuildMusicTag()`；不要另加 `DetectContainer()`。
- CUE 文件引用解析拒绝绝对路径、目录逃逸、symlink 和 CUE 自引用。

## 解析与安全边界

- FFmpeg 只负责 probe、音频流、基础媒体信息及封面解码/像素转换，PNG 由 fpng 编码；标题、歌手、专辑、歌词和封面块由原始字节 parser 读取，不能改用 `AVDictionary`。
- Parser 共用 `ReadContext::input` 与 `AVFormatContext`；二进制访问使用绝对 offset 配合 `ReadRange()`/bounded reader，不依赖或污染流位置。
- `MusicTag` 文本终态必须是 UTF-8；中间态走 `RawMediaInfo`、`RawMetadata`、`RawLyrics`，由 `NormalizeMetadata()`/`NormalizeLyrics()` 收口。
- 分发覆盖 ID3v1/v2.2-v2.4、Vorbis Comment/FLAC/Ogg、OpusTags、MP4 `ilst`、APEv2、RIFF/WAV、AIFF/AIFC、DSF/DFF、ASF/WMA、Matroska/WebM/MKA 和 CUE。APE footer 检测优先于 ID3；MP3+APE 以 APE 为主，ID3v2/ID3v1 只补缺。
- 局部 malformed 元数据或歌词字段应跳过或清空局部结果；输入不可用、无音频流或上下文/容器无法建立才使普通读取顶层失败。封面错误遵循下一节的独立策略。
- 不得放宽资源上限：`ReadRange()` 默认 64 MiB；MP4 atom payload 64 MiB、最多 100000 atoms；封面编码输入和 PNG 输出各 64 MiB、单边 8192、总像素 `32 * 1024 * 1024`。常量集中在 `include/TagReaderInternal.hpp` 的 `CoverDecodeLimits`（该内部头文件位于公共 include 目录）与各 parser 文件顶部的 `kMax*` 常量。

## 封面处理

- `Read(path)` 也有导出副作用：内嵌封面不可用时查同目录 sidecar；默认写 `XDG_RUNTIME_DIR/tagreader-covers`，POSIX 回退为 `temp_directory_path()/tagreader-covers-$UID`。
- 默认目录会创建、拒绝 symlink 并硬化为当前用户私有；显式 `coverExportDir` 也会创建、探测读写并拒绝 symlink。
- 缓存是内容寻址 PNG；命中时直接复用，不重复解码或改写。默认模式是 `FullAndThumbnail`，默认失败策略是 `Propagate`。
- `Ignore` 只抑制 `CoverProcessingError`，并清空 artwork；元数据与歌词继续。其它异常不因该策略被吞掉。
- 默认 `maxSourceCoverBytes` 为 64 MiB，由内嵌封面与 sidecar 共用；默认 `maxSidecarEntries` 为 4096。

## 构建与验证

- 默认顺序必须是 `cmake --preset default` -> `cmake --build --preset default` -> `ctest --preset default --output-on-failure`。
- 聚焦测试用 `ctest --preset default -R <regex> --output-on-failure`。Catch2 discovered test 名就是精确 `TEST_CASE` 文本，没有 target 前缀；可用子串如 `TR-AUDIT-001`、`CoverContract:`、`cue file resolver`。
- 安全测试是 `TagReaderSecurityGenerateSamples` 与 `TagReaderSecuritySmoke`；样本由 `test/security/generate_samples.py` 生成。缺少 `ffmpeg` CLI 或 codec 导致无样本时，Smoke 返回 `77`，CTest 记为 skip。
- 另有 `release`、`sanitize`、`fuzz`、`profile` presets。Release 强制关闭 profiling；Fuzz 仅在 Clang/libFuzzer 下生成 `TagReaderFuzz`，相关 CTest 先生成 corpus；Profile 依赖系统 TracyClient 与 `/usr/include/Tracy`，不是 pkg-config 入口。
- FFmpeg 依赖由 pkg-config 解析：`libavformat`、`libavcodec`、`libavutil`、`libswscale`。Iconv 默认必需，只有显式设置 `-DTAGREADER_ALLOW_LATIN1_FALLBACK_WITHOUT_ICONV=ON` 才允许回退。
- Catch2 默认优先系统包（`TAGREADER_USE_SYSTEM_CATCH2=ON`），缺失时 FetchContent 下载 v3.7.1，离线环境首次配置需要网络。
- `release`/`profile` 编译参数含 `-march=native`，产物仅限本机使用，不可跨机器分发。
- `test/regression/regression_tests.cpp` 不是独立 target，但被 `tr_audit_001_031_catch2_tests.cpp` 与 `tr_audit_032_056_catch2_tests.cpp` 以 `#include` 方式文本包含编译（提供 `RunTrAudit*` 实现，由 TR-AUDIT-001~056 用例调用）；活跃用例在 `*_catch2_tests.cpp` 中。
- `TagReaderTest` 仅是人工 CLI：`./build/default/TagReaderTest <audio-file-path> [cover-export-dir]`，不能替代 CTest。
