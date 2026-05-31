# TagReader 设计说明

## 当前定位

- TagReader 是一个 C++23 轻量音乐元数据读取库。
- 对外 facade 保持单一读取入口：`TagReader::Read(path)` 和可选封面导出目录的 `TagReader::Read(path, coverExportDir)`。
- `Read()` 的主流程是 `ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `DetectContainer()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- `MusicTag` 是最终返回对象；解析中间状态保存在内部的 `RawMediaInfo`、`RawMetadata`、`RawLyrics` 等结构中。
- 写入 `MusicTag` 的文本字段必须是 UTF-8。

## FFmpeg 与原始字节解析分工

- FFmpeg 用于输入 probe、容器识别、主音频流定位、基础媒体信息读取，以及封面图像解码和 PNG 编码。
- 标题、歌手、专辑、歌词、封面块等标签字段不使用 `AVDictionary` 作为元数据来源。
- 元数据和歌词解析优先通过 `ReadContext::input` 直接读取文件原始字节，再按 ID3、Vorbis/FLAC、Ogg Vorbis、MP4 atom 等格式规则解释。
- 封面块来自 ID3 `PIC/APIC`、FLAC `PICTURE`、MP4 `covr` 等格式分支；只有调用方传入 `coverExportDir` 时才导出。

## 输入与失败策略

- `ValidatePath()` 只做早期路径形态检查，例如空路径、不存在、非普通文件、文件大小边界。
- 真实可读性以 `ifstream.open()`、`avformat_open_input()` 和后续读取结果为准；路径检查不是权限授权判断。
- `OpenContext()` 建立 FFmpeg 上下文和独立文件输入流，后续标签解析继续使用文件输入流读取原始字节。
- 格式解析器遇到 malformed 数据时尽量局部失败：损坏字段、损坏歌词或损坏封面可被跳过，`Read()` 在媒体流和容器仍可用时继续返回可用的 `MusicTag`。
- 输入无效、无法打开、没有可用音频流或容器无法建立时，`Read()` 仍按顶层错误路径失败。

## 元数据与歌词解析

- ID3 分支读取 ID3v1、ID3v2.2、ID3v2.3、ID3v2.4 的已支持文本帧、歌词帧和图片帧。
- Vorbis/FLAC 分支读取 Vorbis Comment entry；单个非法 UTF-8 entry 只影响该 entry，后续合法 entry 仍可解析。
- Ogg Vorbis 分支按 page 和 packet 边界扫描 comment packet，并对截断、continuation、payload 大小设置本地失败。
- MP4 分支通过 atom walker 定位 `moov/udta/meta/ilst`，读取已支持 metadata item、`©lyr` 和 iTunes freeform lyrics。
- MP4 walker 内部使用本地解析状态区分 `Ok`、`NotFound`、`Malformed`、`ResourceLimit`，但这些状态不进入 public API。
- FLAC `PICTURE` 当前使用 cpp 内部 bounded `ByteCursor` 解析字段长度和图片字节；其它 parser 游标尚未迁移到该 helper。

## 封面导出与缓存

- 封面导出目录由调用方通过 `Read(path, coverExportDir)` 提供；未提供时不写封面文件。
- 封面缓存是 content-addressed PNG storage，缓存键基于音频文件内嵌图片原始字节计算。
- 缓存路径格式为 `coverExportDir / first2hex / rest.png`，其中 `first2hex` 是 hash 前两个十六进制字符，`rest.png` 是剩余 hash 加 `.png` 后缀。
- 已存在的缓存路径直接返回，不再重复解码或转码。
- 首次写入时会把内嵌图片解码并统一编码为 PNG；无法解码的封面保持为空或被跳过。

## 构建与测试资产

- 普通构建命令是 `cmake -S . -B build` 和 `cmake --build build`。
- 构建目标包括静态库 `TagReaderCore`、人工验收程序 `TagReaderTest`、安全 smoke 程序 `TagReaderSecuritySmoke`。
- `TagReaderTest` 是字段打印程序，不是单元测试框架。
- fuzz corpus 由 `python3 test/corpus/generate_corpus.py` 生成，默认输出 `/tmp/opencode/tagreader_fuzz_corpus`；仓库不提交二进制 seed。
- 仓库当前没有配置 CI workflow、lint、formatter 或单元测试框架。
