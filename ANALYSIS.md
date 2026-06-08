# TagReader 深度代码审计报告

> 审计日期：2026-06-08
> 审计范围：当前仓库根目录内的 `include/`、`src/`、`test/`、`docs/`、`CMakeLists.txt`
> 审计方法：并行代码侦察、直接 `rg`/源码阅读、目标回归执行、sanitizer 构建、最小 PoC 本地验证、外部 APEv2/FFmpeg API 规范对照

## 一、项目真实架构简述

### 1.1 真实入口与调用链

公开 API 只有两个 facade：

```text
TagReader::Read(path)
TagReader::Read(path, coverExportDir)
```

二者均在 `src/TagReader.cpp` 直接转发到 `tagreader_core::ReadTag()`。当前主链路为：

```text
ValidatePath()
  -> OpenContext()
  -> ValidateCoverExportDir()
  -> DetectStream()
  -> DetectTagFormat()
  -> ContainerFromTagFormat()
  -> ReadMediaInfo()
  -> ReadMetadata()
  -> ReadLyrics()
  -> BuildMusicTag()
```

`ReadContext` 同时持有 `std::ifstream input`、文件大小/mtime、`AVFormatContext`、音频流索引、格式探测结果和可选封面目录。FFmpeg 负责 probe、音频流、基础媒体信息和封面图片解码/PNG 编码；标题、歌手、专辑、歌词、封面块等标签字段通过共享 `input` 读取原始字节解析。

### 1.2 组件拓扑

| 组件 | 真实职责 | 高风险点 |
|---|---|---|
| `src/core/TagPipeline.cpp` | public API 内部主流程、路径检查、parser 分发、局部异常吞噬 | 文件 TOCTOU、异常状态恢复、封面导出权限检查 |
| `src/media/` | FFmpeg 会话、流探测、容器/TagFormat 推导、基础媒体信息 | C API 生命周期、文件路径交给 FFmpeg、格式误分发 |
| `src/io/ByteReader.*` | 统一绝对 offset 二进制读取和 bounded cursor | offset/size 溢出、短读、stream failbit |
| `src/formats/id3/` | ID3v1/v2.2/v2.3/v2.4 header/frame/lyrics/APIC 解析 | 变长 frame、syncsafe、flag 语义、图片 payload |
| `src/formats/flac/` | FLAC metadata block、Vorbis Comment、PICTURE | block size、图片字段长度、lyrics block 上限 |
| `src/formats/ogg-vorbis/` | Ogg page/lacing/packet/Vorbis Comment 扫描 | page 计数、packet 拼接、sequence/continuation 状态机 |
| `src/formats/mp4/` | MP4 atom walker、`ilst`、lyrics/freeform、`covr` | atom size、size=0 恢复、嵌套遍历、payload 上限 |
| `src/formats/ape/` | APEv2 footer/header/item、MP3+APE 主字段、APE cover/lyrics | APE footer tagSize 语义、offset 回绕、item 边界 |
| `src/text/` | UTF-8/UTF-16/iconv/Latin-1 解码、LRC 规范化 | 编码声明零信任、decoded size、timestamp 溢出 |
| `src/cover/` | content-addressed PNG 缓存、FFmpeg 图像解码、原子写入 | 导出目录读写权限、cache poisoning、C API RAII、输出上限 |

### 1.3 IO 与失败模型

- `ReadRange()` 是 parser 的主要 IO 边界：限制 `size <= maxSize`、`offset <= streamoff::max()`、`size <= streamsize::max()`，并在 seek/read 失败后清理 stream 状态。
- parser 使用绝对 offset，不应直接依赖当前位置；ID3v1 是少数直接 `seekg(-128, end)` 的固定尾部读取路径。
- `ReadMetadata()` 对大多数 parser `runtime_error` 局部吞噬，但封面导出/缓存错误会重新抛出，避免缓存污染静默通过。
- `ReadLyrics()` 捕获 parser 错误后返回空歌词；最终文本通过 `NormalizeMetadata()`/`NormalizeLyrics()` 收敛为 UTF-8。
- 按当前产品策略，封面导出默认启用：调用方未提供导出目录时使用系统临时目录（Linux 上通常是 `/tmp`），调用方提供目录时默认信任该目录；建议只做读写权限可用性检查，不把目录 symlink 作为默认安全错误。

### 1.4 本次验证基线

- `cmake --build build`：通过。
- `cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON`：通过。
- `cmake --build build-sanitize`：通过。
- `python3 test/corpus/generate_corpus.py`：生成 `encoding/flac/id3/image/mp4/ogg` 共 73 个 seed 到 `/tmp/opencode/tagreader_fuzz_corpus`。
- `cmake -S . -B build-fuzz -DTAGREADER_ENABLE_FUZZING=ON`：在当前 GNU 16.1.1 环境下配置成功，但按 CMake 逻辑警告 `TagReaderFuzz` 需要 Clang/libFuzzer，未生成 fuzz target。
- 目标回归通过：`TR-AUDIT-001` 至 `TR-AUDIT-025` 均可按单 case 参数运行通过。
- sanitizer 目标回归通过：`TR-AUDIT-013`、`TR-AUDIT-017`、`TR-AUDIT-020`、`TR-AUDIT-023`。
- 隔离目录下的 `TagReaderSecuritySmoke` 对 `cover_cache_base.mp3` 和 `id3v24_apic_png.mp3` 均通过缓存复用/并发/污染拒绝检查。
- `./build/TagReaderRegressionTests` 无参数会返回用法错误；当前正确用法是 `--list|<TR-AUDIT-case-id>`。
- `./build/TagReaderSecuritySmoke` 对两个共享同一封面 hash 的样本连续运行时，第二个样本会遇到已被前一个 smoke 故意污染的缓存文件；这是命令安排导致的预期失败，不是新的解析漏洞。

## 二、漏洞与缺陷详情

### ℹ️ Policy-001：封面导出目录默认受信，目录 symlink 不作为漏洞保留

- **策略结论**：此前将 `coverExportDir` 或 shard 父目录 symlink 视为 `High` 风险，是基于“导出目录可能来自不受信租户输入”的假设。用户已明确产品策略：未提供导出目录时默认写入系统临时目录；提供导出目录时默认信任用户选择的位置，推荐只做读写权限检验。因此，目录 symlink 跟随属于可接受的导出目录语义，不再作为安全漏洞或 P0 修复项保留。
- **当前行为证据**：`ValidateCoverExportDir()` 使用 `create_directories()`、`exists()`、`is_directory()`，这些 `std::filesystem` API 会按平台语义解析目录组件；`BuildCoverCachePath()` 继续使用 content-addressed 路径 `coverExportDir / first2hex / rest.png`。这与“用户给出的目录即为导出目标”的策略一致。
- **仍需保留的校验**：应确认最终可写目录具备创建 shard 目录、写临时文件、发布 PNG、读取/复用已有缓存文件的权限；权限不可用时返回明确的 `cover cache`/`cover export` 类错误。最终缓存文件仍应保持当前的内容校验和污染拒绝逻辑。
- **可选部署加固**：如果未来某个服务端场景把导出目录从不受信用户输入直接透传，可在该上层应用或平台适配层额外禁用 symlink、使用私有临时子目录、或采用 POSIX `openat()`/Windows reparse point 检查。但这属于特定部署策略，不是 TagReader 默认 API 的强制安全要求。

### 🟡 Medium-001：APEv2 footer-only tagSize 语义错误导致规范标签无法解析

- **Bug 类别 & 触发位置**：格式语义/offset 计算错误；`src/formats/ape/ApeParser.cpp::ReadApeMetadata()` 与 `ReadApeLyrics()`。
- **漏洞描述与触发原理**：APEv2 规范规定 footer/header 的 `Tag Size` 是“包含 footer 和所有 tag items，排除 header”。TagLib 与 Mutagen specs 也按 `footerLocation + Footer::size() - tagSize` 定位 item 区，并读取 `tagSize - Footer::size()` 作为 item payload。当前实现把无 header 的 `tagSize` 当作“仅 itemBytes”，使用 `context.fileSize - 32 - tagSize` 作为 `itemRegionOffset`，并读取 `tagSize` 字节。这与规范不一致。对合法 footer-only APEv2，`tagSize = itemBytes + 32`，当前代码会把 offset 向前多退 32 字节，把音频尾部数据混入 item 解析，导致合法 APE 元数据/歌词被静默丢弃。
- **本地复现证据**：用 FFmpeg 生成最小 MP3 后追加 footer-only APEv2：一个 `Title=SpecTitle` item，item 长度 23 字节，footer `tagSize=55`。`./build/TagReaderTest /tmp/opencode/tagreader_audit/ape_spec_footer.mp3` 输出 `title:` 为空，而应读出 `SpecTitle`。
- **极端破坏场景推演**：大量由标准工具写出的 footer-only APEv2 标签会被识别为 `TagFormat::Ape`，从而优先于 ID3，但 APE 主字段解析为空；MP3+APE 场景中只有 ID3 补缺字段可见，APE 中独有字段和歌词丢失。攻击者可用合法 APE footer 让系统误以为“无元数据”，造成元数据完整性绕过，不属于内存破坏但属于可达逻辑缺陷。
- **修复建议**：统一按 APEv2 规范解释 `tagSize`：必须至少为 32；item 区大小为 `tagSize - 32`；footer 起点为 `fileSize - 32`；item 起点为 `fileSize - tagSize`。若 header present，再校验 `itemOffset - 32` 处 header，但不要把 header 计入 footer `tagSize` 的 item 读取区域。

```cpp
if (tagSize < 32 || tagSize > kMaxApeTagBytes || tagSize > context.fileSize) {
    return;
}
const std::uintmax_t footerOffset = context.fileSize - 32;
const std::uintmax_t itemRegionOffset = context.fileSize - tagSize;
const std::size_t itemRegionSize = static_cast<std::size_t>(tagSize - 32);
const std::vector<uint8_t> itemBytes = ReadRange(context.input, itemRegionOffset, itemRegionSize, kMaxApeTagBytes);
if (itemBytes.size() != itemRegionSize) {
    return;
}
```

建议补充回归：footer-only APEv2 `Title`、`LYRICS`、`Track` 均应解析；带 header 的 APEv2 应验证 header flag 和 footer flag 的 bit 29/30/31 组合，不应把 header 字节作为 item 解析。

### 🟡 Medium-002：ID3v2.2 元数据路径接受 unsupported compression flag

- **Bug 类别 & 触发位置**：格式 flag 零信任不一致；`src/formats/id3/Id3Parser.cpp::ReadID3v2Metadata()` 与 `ReadID3Lyrics()`。
- **漏洞描述与触发原理**：`Id3v22TagFlagsAreSupported()` 明确认为 ID3v2.2 compression flag `0x40` 不支持。`ReadID3Lyrics()` 在 versionMajor==2 时会调用该函数，遇到 unsupported flag 直接返回；但 `ReadID3v2Metadata()` 对同一个 `tagView.flags` 没有检查，仍然按未压缩 frame 解析。攻击者可以构造带 compression flag 的 ID3v2.2 tag，让 metadata 路径信任本应拒绝的压缩/未知编码状态。
- **本地复现证据**：构造 `ID3 02 00 40`、`TT2=FlagTitle` 的 MP3，执行 `./build/TagReaderTest /tmp/opencode/tagreader_audit/id3v22_compression_flag.mp3`，输出 `title: FlagTitle`。同类 flags 在歌词路径会被拒绝，显示两个 parser 分支安全策略不一致。
- **极端破坏场景推演**：如果未来添加 ID3v2.2 压缩支持前的部分解析逻辑，metadata 分支可能把压缩字节流当普通 frame 载荷传入文本/图片 decoder，造成字段污染、误解析封面或更大的 CPU/内存开销。当前影响主要是格式语义绕过和数据完整性问题，没有发现 OOB 读写。
- **修复建议**：在 `ReadID3v2Metadata()` 中对 v2.2 使用与歌词路径一致的 tag flag gate；未知/压缩 flags 直接跳过整个 ID3v2.2 metadata tag。

```cpp
if (tagView.versionMajor == 2) {
    if (!Id3v22TagFlagsAreSupported(tagView.flags)) {
        return;
    }
    ReadID3v22Frames(context, metadata, tagView.bytes, tagView.cursor);
    return;
}
```

建议补充回归：`ID3v2.2 flags=0x40` 的 `TT2` 不应进入 `MusicTag.title()`；`flags=0x80` unsync 若未实际实现 tag-level v2.2 unsync，也应明确拒绝或实现后再接受。

### 🟡 Medium-003：封面解码把未填充的外部字节直接交给 FFmpeg decoder

- **Bug 类别 & 触发位置**：FFmpeg C API 误用 / packet 输入缓冲区边界；`src/cover/CoverDecoder.cpp::ConvertImageToPng()`。
- **漏洞描述与触发原理**：`ConvertImageToPng()` 先 `av_packet_alloc()`，随后把 caller 持有的 `data` 直接赋给 `packet->data`，并设置 `packet->size`；`packet->buf` 保持 `nullptr`，代码也没有为输入尾部补 `AV_INPUT_BUFFER_PADDING_SIZE` 的零填充。FFmpeg 解码器和 bitstream reader 通常假设输入 buffer 末尾存在 padding。对截断或畸形的嵌入图片，decoder 可能在 packet 末尾附近做宽读，触发越界读、崩溃或未定义行为。该路径在封面导出启用时触发：包括默认系统临时目录导出，以及调用方显式传入导出目录。
- **证据与可达性**：所有 ID3/FLAC/MP4/APE 封面块最终都会进入 `WriteCoverAsPng()`，再调用 `DecodeAndEncodeCoverPng()`/`ConvertImageToPng()`。当前输入大小只限制为 64 MiB，并没有复制到 FFmpeg 自有 padded packet；已有 `TR-AUDIT-014`/`TR-AUDIT-023` 覆盖 malformed/oversized cover 的功能行为，但没有覆盖 decoder 末尾 padding 契约。
- **极端破坏场景推演**：攻击者提交带畸形 PNG/JPEG/WebP/GIF/TIFF/BMP 封面的音频文件，服务端开启封面导出时，FFmpeg decoder 在解析尾部 bitstream 时读取 packet 后方未定义内存，导致进程崩溃或 sanitizer 报告 OOB read。影响主要是本进程可用性和未定义行为；是否泄露内存取决于具体 decoder 和构建。
- **修复建议**：不要把未填充的外部 buffer 直接挂到 `AVPacket`。使用 `av_new_packet()` 让 FFmpeg 分配 packet 数据后 `memcpy`，或自行分配 `size + AV_INPUT_BUFFER_PADDING_SIZE` 字节、复制输入并把尾部清零，再用 `av_packet_from_data()`/owned packet 管理生命周期。

```cpp
std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
if (packet == nullptr || av_new_packet(packet.get(), static_cast<int>(size)) < 0) {
    return {};
}
std::memcpy(packet->data, data, size);
```

建议补充回归：构造末尾截断的 APIC/PICTURE/covr 图片样本，在 sanitizer 构建下覆盖默认临时目录导出和显式导出目录两条路径；修复后不应出现 decoder 侧 OOB read，且返回空封面或明确封面解码失败。

### 🔵 Low-001：入口音频文件 symlink/TOCTOU 防护顺序不严谨

- **Bug 类别 & 触发位置**：文件路径 TOCTOU / C API 路径跟随；`src/core/TagPipeline.cpp::ValidatePath()`、`src/media/FfmpegSession.cpp::OpenContext()`。
- **漏洞描述与触发原理**：入口先用 `exists()`/`is_regular_file()` 检查路径，再在 `OpenContext()` 中调用 `file_size()`、`last_write_time()`、`ifstream.open()`，随后才 `is_symlink(filePath)`，最后把裸路径传给 `avformat_open_input()`。这意味着符号链接检查发生在 C++ stream 打开之后，且 FFmpeg 仍按路径重新打开文件。若路径所在目录可被攻击者并发修改，检查和使用之间可以切换目标。当前已有直接 symlink 检查，因此普通静态 symlink 会被拒绝；问题集中在 TOCTOU 竞态和 FFmpeg 二次打开路径不绑定到已验证 file identity。
- **极端破坏场景推演**：在多租户上传目录中，攻击者可在 `ValidatePath()`、`ifstream.open()`、`is_symlink()`、`avformat_open_input()` 之间替换路径，使 FFmpeg probe 和 raw parser 读取不同文件或读取被策略本应拒绝的目标。影响取决于调用环境权限；单机 CLI 使用风险较低。
- **修复建议**：把 symlink 检查移到打开前，并在 POSIX 下优先使用 `open(..., O_RDONLY | O_CLOEXEC | O_NOFOLLOW)` 获取 fd，`fstat()` 校验 regular file，再通过 `/proc/self/fd/<fd>` 或 FFmpeg custom AVIO 绑定到同一 fd；raw parser 也从同一 fd/stream 派生，避免 FFmpeg 与 `ifstream` 各自按路径打开。

```cpp
FileDescriptor fd(::open(filePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
if (fd.get() < 0) {
    throw std::runtime_error("failed to open trusted input file");
}
struct stat st{};
if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    throw std::runtime_error("input path is not a trusted regular file");
}
```

### 🔵 Low-002：无 `iconv` 构建仍会以 Latin-1 接受本地编码字节

- **Bug 类别 & 触发位置**：编码降级/数据完整性；`src/text/TextCodec.cpp::DetectLegacyLocalEncoding()`。
- **漏洞描述与触发原理**：有 `Iconv` 时，代码会尝试 GB18030、GBK、SHIFT_JIS、CP932、BIG5、Windows code pages 等候选；无 `Iconv` 时直接返回 `latin-1`。这不会破坏 UTF-8 内存安全，但会把 GBK/Shift-JIS 等非 UTF-8 字节稳定转成 Latin-1 乱码并作为“成功解码”的 UTF-8 返回。代码中已有 `#warning` 和注释承认该限制，本次将其列为低危架构/构建风险。
- **极端破坏场景推演**：下游把 `MusicTag.title()` 作为可信展示文本时，攻击者可用非 Latin-1 本地编码制造乱码、混淆搜索/去重或绕过基于文本的策略。
- **修复建议**：生产构建要求 `Iconv` 必选；或在无 iconv 时让 `DetectLegacyLocalEncoding()` 返回失败状态，不再自动 Latin-1 接受非 UTF-8/非 BOM UTF-16 字节。

## 三、已审查但本次未作为漏洞保留的候选

| 候选 | 处理结果 |
|---|---|
| `ReadRange()` 越界/OOM | 已有 `maxSize`、`streamoff`、`streamsize`、offset 加法保护；未发现 OOB。 |
| ID3 frame walker 遇畸形 frame 后丢弃后续 frame | 当前已有 resync 预算并通过 `TR-AUDIT-021` 验证 5000 字节 gap 恢复。 |
| ID3/APE 数字解析宽松 | `ParseUInt16()` 已统一 strict consumed 检查，`TR-AUDIT-025` 通过。 |
| APE `tagSize > fileSize` 回绕 | metadata 路径已有 guard，`TR-AUDIT-020` 和 sanitizer 对应样本通过；lyrics 路径仍建议同步 guard，但当前 `ReadRange()` 会返回空，不构成 OOB。 |
| 用户导出目录或 shard 父目录 symlink | 在“显式导出目录默认受信”的产品策略下不作为漏洞保留；默认系统临时目录和用户目录都应保留读写权限检查与最终缓存文件内容校验。 |
| 封面缓存污染最终 PNG | `ValidateExistingCoverCacheFile()` 会比较 size 和 bytes，`TR-AUDIT-013` 通过；仍应保留现有污染拒绝逻辑。 |
| 封面解码 C API RAII 泄漏 | `AVFrame`、`AVPacket`、`AVCodecContext`、`SwsContext` 均有 RAII deleter；但输入 packet padding 契约另见 `Medium-003`。 |
| 封面解码尺寸上限后置 | 当前宽高/像素/输出限制在 decode 后校验，存在理论 DoS 面；已有输入 64 MiB、输出 64 MiB、像素 32M 上限，未在本环境复现 decoder 预分配绕过，因此作为残余风险而非确认漏洞。 |
| `NormalizeMetadata()` UTF-8 截断 OOB | 后台审计曾提示 `normalized[cut]`；复核后因分支条件是 `normalized.size() > 65536`，`cut == 65536` 下标仍在范围内。该逻辑检查的是截断点后一字节，用于回退到完整 codepoint，未作为 OOB 保留。 |
| Ogg packet/page DoS | page、logical stream、packet、扫描字节均有限制，`TR-AUDIT-009` 通过。 |
| UTF-16/LRC 溢出 | BOM、surrogate、decoded bytes、LRC minute/line limits 已覆盖，`TR-AUDIT-008`、`TR-AUDIT-010`、`TR-AUDIT-024` 通过。 |
| 多线程共享状态 | 每次 `ReadTag()` 独立 `ReadContext`；仅封面 temp counter 是 atomic，`TR-AUDIT-015` 通过。 |

## 四、建议修复优先级

| 优先级 | 项目 | 建议 |
|---|---|---|
| P1 | Medium-001 | 修正 APEv2 `tagSize` 语义；新增 footer-only/header-present APE 回归。 |
| P1 | Medium-002 | 统一 ID3v2.2 metadata/lyrics flags gate；新增 unsupported flag 回归。 |
| P1 | Medium-003 | 修正 FFmpeg decoder 输入 packet padding；新增默认临时目录和显式目录的 sanitizer 封面截断回归。 |
| P2 | Low-001 | 将入口文件打开改为 fd 绑定模型，减少 FFmpeg 与 raw parser 双重按路径打开。 |
| P3 | Low-002 | 将无 iconv 构建策略改为显式低置信/失败，或生产环境强制 Iconv。 |

## 五、审计残余风险

- 本次没有执行长时间 libFuzzer；只审查了 fuzz harness/corpus 设计并运行了目标回归与 sanitizer 关键样本。
- 本环境的 AST-grep 接口返回不可用，系统 `/usr/bin/sg` 也不是 ast-grep；结构化搜索由 `rg`、源码阅读、LSP 诊断和并行审计代理补足。
- 外部 FFmpeg 解码器自身漏洞不在本仓库修复范围内；仓库应继续保持封面输入/像素/输出上限，并及时更新 FFmpeg。
- 当前开发环境为 Linux，但项目计划兼容 Windows；封面导出相关建议应优先保持跨平台语义，POSIX `openat()`/Windows reparse point 检查只作为特定部署加固选项。
- 仍建议为 `Medium-001`、`Medium-002`、`Medium-003` 添加专门 `TR-AUDIT-026+` 回归后再修复，避免未来重引入。
