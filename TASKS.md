# Phase 1 - P0 资源泄漏与局部安全护栏

目标：
为什么现在做这一阶段。

先修复不改变外部语义、回归面最小、但会直接影响长期运行稳定性的资源泄漏和通用 helper 防御缺口。该阶段不改变 parser dispatch、不改变 API、不改变输出字段策略，适合作为后续 parser 修改前的安全基线。

修改内容：
精确到函数级。

- 修改 `src/TagReader.cpp` 的 `ConvertTextWithIconv()`。
- 修改 `src/TagReader.cpp` 的 `DecodeTextToUtf8()`。
- 修改 `src/TagReader.cpp` 的 `TryReadUtf16Text()`。
- 修改 `src/TagReader.cpp` 的 `ReadUtf8Text()`。
- 新增内部 RAII helper：`IconvHandle`，仅在 `TAGREADER_HAS_ICONV` 下编译。
- 新增或复用常量：`kMaxTextFieldBytes`、`kMaxDecodedTextBytes`，不要新增 public 配置。

步骤：

Step 1:

- 修改位置：`src/TagReader.cpp`，`ConvertTextWithIconv()` 附近，约 1669 行。
- 修改内容：把输入大小检查移动到 `iconv_open()` 之前，并用 RAII 包装 `iconv_t`，保证所有 early return 都调用 `iconv_close()`。
- 示例代码结构：

```cpp
#if TAGREADER_HAS_ICONV
class IconvHandle {
public:
    explicit IconvHandle(iconv_t cd) noexcept : cd_(cd) {}
    ~IconvHandle() { if (cd_ != reinterpret_cast<iconv_t>(-1)) iconv_close(cd_); }
    IconvHandle(const IconvHandle&) = delete;
    IconvHandle& operator=(const IconvHandle&) = delete;
    iconv_t get() const noexcept { return cd_; }
private:
    iconv_t cd_;
};
#endif

std::optional<std::string> ConvertTextWithIconv(...) {
    if (size > kMaxDecodedTextBytes / 4) {
        return std::nullopt;
    }
    IconvHandle cd(iconv_open("UTF-8", fromEncoding));
    if (cd.get() == reinterpret_cast<iconv_t>(-1)) {
        return std::nullopt;
    }
    ...
}
```

- 风险：RAII helper 放置位置若在未启用 Iconv 的构建中暴露，会造成条件编译错误。
- 回归影响：只影响 legacy encoding fallback；合法短文本输出必须保持不变。
- 验证方式：分别构建启用 Iconv 和未启用 Iconv 的配置；使用 ASAN/LSAN 构建读取包含大 legacy 文本字段的文件。
- 测试样本：一个 ID3v2.3 `TIT2` 使用 encoding 0 且 payload 大于 `kMaxDecodedTextBytes / 4` 的 mp3；一个普通 Latin-1 ID3v1 mp3；一个 UTF-8 Vorbis comment flac。
- 验收标准：大 legacy 文本返回空字段或被忽略，不泄漏 iconv descriptor；普通 Latin-1 标题仍能读取；构建无条件编译错误。
- 失败标准：LSAN 报告 iconv 相关泄漏；普通短文本乱码；未启用 Iconv 时构建失败。
- 回滚：仅回滚 `IconvHandle` 和 `ConvertTextWithIconv()` 修改；不回滚本阶段其它 helper guard。

Step 2:

- 修改位置：`src/TagReader.cpp`，`DecodeTextToUtf8()`，约 4383 行。
- 修改内容：函数入口统一拒绝超大输入，避免未来 parser 绕过外层限制。
- 示例代码结构：

```cpp
std::optional<std::string> DecodeTextToUtf8(std::span<const std::uint8_t> raw,
                                            std::string_view encoding) {
    if (raw.size() > kMaxTextFieldBytes) {
        return std::nullopt;
    }
    ...
}
```

- 风险：如果当前某些合法歌词字段通过该函数且大小超过 `kMaxTextFieldBytes`，会从“可读取”变为“忽略”。
- 回归影响：P0 目标优先限制资源；歌词的大字段应走歌词专用上限而不是文本字段 helper。
- 验证方式：构造接近上限和超过上限的 UTF-8、UTF-16LE、UTF-16BE payload。
- 测试样本：MP4 `©nam` 1 KiB UTF-8；MP4 `©nam` 超过 `kMaxTextFieldBytes`；ID3 UTF-16 标题；Vorbis comment 正常 UTF-8 标题。
- 验收标准：正常小字段读取一致；超大字段被忽略且不 crash；ASAN/UBSAN 无报错。
- 失败标准：普通 metadata 字段丢失；超大字段仍触发大分配或耗时明显增长。
- 回滚：回滚 `DecodeTextToUtf8()` 入口 guard，并保留 `TryReadUtf16Text()` 输出 guard。

Step 3:

- 修改位置：`src/TagReader.cpp`，`TryReadUtf16Text()`，约 1738 行。
- 修改内容：每次 append UTF-8 片段前检查输出大小，超过 `kMaxDecodedTextBytes` 立即返回 `std::nullopt`。
- 示例代码结构：

```cpp
auto appendChecked = [&](std::string_view chunk) -> bool {
    if (chunk.size() > kMaxDecodedTextBytes - value.size()) {
        return false;
    }
    value.append(chunk);
    return true;
};
```

- 风险：需要保证 `kMaxDecodedTextBytes - value.size()` 不在 `value.size()` 已超过上限时下溢。
- 回归影响：只拒绝异常放大的 UTF-16 输出；正常 UTF-16 文本不变。
- 验证方式：用 UTF-16 surrogate、BOM、odd length、超大有效 BMP 字符集样本跑 ASAN/UBSAN。
- 测试样本：UTF-16LE BOM 标题；UTF-16BE 标题；奇数字节 UTF-16；孤立 high surrogate；超过输出上限的重复字符。
- 验收标准：合法 UTF-16 正确输出 UTF-8；非法 surrogate 返回空；超大输出被拒绝且内存稳定。
- 失败标准：UTF-16 BOM 正常样本回归；UBSAN 报整数下溢；超大样本 OOM。
- 回滚：回滚 append guard，并保留 `DecodeTextToUtf8()` 输入 guard。

Step 4:

- 修改位置：`src/TagReader.cpp`，`ReadUtf8Text()`，约 1727 行。
- 修改内容：添加 `data == nullptr && size != 0` 防御返回。
- 示例代码结构：

```cpp
if (data == nullptr && size != 0) {
    return std::nullopt;
}
```

- 风险：无外部行为风险，当前 public 路径理论上不会传入该组合。
- 回归影响：无。
- 验证方式：新增最小内部测试或临时 fuzz harness 直接调用 helper；如果 helper 不是可见符号，则通过构造空 frame payload 验证无异常。
- 测试样本：空 UTF-8 文本字段；正常 UTF-8 文本字段；内部 null+nonzero 单元样本。
- 验收标准：空字符串仍可处理；null+nonzero 返回空；无 crash。
- 失败标准：空字符串被误判失败；任何正常 UTF-8 字段丢失。
- 回滚：单行 guard 可独立回滚。

Step 5:

- 修改位置：`test/corpus/generate_corpus.py` 和 `test/fuzz/tagreader_fuzz.cpp`。
- 修改内容：新增 deterministic corpus seed：大 legacy ID3 文本、非法 UTF-16、超大 UTF-16、空 UTF-8 字段；不改变 fuzz target API。
- 风险：corpus 生成脚本输出过大导致本地 fuzz 初始 corpus 膨胀。
- 回归影响：只影响测试资源。
- 验证方式：运行 `python3 test/corpus/generate_corpus.py`，再用 fuzz 构建短跑 corpus。
- 测试样本：`id3v23_large_latin1_text.mp3`、`id3v23_utf16_odd_length.mp3`、`mp4_utf16_title_over_limit.m4a`。
- 验收标准：生成脚本成功；fuzz target 可以读取所有 seed；ASAN/UBSAN/LSAN 无报错。
- 失败标准：生成脚本异常；seed 大小超过设计上限；fuzz 启动即 crash。
- 回滚：删除新增 seed 生成函数，不回滚代码修复。

# Phase 2 - P0 Cover Cache 原子发布与去重

目标：
为什么现在做这一阶段。

Cover 写入涉及文件系统竞争和外部可见副作用，必须在 parser 大规模改动前稳定。该阶段严格遵守既定决策：调用方提供 `coverExportDir`、文件名基于原始 embedded image bytes hash、content-addressed storage、统一 PNG、已存在时禁止重复 decode/transcode、atomic write、考虑多线程和多进程竞争。

修改内容：
精确到函数级。

- 修改 `src/TagReader.cpp` 的 `HashEmbeddedImageBytes()`。
- 修改 `src/TagReader.cpp` 的 `BuildCoverCachePath()`。
- 修改 `src/TagReader.cpp` 的 `WriteCoverAsPng()`。
- 替换或重写 `src/TagReader.cpp` 的 `AtomicWriteFileIfAbsent()`。
- 新增 helper：`EnsureDirectoryFsyncable()`、`PublishFileIfAbsent()`、`FsyncDirectory()`。
- Linux 优先使用 `link()` 或 `renameat2(RENAME_NOREPLACE)`；为减少 syscall 兼容风险，首选 `link(temp, final)` 发布模型。

步骤：

Step 1:

- 修改位置：`src/TagReader.cpp`，`BuildCoverCachePath()`，约 273-302 行。
- 修改内容：确认并固定路径格式为 `coverExportDir / first2Hex / restHex + ".png"`。如果当前 hash 输出不足 3 个 hex 字符，直接视为内部错误返回空路径。
- 示例代码结构：

```cpp
CoverCachePath BuildCoverCachePath(const std::filesystem::path& root,
                                   std::span<const std::uint8_t> imageBytes) {
    const auto hash = HashEmbeddedImageBytes(imageBytes);
    if (hash.size() < 3) return {};
    return {root / hash.substr(0, 2) / (hash.substr(2) + ".png")};
}
```

- 风险：如果已有缓存使用旧路径格式，升级后不会命中旧缓存。
- 回归影响：符合新设计决策；不提供旧路径兼容，避免复杂化。
- 验证方式：同一 embedded image 在不同音频文件中导出到同一路径；不同图片路径不同。
- 测试样本：两个文件内嵌完全相同 JPEG APIC；同一歌曲名但不同图片；不同歌曲名但同一图片。
- 验收标准：路径只由 embedded image bytes 决定；路径包含两字符分片目录；不含歌曲名、歌手名、源文件 hash。
- 失败标准：路径中出现 metadata 字段；同图不同文件导出不同路径；不同图导出相同路径。
- 回滚：回滚 `BuildCoverCachePath()`，但不得回到歌曲名或文件 hash 命名。

Step 2:

- 修改位置：`src/TagReader.cpp`，`WriteCoverAsPng()`，约封面导出入口。
- 修改内容：先计算最终路径并检查 `std::filesystem::exists(finalPath)`；如果已存在，直接返回路径，禁止调用 `DecodeAndEncodeCoverPng()`。
- 示例代码结构：

```cpp
const auto finalPath = BuildCoverCachePath(context.coverExportDir, imageBytes);
if (std::filesystem::exists(finalPath)) {
    return finalPath;
}
auto pngBytes = DecodeAndEncodeCoverPng(imageBytes);
return AtomicWriteFileIfAbsent(finalPath, pngBytes) ? finalPath : existingPath;
```

- 风险：存在损坏 PNG 时也会被直接返回；这是 content-addressed cache 的既定性能语义。
- 回归影响：重复读取相同封面时 CPU 显著下降；首次读取行为不变。
- 验证方式：对 `DecodeAndEncodeCoverPng()` 增加测试计数钩子不可取；改用文件 mtime 和运行时间确认第二次未重写。
- 测试样本：一个 ID3v2.4 带 APIC 的 mp3；一个 FLAC PICTURE；一个 MP4 `covr`。
- 验收标准：第一次生成 PNG；第二次返回同一路径且文件 mtime 不变；第二次不创建临时文件残留。
- 失败标准：第二次覆盖或修改 PNG；第二次仍产生临时文件；无 coverExportDir 时仍尝试导出。
- 回滚：回滚 exists fast path，但保留 atomic publish 修复。

Step 3:

- 修改位置：`src/TagReader.cpp`，`AtomicWriteFileIfAbsent()`，约 370-445 行。
- 修改内容：改为“写 temp 文件后用 hard link 发布”的 if-absent 模型。流程为：创建同目录唯一 temp；写入全部 PNG；`fsync(tempFd)`；关闭 temp；`link(tempPath, finalPath)`；如果 `EEXIST`，删除 temp 并返回“已有目标”；如果成功，`fsync(parentDir)` 后删除 temp link 或保留 final link。
- 示例代码结构：

```cpp
enum class PublishResult { Published, AlreadyExists, Failed };

PublishResult PublishFileIfAbsent(const std::filesystem::path& temp,
                                  const std::filesystem::path& final) {
    if (::link(temp.c_str(), final.c_str()) == 0) {
        FsyncDirectory(final.parent_path());
        ::unlink(temp.c_str());
        return PublishResult::Published;
    }
    if (errno == EEXIST) {
        ::unlink(temp.c_str());
        return PublishResult::AlreadyExists;
    }
    ::unlink(temp.c_str());
    return PublishResult::Failed;
}
```

- 风险：hard link 在某些文件系统或权限策略下可能失败；temp 必须与 final 同目录，避免跨设备。
- 回归影响：不会覆盖已有目标；并发下最多一个 writer 发布成功，其它 writer 返回已有路径。
- 验证方式：并发启动 16 个进程读取同一带封面的音频，全部指向同一个 `coverExportDir`。
- 测试样本：同一 APIC mp3；同一 FLAC PICTURE；预先存在目标文件；只读 coverExportDir；父目录不存在。
- 验收标准：并发后最终 PNG 存在且大小稳定；无进程 crash；无 temp 文件残留；预先存在目标时不覆盖内容。
- 失败标准：`rename()` 覆盖仍存在；出现截断 PNG；并发留下 temp；只读目录导致未捕获异常。
- 回滚：回滚到上一个 commit 的 `AtomicWriteFileIfAbsent()`，但必须立即禁用 cover 导出或保留 exists fast path 以避免覆盖风险。

Step 4:

- 修改位置：`src/TagReader.cpp`，`AtomicWriteFileIfAbsent()` 写入循环。
- 修改内容：处理 partial write、`EINTR`、`ENOSPC`，确保任何失败路径关闭 fd 并 unlink temp。
- 风险：错误处理分支复杂，容易双 close 或遗漏 unlink。
- 回归影响：异常路径更稳定；成功路径不变。
- 验证方式：用 `ulimit -f` 或满磁盘测试目录模拟写失败；用只读目录模拟 open 失败。
- 测试样本：小 PNG；超过文件大小限制的 PNG；不可写导出目录。
- 验收标准：失败时 `Read()` 不 crash；metadata 仍可返回；cover path 为空或未设置；无 temp 残留。
- 失败标准：异常泄漏到顶层导致读取失败；fd 泄漏；temp 残留。
- 回滚：仅回滚写入循环重构，保留 no-replace 发布。

Step 5:

- 修改位置：`test/security/security_smoke.cpp`。
- 修改内容：增加 cover cache smoke：重复读取同一文件、并发读取同一文件、预先创建目标路径后确认不覆盖。若当前 smoke 只接受音频列表，则新增内部函数 `RunCoverCacheSmoke()`。
- 风险：并发测试在慢文件系统上可能偶发超时。
- 回归影响：测试时间增加，应限制进程数和样本大小。
- 验证方式：`./build/TagReaderSecuritySmoke <cover-export-dir> <audio-file-path>`。
- 测试样本：ID3 APIC mp3、FLAC PICTURE、MP4 covr。
- 验收标准：重复读取路径相同；mtime 不变；并发无覆盖；安全 smoke 返回 0。
- 失败标准：路径变化；mtime 变化；并发失败；stderr 出现未捕获 filesystem 异常。
- 回滚：回滚 smoke 新增用例，不回滚生产修复。

# Phase 3 - P0 ID3 Dispatch 与大帧内存控制

目标：
为什么现在做这一阶段。

ID3 是 MP3 主入口，当前未知或二进制帧会先走文本解码，造成 CPU 和内存 DoS。该阶段在不重写 ID3 parser 的前提下，先修正 dispatch，再减少大 frame 无必要拷贝。

修改内容：
精确到函数级。

- 修改 `ReadID3v22Frame()`。
- 修改 `ReadID3v2Frame()`。
- 修改 `ReadID3v23Or24Frames()`。
- 新增 helper：`IsId3v22SupportedTextFrame()`、`IsId3v23Or24SupportedTextFrame()`、`IsId3LyricsFrame()`、`IsId3PictureFrame()`。
- 新增 helper：`ReadID3v2FramePayloadView()` 或局部 `std::span<const uint8_t>` 分支，只有需要 unsync/grouping/data length indicator 时才复制。

步骤：

Step 1:

- 修改位置：`src/TagReader.cpp`，ID3 frame helper 区域，靠近 `ReadID3v22Frame()` 和 `ReadID3v2Frame()`。
- 修改内容：新增支持帧集合判断函数。metadata 文本帧只包含当前实际写入 `RawMetadata` 的帧：标题、歌手、专辑、专辑歌手、作曲、流派、年份、音轨、碟号。图片帧只包含 `PIC`/`APIC`。
- 示例代码结构：

```cpp
bool IsId3v23Or24SupportedTextFrame(std::string_view id) {
    return id == "TIT2" || id == "TPE1" || id == "TALB" || id == "TPE2" ||
           id == "TCOM" || id == "TCON" || id == "TDRC" || id == "TYER" ||
           id == "TRCK" || id == "TPOS";
}
```

- 风险：遗漏当前支持的字段会导致 metadata 回归。
- 回归影响：未知二进制帧被跳过；已支持文本帧保持读取。
- 验证方式：对每个支持字段构造最小 ID3v2.2、v2.3、v2.4 样本。
- 测试样本：含 `TIT2/TPE1/TALB/TRCK/TPOS/APIC` 的 mp3；含大 `GEOB/PRIV/MCDI/COMR` 的 mp3。
- 验收标准：支持字段完整读取；未知大帧不触发文本解码；cover 仍导出。
- 失败标准：任一已有支持字段丢失；未知大帧读取耗时接近修复前；APIC 不导出。
- 回滚：回滚 helper 和 dispatch 判断，保留 frame size guard。

Step 2:

- 修改位置：`ReadID3v22Frame()`，约 2744-2753 行。
- 修改内容：先判断 `PIC`，再判断是否支持文本帧；非支持帧直接 return，不调用 `ReadId3TextFrame()`。
- 风险：`ULT/SLT` 歌词帧不应在 metadata parser 中处理；歌词 parser 仍由 `ReadID3v22LyricsFrames()` 处理。
- 回归影响：metadata parser 更严格；lyrics parser 不变。
- 验证方式：ID3v2.2 metadata 与 lyrics 分别测试。
- 测试样本：`TT2/TP1/TAL/PIC` 正常文件；`ULT` plain lyrics；大未知 `GEO` 帧。
- 验收标准：metadata 字段仍读取；lyrics 仍读取；大未知帧跳过。
- 失败标准：lyrics 因 metadata dispatch 改动丢失；未知帧仍被解码。
- 回滚：仅回滚 `ReadID3v22Frame()` 的 dispatch 修改。

Step 3:

- 修改位置：`ReadID3v2Frame()`，约 2839-2849 行。
- 修改内容：先判断 `APIC`，再判断支持文本帧；非支持帧直接 return，不调用 `ReadId3TextFrame()`。`COMM`、`TXXX` 不通过 metadata 通用文本入口，避免二进制/描述字段误读。
- 风险：如果之前误把某些非标准字段当 metadata 读取，修复后会消失。
- 回归影响：符合当前公开字段矩阵，降低 DoS。
- 验证方式：ID3v2.3 和 v2.4 字段矩阵测试。
- 测试样本：`TIT2/TPE1/TALB/TPE2/TCOM/TCON/TDRC/TYER/TRCK/TPOS/APIC`；大 `GEOB/PRIV/MCDI/COMM/TXXX`。
- 验收标准：支持字段读取；`COMM/TXXX` 不进入 metadata 文本字段；大未知帧 CPU 和内存稳定。
- 失败标准：支持字段缺失；`COMM` 被错误写入 title 或 lyrics；未知帧仍分配大字符串。
- 回滚：仅回滚 `ReadID3v2Frame()` 的 dispatch 修改。

Step 4:

- 修改位置：`ReadID3v23Or24Frames()`，约 2724-2738 行。
- 修改内容：在复制 frame payload 前判断 frame id 和 flags。未知且不需要歌词/图片处理的 frame 直接跳过。对无 grouping、无 data length indicator、无 frame unsync 的已支持 frame 使用 `std::span<const uint8_t>` 传入 reader；只有需要修改 payload 时才复制 vector。
- 示例代码结构：

```cpp
const auto rawPayload = std::span(tagBytes).subspan(frameDataOffset, frameSize);
if (!ShouldProcessId3Frame(frameId, mode)) {
    cursor = frameEnd;
    continue;
}
std::span<const std::uint8_t> payload = rawPayload;
std::vector<std::uint8_t> owned;
if (requiresTransform) {
    owned.assign(rawPayload.begin(), rawPayload.end());
    ApplyId3FrameTransforms(owned, flags);
    payload = owned;
}
```

- 风险：函数签名变更会波及 `ReadID3v2Frame()`、lyrics frame reader、picture reader。
- 回归影响：内存峰值下降；行为应保持一致。
- 验证方式：最大 16 MiB ID3 tag，单个大未知 frame；单个大 APIC frame；带 frame-level unsync 的文本 frame。
- 测试样本：大 `PRIV`；大 `APIC`；v2.4 frame unsync `TIT2`；v2.3 grouping identity frame。
- 验收标准：未知 frame 不复制；需要 transform 的 frame 仍正确；APIC 导出正常；ASAN/UBSAN 无报错。
- 失败标准：frame flags 处理回归；APIC payload 错位；v2.4 unsync 文本乱码。
- 回滚：回滚 span 优化，保留 Step 1-3 的 dispatch 跳过。

Step 5:

- 修改位置：`test/corpus/generate_corpus.py`。
- 修改内容：新增 ID3 DoS corpus：v2.2 大未知帧、v2.3 大 `GEOB`、v2.4 大 `PRIV`、v2.4 frame-level unsync 文本、v2.4 tag-level unsync + footer 样本。
- 风险：seed 太大导致 corpus 生成慢。
- 回归影响：只影响测试。
- 验证方式：ASAN build 下运行 `TagReaderSecuritySmoke` 和短 fuzz。
- 测试样本：上述 deterministic seed。
- 验收标准：所有 seed 不 crash；大未知帧不造成明显内存峰值；支持字段仍读取。
- 失败标准：任何 seed crash；读取耗时异常；metadata 字段矩阵回归。
- 回滚：删除新增 seed，保留生产修复。

# Phase 4 - P0 歌词与 Ogg 资源上限

目标：
为什么现在做这一阶段。

歌词和 Ogg continuation 是最容易通过合法结构制造大量对象和重复 IO 的路径。该阶段先加硬上限和越界前置检查，不改变歌词 API，也不实现完整 Ogg demuxer。

修改内容：
精确到函数级。

- 修改 `ReadLyricsFromPlainText()`。
- 修改 `NormalizeLyrics()` 或 `BuildMusicTag()` 中 timed lyrics 输出前处理。
- 修改 `ReadID3v22LyricsFrames()`。
- 修改 `ReadID3v23Or24LyricsFrames()`。
- 修改 `ReadOggVorbisCommentEntries()`。
- 新增常量：`kMaxLyricLines`、`kMaxLrcTimestampsPerLine`、`kMaxPlainLyricsBytes`。

步骤：

Step 1:

- 修改位置：`src/TagReader.cpp`，常量区域。
- 修改内容：新增歌词输出限制。建议初始值：`kMaxLyricLines = 20000`，`kMaxLrcTimestampsPerLine = 32`，`kMaxPlainLyricsBytes = 1 MiB`。这些是内部安全阈值，不暴露 public API。
- 风险：极端长歌词可能被截断或降级。
- 回归影响：普通歌词不受影响；恶意歌词受限。
- 验证方式：边界样本正好等于上限、超过上限一个 token、超过总行数。
- 测试样本：普通 LRC 100 行；单行 32 个 timestamp；单行 33 个 timestamp；2 万行歌词；超过 1 MiB plain lyrics。
- 验收标准：上限内完整读取；超过上限时不 crash、不 OOM，并按设计降级为 plain 或停止追加。
- 失败标准：普通 LRC 丢行；超过上限仍生成几十万条 lyric。
- 回滚：回滚常量和 guard，但保留 Ogg 文件边界检查。

Step 2:

- 修改位置：`ReadLyricsFromPlainText()`，约 4184-4259 行。
- 修改内容：解析每行 timestamp 时限制每行 token 数；追加 timed lyrics 时限制总行数。超过 `kMaxLrcTimestampsPerLine` 时将该行作为 plain text 处理，或只保留前 N 个 timestamp；选择“只保留前 N 个 timestamp 并继续”，以最大限度保留同步歌词。
- 示例代码结构：

```cpp
if (timestamps.size() == kMaxLrcTimestampsPerLine) {
    reachedTimestampLimit = true;
    break;
}
...
if (timed.size() >= kMaxLyricLines) {
    return FinalizeLyrics(rawLyrics);
}
```

- 风险：同一行超过 32 个 timestamp 的合法 LRC 会丢弃后续 timestamp。
- 回归影响：普通 LRC 不变；恶意 timestamp 爆炸受限。
- 验证方式：统计输出 `Lyrics` 行数和内存占用。
- 测试样本：一行 10 万个 `[00:01.00]` 后接歌词；乱序 timestamp；重复 timestamp；普通多 timestamp 行。
- 验收标准：输出行数不超过 `kMaxLyricLines`；单行 timestamp 不超过 32 个输出；内存稳定；不 crash。
- 失败标准：输出行数无限增长；CPU 时间随 token 数线性巨大增长且无提前截断。
- 回滚：仅回滚 `ReadLyricsFromPlainText()` token 限制。

Step 3:

- 修改位置：`NormalizeLyrics()` 或 `BuildMusicTag()`，约 4690-4697 行。
- 修改内容：对 timed lyrics 按 timestamp 稳定排序，并去重完全相同的 `(timestamp,text)`。该步骤合并审计项 13 和 14，因为新增行数上限后需要统一输出 canonicalization，避免 parser 分支各自排序。
- 示例代码结构：

```cpp
std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
    return a.timestamp < b.timestamp;
});
lines.erase(std::unique(lines.begin(), lines.end(), SameTimestampAndText), lines.end());
```

- 风险：调用方如果依赖文件出现顺序，会观察到顺序变化。
- 回归影响：播放器语义更稳定；重复行减少。
- 验证方式：构造乱序和重复 LRC，检查输出递增。
- 测试样本：`[00:10]B` 后 `[00:01]A`；两个完全相同 `[00:01]A`；同 timestamp 不同文本。
- 验收标准：timestamp 非递减；完全重复行只保留一次；同 timestamp 不同文本保留稳定相对顺序。
- 失败标准：不同文本被误删；plain lyrics 被错误排序或拆分。
- 回滚：回滚排序去重，保留资源上限。

Step 4:

- 修改位置：`ReadID3v22LyricsFrames()` 和 `ReadID3v23Or24LyricsFrames()`。
- 修改内容：在 append SYLT/SLT timed lines 前检查 `kMaxLyricLines`；超过后停止追加当前歌词帧剩余内容。
- 风险：长同步歌词被截断。
- 回归影响：保护所有 ID3 timed lyrics 分支，不只 LRC。
- 验证方式：构造 SYLT/SLT 大量 timestamp 样本。
- 测试样本：ID3v2.3 `SYLT` 100 行；`SYLT` 25000 行；ID3v2.2 `SLT` 大量行。
- 验收标准：行数不超过上限；正常 SYLT 仍读取；超过上限不 crash。
- 失败标准：超过上限仍增长；正常 SYLT 全部丢失。
- 回滚：回滚 ID3 lyrics guard，保留 LRC guard。

Step 5:

- 修改位置：`ReadOggVorbisCommentEntries()`，约 3138-3175 行。
- 修改内容：计算 `nextCursor = payloadOffset + payloadSize` 后，在 `ReadRange()` 前检查溢出和 `nextCursor <= context.fileSize`。失败时返回 false，不执行无意义 IO。
- 示例代码结构：

```cpp
std::uintmax_t nextCursor = 0;
if (!TryAddUintmax(payloadOffset, payloadSize, nextCursor) || nextCursor > context.fileSize) {
    return false;
}
```

- 风险：无，当前短读失败路径提前化。
- 回归影响：malformed Ogg 更快失败。
- 验证方式：截断 Ogg page payload；payload size 恰好到 EOF；payload size 超过 EOF。
- 测试样本：超大 Ogg continuation packet；segment table 声明 255*255 payload 但文件短；合法小 Ogg Vorbis comment。
- 验收标准：合法 Ogg 仍读取；截断 Ogg 不 crash；不会反复 seek/read 越界范围。
- 失败标准：合法 Ogg 被拒绝；截断样本抛出未捕获异常。
- 回滚：单独回滚 Ogg boundary guard。

# Phase 5 - P1 MP4 Atom 一致性修复

目标：
为什么现在做这一阶段。

MP4 当前有多个 data atom 只处理第一个、metadata walker 递归而 lyrics walker 显式栈、`meta` full box 未验证。该阶段先修复行为 bug，再做最小 walker 统一，避免一次性拆 God Object。

修改内容：
精确到函数级。

- 修改 `ReadMP4ItemAtom()`。
- 修改 `ReadMP4DataAtom()`。
- 修改 `ReadMP4AtomTree()`。
- 修改 `ReadMP4LyricsAtomTree()`。
- 新增 helper：`ReadMp4MetaChildRange()`、`ForEachMp4ChildAtom()`、`WalkMp4IlstItems()`。
- 新增 callback struct：`Mp4ItemCallbacks`，包含 metadata item callback 和 lyrics item callback。

步骤：

Step 1:

- 修改位置：`ReadMP4ItemAtom()`，约 3470-3497 行。
- 修改内容：删除“第一个 child 是 `data` 就立即 return”的特殊分支，统一 child scan loop。每个 `data` atom 独立调用 `ReadMP4DataAtom()`；当前字段已成功填充后仍继续扫描 siblings，但不覆盖已填字段，保持 first-valid-wins。
- 示例代码结构：

```cpp
for (auto childOffset = payloadOffset; childOffset < atomEnd;) {
    auto child = ReadMp4AtomHeader(...);
    if (child.type == "data") {
        ReadMP4DataAtom(context, itemType, child.payloadOffset, child.payloadSize, metadata);
    }
    childOffset = child.endOffset;
}
```

- 风险：多个有效 `data` atom 时字段选择策略需要固定。
- 回归影响：无效第一个 data 不再屏蔽后续有效 data；已有单 data 行为不变。
- 验证方式：构造 item 第一个 data 空、第二个 data 有效；两个有效 data。
- 测试样本：MP4 `©nam` first data empty second `Title`；`©ART` two valid data；`covr` first invalid second valid PNG/JPEG。
- 验收标准：第二个有效 data 被读取；两个有效 data 保留第一个有效值；cover 能从后续 valid data 导出。
- 失败标准：仍忽略第二个 data；覆盖 first-valid 字段；atom scan 死循环。
- 回滚：仅回滚 `ReadMP4ItemAtom()` loop 修改。

Step 2:

- 修改位置：新增 `ReadMp4MetaChildRange()`，并在 `ReadMP4AtomTree()` 与 `ReadMP4LyricsAtomTree()` 中调用。
- 修改内容：遇到 `meta` atom 时读取 4 字节 full box header，验证 payload 至少 4 字节。对 version/flags 做显式记录；当前接受 version 0，其它 version 不递归或按 Unknown meta 跳过。
- 示例代码结构：

```cpp
std::optional<Mp4Range> ReadMp4MetaChildRange(const ReadContext& context,
                                              const Mp4AtomHeader& atom) {
    if (atom.payloadSize < 4) return std::nullopt;
    auto fullBox = ReadRange(context.input, atom.payloadOffset, 4, 4);
    if (!fullBox) return std::nullopt;
    if ((*fullBox)[0] != 0) return std::nullopt;
    return Mp4Range{atom.payloadOffset + 4, atom.atomEnd};
}
```

- 风险：某些非 version 0 的现实文件会被跳过。
- 回归影响：非法 `meta` 不再隐式接受；合法 iTunes metadata 不变。
- 验证方式：`meta` payload 小于 4、version 0、version 1、flags 非零样本。
- 测试样本：正常 M4A ilst；`meta` payload 3 bytes；`meta` version 1；深度嵌套 atom 的恶意 m4a。
- 验收标准：正常 M4A 仍读取；短 `meta` 不 crash；version 异常时安全跳过。
- 失败标准：正常 iTunes metadata 全部丢失；短 `meta` 造成 OOB 或异常。
- 回滚：回滚 full box 验证，保留 multi-data fix。

Step 3:

- 修改位置：新增 `ForEachMp4ChildAtom()`，并逐步替换 `ReadMP4AtomTree()` 内部 while。
- 修改内容：把 atom header 读取、atom count、size 0、parent limit、next offset 检查集中到一个 helper。metadata walker 仍可保留原函数名，但内部使用 helper。
- 风险：通用 helper 错误会影响 metadata 和 lyrics 两条路径。
- 回归影响：减少重复边界逻辑。
- 验证方式：MP4 atom size 0、extended size 1、too-small atom、atomEnd 超出 parent。
- 测试样本：合法 M4A；atom size 0 at root；atom size 0 inside parent；extended 64-bit atom；overlapping atom。
- 验收标准：合法文件读取；非法 atom 安全停止；atom count limit 生效。
- 失败标准：死循环；合法文件字段丢失；非法 size 触发异常泄漏。
- 回滚：回滚 helper 替换，保留原 walker。

Step 4:

- 修改位置：`ReadMP4AtomTree()` 和 `ReadMP4LyricsAtomTree()`。
- 修改内容：将 metadata 递归 walker 改为显式 stack，复用 `Mp4PathState` 和 `ForEachMp4ChildAtom()`。lyrics walker 暂保留函数名，但内部调用同一 walker，callback 分别处理 metadata item 和 lyrics item。
- 示例代码结构：

```cpp
struct Mp4ItemCallbacks {
    std::function<void(const Mp4AtomHeader&, Mp4PathState)> onMetadataItem;
    std::function<void(const Mp4AtomHeader&, Mp4PathState)> onLyricsItem;
};

void WalkMp4IlstItems(ReadContext& context, Mp4ItemCallbacks callbacks);
```

- 风险：这是本阶段最大回归点，可能影响 MP4 metadata 和 lyrics 同时读取。
- 回归影响：行为应一致化；递归风险消除；重复代码减少但不拆文件。
- 验证方式：MP4 metadata-only、lyrics-only、metadata+lyrics、freeform lyrics 文件。
- 测试样本：`©nam/©ART/©alb/covr` M4A；`©lyr` M4A；iTunes freeform lyrics；深度嵌套恶意 m4a。
- 验收标准：metadata 和 lyrics 均与修改前一致或更完整；深度嵌套不递归 crash；atom limit 统一。
- 失败标准：MP4 lyrics 丢失；metadata 丢失；walker 死循环或 stack 增长失控。
- 回滚：回滚显式 stack 统一，保留 Step 1 和 Step 2 的局部修复。

Step 5:

- 修改位置：`test/corpus/generate_corpus.py`。
- 修改内容：新增 MP4 corpus：multi-data item、meta payload too small、meta version 1、extended atom、deep nesting、covr invalid then valid。
- 风险：构造 MP4 atom 时 size 计算错误会导致无效 seed 不能覆盖目标路径。
- 回归影响：只影响测试。
- 验证方式：短 fuzz 加安全 smoke。
- 测试样本：上述 deterministic m4a。
- 验收标准：fuzz 不 crash；multi-data oracle 通过；deep nesting 不递归。
- 失败标准：seed 无法被容器识别；测试没有覆盖 `ReadMP4ItemAtom()`。
- 回滚：删除新增 corpus seed。

# Phase 6 - P1 ID3v2.4 Unsync 与 Parser 一致性

目标：
为什么现在做这一阶段。

ID3v2.4 tag-level unsynchronization 当前在 frame region 划分前处理，容易破坏 extended header/footer offset。该阶段在 ID3 dispatch 和大帧控制完成后执行，因为它会触碰 ID3 tag/frame 区间逻辑。

修改内容：
精确到函数级。

- 修改 `ReadId3TagBytes()`。
- 修改 `PrepareId3v24FrameRegion()`。
- 修改 `ReadID3v23Or24Frames()`。
- 新增 struct：`Id3TagPayloadView`，字段包含 `version`、`flags`、`rawPayload`、`frameOffset`、`frameSize`、`tagUnsync`。
- 新增 helper：`ApplyId3UnsyncIfNeeded()`。

步骤：

Step 1:

- 修改位置：`ReadId3TagBytes()`。
- 修改内容：读取 tag payload 后不要立即对整个 `tagBytes` 做 tag-level unsync。返回原始 payload 和 header flags，保持现有 public 函数名时可新增 out parameter 或新 struct。
- 风险：函数签名变化会影响 metadata 和 lyrics 两条 ID3 调用链。
- 回归影响：为正确 frame region 计算做准备；暂不改变 frame 解码。
- 验证方式：ID3v2.3、v2.4 无 unsync 样本必须完全一致。
- 测试样本：v2.3 普通 tag；v2.4 普通 tag；v2.4 tag-level unsync tag。
- 验收标准：无 unsync 样本字段不变；tag payload size 检查仍生效。
- 失败标准：普通 ID3v2 文件全部读不到；tag size limit 失效。
- 回滚：回滚 `ReadId3TagBytes()` struct 化。

Step 2:

- 修改位置：`PrepareId3v24FrameRegion()`。
- 修改内容：在原始 tag payload 上先处理 footer 和 extended header，得到 frame region offset/size。不要在此函数中删除 unsync 字节。
- 示例代码结构：

```cpp
Id3FrameRegion PrepareId3v24FrameRegion(std::span<const uint8_t> rawPayload,
                                        uint8_t flags) {
    auto end = rawPayload.size();
    if (flags & kId3FooterFlag) end -= 10;
    auto begin = 0;
    if (flags & kId3ExtendedHeaderFlag) begin += ParseExtendedHeaderSize(rawPayload);
    return {begin, end - begin};
}
```

- 风险：extended header size 解析必须保留 syncsafe 检查。
- 回归影响：v2.4 footer/extended header 更符合 spec。
- 验证方式：footer、extended header、两者组合、截断 extended header。
- 测试样本：v2.4 footer only；extended header only；footer+extended header；malformed extended header。
- 验收标准：合法组合读取字段；malformed 安全失败；不 OOB。
- 失败标准：footer 样本错位；extended header 样本 crash。
- 回滚：回滚 frame region 计算。

Step 3:

- 修改位置：`ReadID3v23Or24Frames()`。
- 修改内容：对 v2.4 frame payload 应用 frame-level unsync flag；如果 tag-level unsync 置位且 frame-level 未置位，则仅对 frame payload 应用 unsync，不处理 frame header/extended header/footer。
- 风险：不同 ID3v2.4 文件对 tag-level unsync 的现实写法不一致，需要兼容但不能破坏结构。
- 回归影响：合法 tag-level unsync 文件读取更稳定。
- 验证方式：frame payload 含 `0xFF 0x00` 的标题和 APIC；frame header 附近含 `0xFF 0x00` 的 malformed 样本。
- 测试样本：v2.4 tag-level unsync `TIT2`；v2.4 frame-level unsync `TIT2`; v2.4 APIC unsync；v2.4 footer+unsync。
- 验收标准：payload unsync 后文本正确；frame offset 不错位；APIC 不损坏。
- 失败标准：frame header 被误删字节；footer 样本字段丢失；APIC decode 失败。
- 回滚：回滚 v2.4 unsync 新策略，恢复旧整体 unsync，但保留 dispatch DoS 修复。

Step 4:

- 修改位置：`test/corpus/generate_corpus.py`。
- 修改内容：新增 ID3v2.4 unsync matrix seed。
- 风险：构造样本复杂，容易 seed 自身不合法。
- 回归影响：只影响测试。
- 验证方式：用 `TagReaderTest` 打印字段，并用 fuzz 短跑。
- 测试样本：tag-level unsync、frame-level unsync、footer、extended header、APIC。
- 验收标准：合法样本字段读取；malformed 样本不 crash。
- 失败标准：合法样本被全部拒绝；fuzz crash。
- 回滚：删除新增 seed。

# Phase 7 - P1 FLAC、Vorbis 与路径/全局状态收敛

目标：
为什么现在做这一阶段。

在主要 crash、OOM、race 修复后，处理低风险但影响长期可维护性的 parser consistency 和库副作用。该阶段不改变 facade API。

修改内容：
精确到函数级。

- 修改 `ReadFlacPictureEntry()`。
- 修改 `ReadVorbisCommentEntry()`。
- 修改 `ReadVorbisLyricsEntry()`。
- 修改 `TagReader::Read()`。
- 修改 `ValidatePath()` 和 `OpenContext()` 的职责说明或最小实现。

步骤：

Step 1:

- 修改位置：`ReadFlacPictureEntry()`，约 3318-3363 行。
- 修改内容：把 `need(n)` 改为 overflow-safe 写法 `p <= pictureSize && n <= pictureSize - p`。同函数内所有 `p += n` 前先调用 `need(n)`。
- 风险：无行为风险。
- 回归影响：只提升 defensive coding。
- 验证方式：FLAC picture mimeLen、descLen、imageLen 边界。
- 测试样本：合法 FLAC PICTURE；descLen 截断；mimeLen 截断；imageLen 超出 block。
- 验收标准：合法 cover 导出；截断 block 安全失败。
- 失败标准：合法 FLAC cover 丢失；截断样本 crash。
- 回滚：单独回滚 lambda 修改。

Step 2:

- 修改位置：`ReadVorbisCommentEntry()` 和 `ReadVorbisLyricsEntry()`。
- 修改内容：保持默认严格 UTF-8，但把 key/value 失败隔离到字段级。单个 entry UTF-8 失败只跳过该 entry，不影响后续 entry；如果当前已经是该行为，则增加测试锁定，不改生产代码。
- 风险：如果加入 fallback 会违反默认 strict 策略；本阶段禁止 legacy fallback。
- 回归影响：字段级失败更可预测。
- 验证方式：一个非法 UTF-8 entry 后跟合法 `TITLE`。
- 测试样本：Vorbis comment 第一项非法 key；第一项非法 value；后续合法 title/artist；非法 lyrics 后合法 metadata。
- 验收标准：非法 entry 被跳过；后续合法字段读取；输出仍 UTF-8。
- 失败标准：一个非法 entry 导致整个 comment block 丢失；输出非法 UTF-8。
- 回滚：回滚任何 fallback 代码，保留测试。

Step 3:

- 修改位置：`TagReader::Read()`，约 816-823 行。
- 修改内容：移除库内部无条件 `av_log_set_level(AV_LOG_QUIET)`。如果测试程序需要静默，在 `test/main.cpp` 和 `test/security/security_smoke.cpp` 设置 FFmpeg log level。
- 风险：宿主程序运行时可能重新看到 FFmpeg 日志。
- 回归影响：消除库级全局副作用；测试程序仍可静默。
- 验证方式：多线程调用 TagReader 同时读取外部 FFmpeg log level。
- 测试样本：普通 mp3；故意 malformed 文件触发 FFmpeg probe 日志。
- 验收标准：`TagReader::Read()` 不修改全局 log level；测试程序输出仍可控。
- 失败标准：库仍修改全局状态；测试输出大量噪声导致验收困难。
- 回滚：只在确有用户需求时恢复，但应改为显式配置；不要恢复无条件全局修改。

Step 4:

- 修改位置：`ValidatePath()` 和 `OpenContext()`，约 839-941 行。
- 修改内容：保持 path 早期错误提示，但删除或弱化“permissions bits 等于可读性”的判断。真正可读性以 `ifstream.open()` 和 `avformat_open_input()` 结果为准。补充代码注释：`ValidatePath()` 不是安全授权检查。
- 风险：某些不可读文件错误信息从 permission error 变为 open failure。
- 回归影响：减少 TOCTOU 误导，不改变成功路径。
- 验证方式：不存在文件、目录路径、权限不足文件、打开后被替换文件。
- 测试样本：普通文件；目录；无权限文件；symlink race 手工测试。
- 验收标准：普通文件读取；目录拒绝；不可读文件在 open 阶段失败；无安全授权暗示。
- 失败标准：目录被当作文件；不可读文件导致未捕获异常；普通文件被误拒。
- 回滚：恢复原权限预检查，但保留注释说明 TOCTOU。

# Phase 8 - P2 渐进式结构清理与测试固定

目标：
为什么现在做这一阶段。

所有 P0/P1 稳定性修复完成后，再做小步结构清理。禁止全面拆 God Object；只抽取低风险 helper/type，让后续 parser 修复更容易测试。

修改内容：
精确到函数级。

- 新增内部 bounded cursor：`ByteCursor`，先只用于新改过的 FLAC picture 和 MP4 data payload 局部，不全量替换。
- 新增 parser-local result enum：`ParseStatus { Ok, NotFound, Malformed, ResourceLimit }`，先只用于 MP4 walker 内部，不改 public API。
- 补充 `test/corpus/generate_corpus.py` 覆盖所有已修复风险。
- 补充 `DESIGN.md` 中的安全边界和 cover cache 模型。

步骤：

Step 1:

- 修改位置：`src/TagReader.cpp` 内部 helper 区域。
- 修改内容：新增 `ByteCursor`，只提供 `remaining()`、`readU32Be()`、`readBytes(n)`、`skip(n)`，所有方法使用 `n <= remaining()` 模式。第一轮只迁移 `ReadFlacPictureEntry()`，不要一次迁移 ID3/MP4/Ogg。
- 风险：抽象错误会影响 FLAC cover。
- 回归影响：局部减少手写 cursor 算术。
- 验证方式：FLAC picture 边界 corpus。
- 测试样本：合法 PICTURE；截断 mime；截断 desc；截断 image。
- 验收标准：行为与 Phase 7 一致；代码中无 `p + n <= pictureSize` 模式。
- 失败标准：合法 FLAC cover 丢失；截断样本 crash。
- 回滚：回滚 `ByteCursor` 迁移，保留 Phase 7 safe lambda。

Step 2:

- 修改位置：MP4 walker helper 区域。
- 修改内容：新增 `ParseStatus` 仅用于 `ForEachMp4ChildAtom()` 和 `WalkMp4IlstItems()` 内部，区分 malformed 与 not found。`ReadMetadata()` 和 `ReadLyrics()` 仍按现有策略吞掉 runtime/filesystem 错误，不改变 public 行为。
- 风险：状态映射错误导致 malformed 被当作成功。
- 回归影响：便于 fuzz oracle 和日志调试，但对 API 无变化。
- 验证方式：合法、not found、malformed、resource limit 四类 MP4 样本。
- 测试样本：无 `ilst` 的 m4a；非法 atom size；atom count 超限；合法 metadata。
- 验收标准：内部状态正确；public 仍返回可用 `MusicTag` 或空字段；无 crash。
- 失败标准：not found 抛异常；malformed 继续越界解析。
- 回滚：回滚 enum 状态，恢复 bool 返回。

Step 3:

- 修改位置：`test/corpus/generate_corpus.py` 和 `test/corpus/README.md`。
- 修改内容：把 Phase 1-7 的 corpus 分类写入脚本和 README：ID3 dispatch、ID3 unsync、MP4 atom、Ogg truncation、FLAC picture、lyrics DoS、cover cache。
- 风险：README 与脚本不一致。
- 回归影响：测试资产可复现。
- 验证方式：删除旧 corpus 输出目录后重新生成。
- 测试样本：脚本生成的全部 seed。
- 验收标准：生成目录包含每一类 seed；README 描述与文件名一致；fuzz target 可读取全部 seed。
- 失败标准：脚本失败；README 提到不存在的 seed；seed 不是 deterministic。
- 回滚：回滚文档和脚本分类改动。

Step 4:

- 修改位置：`DESIGN.md`。
- 修改内容：记录稳定后的设计事实：单入口 facade 不变；metadata/lyrics 直接读原始字节；cover cache 为 content-addressed PNG storage；hash 基于 embedded image bytes；已存在路径禁止重复 transcode；parser malformed 策略是局部失败并保留 `Read()` 可用性。
- 风险：文档提前描述未完成代码会误导。
- 回归影响：无代码影响。
- 验证方式：对照代码确认每条文档事实已实现。
- 测试样本：不适用。
- 验收标准：文档不声称不存在的测试框架、CI、formatter；与代码一致。
- 失败标准：文档包含未来计划当作已实现；提到未配置工具。
- 回滚：回滚文档修改。

# Phase 9 - 总体验收与发布前冻结

目标：
为什么现在做这一阶段。

所有修复完成后，需要用固定命令和固定输入证明稳定化目标完成。该阶段不再引入功能改动，只修复验收发现的回归。

修改内容：
精确到函数级。

- 不主动修改生产函数。
- 如验收失败，只回到对应 Phase 的函数做最小修复。
- 检查 `ANALYSIS.md` 中 P0/P1/P2 项是否已映射到代码、测试或明确延后。

步骤：

Step 1:

- 修改位置：无。
- 修改内容：普通构建验证。
- 风险：本地 FFmpeg/pkg-config 环境缺失。
- 回归影响：无。
- 验证方式：运行 `cmake -S . -B build`，然后 `cmake --build build`。
- 测试样本：无。
- 验收标准：`TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke` 构建成功。
- 失败标准：任一目标构建失败。
- 回滚：根据编译错误回滚最近 Phase 的最小改动。

Step 2:

- 修改位置：无。
- 修改内容：ASAN/UBSAN 构建验证。
- 风险：sanitizer 暴露已有第三方库噪声。
- 回归影响：无。
- 验证方式：运行 `cmake -S . -B build-asan -DTAGREADER_ENABLE_SANITIZERS=ON`，然后 `cmake --build build-asan`。
- 测试样本：普通 mp3、ID3 APIC mp3、FLAC PICTURE、Ogg Vorbis comments、MP4 covr、恶意 corpus 全集。
- 验收标准：ASAN/UBSAN 无报错；malformed 输入不 crash。
- 失败标准：任何 sanitizer 报告来自本项目代码；OOM；未捕获异常。
- 回滚：定位到对应 Phase，回滚或修正最小范围。

Step 3:

- 修改位置：无。
- 修改内容：fuzz corpus 生成与短跑。
- 风险：Clang 或 libFuzzer 不可用。
- 回归影响：无。
- 验证方式：运行 `python3 test/corpus/generate_corpus.py`，再运行 `cmake -S . -B build-fuzz-clang -DCMAKE_CXX_COMPILER=clang++ -DTAGREADER_ENABLE_SANITIZERS=ON -DTAGREADER_ENABLE_FUZZING=ON`，然后 `cmake --build build-fuzz-clang`，最后用生成 corpus 短跑 `TagReaderFuzz`。
- 测试样本：生成目录 `/tmp/opencode/tagreader_fuzz_corpus`。
- 验收标准：corpus 生成成功；fuzz target 构建成功；短跑无 crash、无 leak、无 timeout。
- 失败标准：fuzz seed 触发 crash；LSAN 报 iconv 或 cover path 泄漏；单 seed 长时间卡住。
- 回滚：先隔离触发 seed，再回滚对应 Phase 的最小修改。

Step 4:

- 修改位置：无。
- 修改内容：功能回归验收。
- 风险：仓库没有真实音频样本，需使用人工准备样本或 corpus 中可识别样本。
- 回归影响：无。
- 验证方式：运行 `./build/TagReaderTest <audio-file-path> [cover-export-dir]`。
- 测试样本：一个 ID3v2.4 带 APIC 的 mp3；一个 ID3v2.3 带 SYLT 的 mp3；一个 FLAC 带 Vorbis Comment 和 PICTURE；一个 Ogg Vorbis comment；一个 MP4/M4A 带 `©nam/©ART/©lyr/covr`；一个深度嵌套 atom 的恶意 m4a；一个超大 Ogg continuation packet；一个 LRC timestamp 爆炸样本。
- 验收标准：合法文件 metadata、lyrics、media info 仍读取；cover dedup 成功；恶意文件不 crash；内存占用稳定；重复 cover 不重复 transcode。
- 失败标准：合法主格式任一完全不可读；cover 路径不稳定；恶意样本 crash/OOM；重复读取修改 PNG mtime。
- 回滚：按失败格式回滚对应 Phase，不做全仓库 reset。

Step 5:

- 修改位置：`TASKS.md`。
- 修改内容：在每个 Phase 完成后标记实际 commit 或变更摘要。不要在代码未实现时把任务标为完成。
- 风险：文档状态与代码不一致。
- 回归影响：无。
- 验证方式：逐项对照 `git diff` 和测试结果。
- 测试样本：不适用。
- 验收标准：所有 P0 已完成并验证；P1 已完成或有明确测试覆盖；P2 仅在 P0/P1 稳定后完成。
- 失败标准：文档声称完成但代码或测试未覆盖。
- 回滚：修正文档状态，不回滚代码。
