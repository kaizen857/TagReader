# TagReader 安全审计报告

## 一、项目真实架构简述

本次审计以当前工作区源码为准，重点覆盖 `include/`、`src/`、`test/`、`CMakeLists.txt` 和 `docs/DESIGN.md`。`README.md` 只有标题，不作为架构来源。

对外 API 只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`，实现位于 `src/TagReader.cpp`，直接转发到 `tagreader_core::ReadTag()`。真实主链路是：`ValidatePath()` -> `OpenContext()` -> `ValidateCoverExportDir()` -> `DetectStream()` -> `DetectTagFormat()` -> `ContainerFromTagFormat()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。

IO 模型分成两层：FFmpeg 在 `src/media/FfmpegSession.cpp` 和 `src/media/MediaInfoReader.cpp` 中负责打开/probe、查找音频流和读取基础媒体信息；标签字段、歌词和封面块由项目自己的 raw-byte parser 通过 `ReadContext::input` 与 `ReadRange()` 按绝对 offset 读取。`ReadContext` 同时保存 `std::ifstream input`、`AVFormatContext`、文件大小、最后修改时间、容器名和封面导出目录。

高风险解析器集中在 `src/formats/`：ID3、FLAC/Vorbis Comment、Ogg Vorbis、MP4 atom/ilst、APEv2。文本编码统一进入 `src/text/TextCodec.cpp`，最终字段和歌词规范化在 `src/text/TextNormalize.cpp`。封面由各 parser 抽取原始图片字节后进入 `src/cover/CoverCache.cpp`，再调用 `src/cover/CoverDecoder.cpp` 使用 FFmpeg 解码并统一输出 PNG。

封面副作用是当前安全边界中最重要的文件系统写入面。单参数 `Read(path)` 会默认使用 `std::filesystem::temp_directory_path() / "tagreader-covers"`；双参数 `Read(path, coverExportDir)` 使用调用方目录。默认放在系统临时目录本身有合理产品动机：多数 Linux 机器的 `/tmp` 为 tmpfs，可减少测试用户“用后不管”场景下的物理磁盘垃圾文件。风险点不在使用临时目录或 tmpfs，而在共享临时目录下使用固定、可预测且未加固的默认根目录。缓存路径为 `coverExportDir / first2hex / rest.png`，key 基于内嵌图片原始字节的 SHA-256。

## 二、漏洞与缺陷详情

### 🟠 High：默认封面导出目录可被共享临时目录 symlink 劫持

- **Bug 类别 & 触发位置**：CWE-59 / CWE-377，符号链接跟随与不安全临时目录；触发位置为 `tagreader_core::DefaultCoverExportDir()`、`ValidateCoverExportDir()`、`tagreader_cover::WriteCoverAsPng()`。
- **触发位置证据**：`src/core/TagPipeline.cpp:37` 返回系统临时目录下固定子目录；`src/core/TagPipeline.cpp:96` 只用 `create_directories()`、`exists()`、`is_directory()` 和探针文件验证目录；`src/cover/CoverCache.cpp:398` 直接在该 root 下拼接 hash shard 路径。
- **漏洞描述与触发原理**：`Read(path)` 在调用方未显式传入封面目录时使用系统临时目录下的固定默认路径。这一默认位置有利于利用 tmpfs 并避免测试垃圾堆积到物理磁盘，但固定路径位于共享命名空间，攻击者若能在同机共享临时目录中预先创建 `tagreader-covers` 为指向攻击者目录的 symlink，`ValidateCoverExportDir()` 会把 symlink 目标当作普通目录接受，后续封面 PNG 会实际写入 symlink 目标。当前代码会校验叶子缓存文件内容，能防止简单缓存污染/覆盖，但没有阻止默认缓存根被重定向。
- **极端破坏场景推演**：在多用户机器、服务进程或提权包装器场景中，低权限用户先创建 `/tmp/tagreader-covers -> /tmp/attacker-owned-dir`。高权限或不同安全域的进程随后调用单参数 `Read()` 解析带封面的音频，生成的 PNG 缓存文件由受害进程权限写入攻击者指定目录。由于缓存文件名可由嵌入图片字节决定，攻击者可以制造可预测路径的 victim-owned 文件。当前不构成任意文件覆盖，但构成缓存根逃逸与权限边界混淆。
- **验证结果**：已用隔离 `TMPDIR=/tmp/opencode/tagreader_audit_tmp` 复现。预先创建 `tagreader-covers` symlink 指向 `/tmp/opencode/tagreader_audit_hijack` 后，运行 `./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_export_base.mp3`，返回的 `coverPath` 位于 symlink 路径，实际 PNG 文件出现在 symlink 目标目录。
- **修复建议**：可以继续保留“默认落在 tmpfs/系统临时区域，方便测试用户用后不管”的产品目标，但不要使用共享 `/tmp` 下固定且未加固的目录。优先使用 `XDG_RUNTIME_DIR/tagreader-covers` 这类 per-user、通常为 `0700` 且常见为 tmpfs 的运行时目录；fallback 可使用 `/tmp/tagreader-covers-$UID` 或 `/tmp/tagreader-covers/<uid>`，创建后校验 owner、权限 `0700`，并拒绝 symlink。若不需要跨进程复用缓存，可用 `mkdtemp` 风格随机目录进一步降低抢占风险。显式目录可按现有设计继续信任调用方，但默认目录必须额外加固。

```cpp
std::filesystem::path DefaultCoverExportDir()
{
    if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] != '\0')
    {
        return std::filesystem::path(runtime) / "tagreader-covers";
    }

    return std::filesystem::temp_directory_path() / ("tagreader-covers-" + std::to_string(::getuid()));
}

void RejectSymlinkDirectory(const std::filesystem::path &dir)
{
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(dir, ec);
    if (ec || std::filesystem::is_symlink(status))
    {
        throw std::runtime_error("cover export directory symlink is not trusted: " + dir.string());
    }
}
```

### 🟡 Medium：输入文件路径预检与双重打开存在 TOCTOU / split-brain 竞态

- **Bug 类别 & 触发位置**：CWE-367 / CWE-59，检查时间与使用时间竞态；触发位置为 `ValidatePath()` 与 `OpenContext()`。
- **触发位置证据**：`src/core/TagPipeline.cpp:67` 使用 `exists()` 与 `is_regular_file()` 预检；`src/media/FfmpegSession.cpp:49` 再次用 `is_symlink()` 拒绝符号链接；`src/media/FfmpegSession.cpp:71` 使用 `std::ifstream` 按路径打开；`src/media/FfmpegSession.cpp:78` 又把同一路径字符串传给 `avformat_open_input()`。
- **漏洞描述与触发原理**：文件路径由外部调用方提供，属于不可信输入。当前代码通过路径名做多次检查和多次打开，检查与使用之间没有用 fd 固定目标文件。攻击者若控制目标目录或能并发替换路径，可让预检看到文件 A，而 `ifstream` 或 FFmpeg 实际打开文件 B；还可能出现 `ReadContext::input` 和 `AVFormatContext` 指向不同文件的 split-brain 状态。
- **极端破坏场景推演**：服务进程读取用户可写目录中的音频路径。攻击者在 `is_regular_file()` / `is_symlink()` 之后快速替换为 symlink 或其他普通文件，使 FFmpeg probe 与 raw-byte parser 处理的不是同一对象。轻则解析结果混乱，重则绕过 symlink 拒绝策略，将 FFmpeg 暴露给原本不应处理的文件或设备路径。由于 public API 返回 metadata 而非原始字节，本次未证明任意文件内容泄漏，因此严重度低于 High。
- **验证结果**：已有回归 `./build/TagReaderRegressionTests TR-AUDIT-029` 通过，说明“直接传入 symlink”会被拒绝；该测试不覆盖检查后替换的竞态窗口。本次审计静态确认竞态窗口仍存在。
- **修复建议**：用描述符语义替代路径二次打开。POSIX 下可 `open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)`，随后 `fstat()` 确认 `S_ISREG`，文件大小/mtime 均来自同一 fd；raw-byte 读取使用 fd 或由 fd 构造的流；FFmpeg 使用自定义 `AVIOContext` 或 `/proc/self/fd/<fd>`，确保 FFmpeg 与 parser 指向同一文件对象。

```cpp
int fd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
if (fd < 0) {
    throw std::runtime_error("failed to open input without following symlinks");
}

struct stat st {};
if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    throw std::runtime_error("input is not a trusted regular file");
}
```

### 🟡 Medium：显式封面导出目录接受 symlink，导致调用方目录边界依赖外部约定

- **Bug 类别 & 触发位置**：CWE-59 / 架构缺陷，显式目录信任边界不够清晰；触发位置为 `ValidateCoverExportDir()` 与测试 `TR-AUDIT-031`。
- **触发位置证据**：`src/core/TagPipeline.cpp:115` 使用 `std::filesystem::is_directory()`，它会跟随目录 symlink；`test/regression/regression_tests.cpp:4097` 创建目录 symlink；`test/regression/regression_tests.cpp:4108` 调用 `TagReader::Read(samplePath, symlinkExportDir)`；`test/regression/regression_tests.cpp:4116` 明确期望 symlink 目录 accepted 或 platform-skip。
- **漏洞描述与触发原理**：显式 `coverExportDir` 是外部输入。当前项目文档把显式目录 symlink 视为调用方信任目录，这是可接受的 API 约定，但在安全敏感调用方中容易被误用。如果调用方把用户可控目录传给 `Read(path, coverExportDir)`，封面缓存会写入 symlink 目标，突破调用方以为的目录边界。
- **极端破坏场景推演**：上层服务把用户提供的导出目录限制为某个工作区路径，但没有先做 `symlink_status()` / realpath 约束。攻击者在工作区内放置目录 symlink 指向工作区外可写位置，TagReader 将封面写到外部位置。叶子文件仍受缓存验证和 hard-link 发布约束，不是任意覆盖；风险在于目录边界绕过。
- **验证结果**：`./build/TagReaderRegressionTests TR-AUDIT-031` 输出 `symlink-dir-accepted` 并 PASS，确认这是当前设计行为，不是偶发实现偏差。
- **修复建议**：若库要默认安全，应新增严格模式或直接拒绝显式 symlink 目录；若保持当前 API 约定，则必须在 public 文档中把“显式目录 symlink 会被跟随”标为安全前置条件。更安全的实现是对显式目录也执行 `symlink_status()`，并用 `openat()`/目录 fd 约束 shard 与最终文件创建。

### 🔵 Low：FLAC 元数据块 fail-fast 会放大局部损坏影响

- **Bug 类别 & 触发位置**：鲁棒性缺陷 / 异常状态机过宽；触发位置为 `ReadFlacMetadataBlocks()` 与 `ReadMetadata()`。
- **触发位置证据**：`src/formats/flac/FlacParser.cpp:164` 逐块扫描 FLAC metadata；遇到截断块、读取失败或非法 Vorbis comment 会 `throw std::runtime_error`；`src/core/TagPipeline.cpp:175` 的 `ignoreMalformedMetadata` 捕获 `runtime_error` 后仅记录诊断并继续返回部分 metadata。
- **漏洞描述与触发原理**：格式解析策略要求局部 malformed 字段/歌词/封面尽量局部失败。FLAC metadata 块当前在部分损坏时中断整个 metadata 扫描，攻击者可用一个坏块阻止后续合法字段或封面被读取。由于异常被顶层吞掉，调用方只看到字段缺失，不容易区分真实空字段和被破坏字段。
- **极端破坏场景推演**：攻击者构造一个包含早期损坏 Vorbis comment block、后续合法 PICTURE 或补充字段的 FLAC。应用以为该文件没有封面或关键字段，从而绕过依赖元数据的业务规则，例如内容审核、去重或展示策略。
- **修复建议**：把单块错误降级为局部 `continue`，只跳过当前 metadata block；对 `cover cache` / `cover export` 错误继续上抛，保持缓存污染信号。

### 🔵 Low：歌词去重存在 O(n²) CPU 放大点

- **Bug 类别 & 触发位置**：算法复杂度 DoS；触发位置为 `NormalizeLyrics()`。
- **触发位置证据**：`src/text/TextNormalize.cpp:220` 把 timed lyrics 限制为 20000 行；`src/text/TextNormalize.cpp:224` 先按 timestamp 排序；`src/text/TextNormalize.cpp:226` 对相同 timestamp 分组；`src/text/TextNormalize.cpp:234` 对每个候选用 `std::any_of(groupBegin, write, ...)` 做重复文本检查。
- **漏洞描述与触发原理**：攻击者可构造大量相同 timestamp、不同文本的 LRC/SYLT 行，使同一分组内每条记录都扫描已保留记录，复杂度退化为 O(n²)。20000 行上限限制了绝对规模，但在批量解析或服务端场景仍可造成可观 CPU 放大。
- **极端破坏场景推演**：上传多个带 20000 条同时间戳歌词的音频文件，服务端批量读取 metadata 时 CPU 时间被歌词规范化消耗，降低吞吐。
- **修复建议**：排序键改为 `(timestamp, text)` 后线性 `unique()`；或每个 timestamp 分组内使用 `std::unordered_set<std::string_view>` / `std::unordered_set<std::string>` 去重。

## 三、降级或驳回的候选

- **`NormalizeMetadata()` 越界读候选已驳回**：初看 `src/text/TextNormalize.cpp:175` 访问 `normalized[cut]` 可疑，但分支条件是 `normalized.size() > kMaxFinalTextFieldBytes`，`cut` 为第一个将被丢弃的字节索引，仍在 string 范围内。该逻辑用于判断截断边界是否落在 UTF-8 continuation byte 前，随后 `IsValidUtf8()` 兜底。该项不是 CWE-125。
- **ID3 / MP4 / Ogg / APE 解析循环未确认 OOB 或整数回绕**：ID3 tag 16 MiB 上限、MP4 atom 计数与 payload 上限、Ogg page/packet/stream/扫描字节上限、APE tag/item/value 上限均存在；本次未发现可构造的越界读写或死循环路径。
- **封面缓存叶子文件污染已降级**：`src/cover/CoverCache.cpp:216` 对已有缓存文件执行 `symlink_status()`、`O_NOFOLLOW`、`fstat(S_ISREG)`、大小和逐字节比对；`./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_smoke_covers /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3` 已通过污染拒绝验证。剩余风险在父目录/默认 root 信任，而不是叶子文件覆盖。
- **FFmpeg RAII 未见泄漏/UAF**：`src/media/FfmpegSession.cpp:16` 的 deleter 使用 `avformat_close_input()`；`avformat_find_stream_info()` 失败路径显式关闭；`src/cover/CoverDecoder.cpp` 用 `unique_ptr` deleter 管理 `AVFrame`、`AVPacket`、`AVCodecContext`、`SwsContext`。
- **测试中的 `std::system()` 不计入运行时漏洞**：回归测试会用 shell 调 `ffmpeg` 生成样本，但输入路径来自测试内部临时目录，不属于库的产品运行时攻击面。

## 四、已执行的验证

- `python3 test/security/generate_samples.py`：生成安全 smoke 样本，输出 `/tmp/opencode/tagreader_security_samples`。
- `./build/TagReaderRegressionTests TR-AUDIT-029`：通过，确认直接 symlink 输入会被拒绝。
- `./build/TagReaderRegressionTests TR-AUDIT-031`：通过，确认默认封面导出、显式目录、非可写目录处理，以及显式 symlink 目录当前被接受。
- `TMPDIR=/tmp/opencode/tagreader_audit_tmp ./build/TagReaderTest /tmp/opencode/tagreader_security_samples/cover_export_base.mp3`：在隔离环境复现默认缓存 root symlink 劫持，实际 PNG 写入 `/tmp/opencode/tagreader_audit_hijack/...`。
- `./build/TagReaderSecuritySmoke /tmp/opencode/tagreader_security_smoke_covers /tmp/opencode/tagreader_security_samples/cover_cache_base.mp3`：通过，确认已有缓存污染被拒绝，重复/并发缓存路径未被重写。

## 五、架构文档一致性

`docs/DESIGN.md` 当前描述的入口、管线、格式支持、封面缓存和显式目录 symlink 行为与代码一致；本次审计没有修改该文档。需要注意的是，文档把显式 symlink 目录定义为可接受行为，这正是上文“显式目录边界依赖外部约定”的原因。

## 六、残余风险

- 未做大规模 sanitizer/fuzzer 长时间运行；本次动态验证只覆盖与候选问题直接相关的现有回归和 smoke。
- 未审计上层调用方如何选择 `coverExportDir`；如果上层已经保证目录私有且不可被低权限用户替换，显式目录 symlink 风险会降低。
- FFmpeg 内部解码器安全性视系统 FFmpeg 版本而定；本项目已设置输入/像素/输出上限，但仍应在依赖升级时继续跑 fuzz corpus。
