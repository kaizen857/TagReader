# TagReader 深度安全审计报告

> 审计日期：2026-06-03
> 审计范围：全库（src/、include/、test/）
> 审计方法：静态代码审查 + 架构分析 + 调用链追踪

---

## 一、当前架构简述

### 1.1 真实调用链

```
TagReader::Read(path [, coverExportDir])
  └─ tagreader_core::ReadTag()
       ├─ ValidatePath()          // 路径形态检查（非权限授权）
       ├─ OpenContext()           // FFmpeg AVFormatContext + 独立 ifstream
       ├─ ValidateCoverExportDir()
       ├─ DetectStream()          // 定位主音频流
       ├─ DetectTagFormat()       // 原始字节嗅探格式（ID3/FLAC/Ogg/MP4）
       ├─ ReadMediaInfo()         // FFmpeg 读取采样率/比特率/声道等
       ├─ ContainerFromTagFormat()// TagFormat → 容器名映射
       ├─ DetectContainer()       // 记录容器名
       ├─ ReadMetadata()          // 核心元数据解析（含异常吞噬）
       │    ├─ ID3v2 → ID3v1 / FLAC / Ogg Vorbis / MP4 ilst
       │    └─ 封面解码 + 缓存写入（仅 coverExportDir 非空时）
       ├─ ReadLyrics()            // 歌词解析（含异常吞噬）
       └─ BuildMusicTag()         // 中间态 → 公开 MusicTag（UTF-8 校验）
```

### 1.2 关键架构特征

| 特征 | 描述 |
|------|------|
| **IO 模型** | 所有 parser 复用同一个 `std::ifstream`（`ReadContext::input`），通过 `ReadRange()` 统一 seek/read，每次调用前后执行 `clear()` |
| **异常策略** | "顶层媒体不可用则抛错，局部 tag malformed 则跳过"。`ReadMetadata()` 吞掉非封面相关的 `runtime_error`；`ReadLyrics()` 吞掉所有异常返回空歌词 |
| **编码零信任** | ID3 encoding byte 不盲信：UTF-16 路径检查 BOM，UTF-8 路径追加 `IsValidUtf8` 校验；检测管道含 6 层 fallback sniffing |
| **资源上限** | ID3 tag 16MiB、MP4 atom 100000 个、Vorbis comment 4096 条、Ogg 扫描 64MiB/100000 页、封面输入/输出各 64MiB、像素 32M、LRC 20000 行 |
| **FFmpeg 分工** | FFmpeg 仅用于 probe/容器/媒体信息/封面解码；标签字段全部从原始字节解析，不使用 `AVDictionary` |

### 1.3 与 docs/DESIGN.md 的差异（已同步修正）

- 主流程补充了 `DetectTagFormat()` 和 `ValidateCoverExportDir()` 步骤
- 构建目标新增 `TagReaderRegressionTests`（对应 TR-AUDIT 回归验证）

---

## 二、漏洞与缺陷详情

### 🟠 HIGH-001：ReadMetadata() 异常吞噬后的 IO 流状态污染

| 属性 | 值 |
|------|-----|
| **Bug 类别** | IO 状态污染 / 级联解析失败 |
| **触发位置** | `src/core/TagPipeline.cpp` → `ReadMetadata()` 第 104-119 行 + `ReadLyrics()` 第 162 行 |
| **严重程度** | 🟠 High |

**漏洞描述**：

`ReadMetadata()` 使用 lambda `ignoreMalformedMetadata` 包裹所有 parser 调用。该 lambda 捕获 `std::runtime_error` 后，仅当错误信息含 "cover export" 或 "cover cache" 时才重新抛出；其余全部静默吞噬。

问题在于：parser 内部通过 `ReadRange()` 操作 `context.input`（共享的 `std::ifstream`）。若 parser 在 `read()` 后、`clear()` 前抛出异常（例如 Vorbis comment 解析遇到 malformed entry 后 throw），`context.input` 可能处于 `failbit` 状态。异常被吞噬后，`ReadLyrics()`（第 280 行）复用同一个 `context.input` 并调用自己的 parser，但第 162 行仅检查 `is_open()`，**未调用 `clear()` 恢复流状态**。结果：后续所有 `ReadRange()` 调用直接在 fail 流上执行，返回空数据。

**攻击/破坏场景**：

1. 构造含一个畸形 ID3v2 frame 的 MP3 文件，该 frame 导致 parser 在 `read()` 后 throw
2. 文件同时包含合法的 `USLT` 歌词帧（位于畸形 frame 之后）
3. `ReadMetadata()` 吞噬异常 → stream 处于 failbit
4. `ReadLyrics()` 操作已损坏的 stream → 返回空歌词
5. 用户看到字段缺失但无错误提示

**修复建议**：

```cpp
// 方案 A：在 ignoreMalformedMetadata 的 catch 块中恢复流状态
auto ignoreMalformedMetadata = [&context](auto &&readMetadata) {
    try {
        readMetadata();
    } catch (const std::filesystem::filesystem_error &) {
        context.input.clear();  // 新增：恢复流状态
    } catch (const std::runtime_error &ex) {
        if (IsCoverExportOrCacheError(ex.what())) {
            throw;
        }
        context.input.clear();  // 新增：恢复流状态
    }
};

// 方案 B：在 ReadLyrics() 入口处无条件恢复
// 在 TagPipeline.cpp:ReadLyrics() 的 parser 调用前插入：
context.input.clear();
```

---

### 🟠 HIGH-002：ReadFlacLyrics() 缺少 TryAddUintmax 溢出保护

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 整数溢出 / 偏移量回绕 |
| **触发位置** | `src/formats/flac/FlacParser.cpp` 第 293 行 |
| **严重程度** | 🟠 High |

**漏洞描述**：

`ReadFlacLyrics()` 在第 293 行使用 `blockSize > context.fileSize - cursor` 进行范围检查。此模式与 `ReadFlacMetadataBlocks()` 使用的 `TryAddUintmax(cursor, blockSize, blockEnd)` 不一致。当 `cursor > context.fileSize` 时（例如上游 parser 因 malformed 数据将 cursor 推进到非法位置），`fileSize - cursor` 会因无符号回绕产生极大值，导致检查失效。

**攻击/破坏场景**：

1. 构造一个 FLAC 文件，其 Vorbis Comment block 声明了畸形的 block length
2. 上游 parser 将 cursor 推进到超过 `fileSize` 的位置
3. `ReadFlacLyrics()` 中 `fileSize - cursor` 回绕为接近 `UINTMAX_MAX` 的值
4. `blockSize` 几乎总是小于此值，检查通过
5. 后续 `ReadRange()` 调用传入无效 offset，虽不会导致越界（ReadRange 自身有保护），但产生无意义的 IO 操作

**修复建议**：

```cpp
// 将 ReadFlacLyrics() 第 293 行的检查改为与 ReadFlacMetadataBlocks() 一致：
std::uintmax_t blockEnd{};
if (!tagreader_io::TryAddUintmax(cursor, blockSize, blockEnd) || blockEnd > context.fileSize) {
    break;  // 或 return，视上下文而定
}
```

---

### 🟡 MEDIUM-001：无 iconv 构建时编码嗅探直接回退 latin-1

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 编码安全性 / "零信任"策略缺陷 |
| **触发位置** | `src/text/TextCodec.cpp` 第 125-151 行 (`DetectLegacyLocalEncoding`) |
| **严重程度** | 🟡 Medium |

**漏洞描述**：

当 `TAGREADER_HAS_ICONV` 未定义时（即编译环境无 iconv 支持），`DetectLegacyLocalEncoding()` 直接返回 `"latin-1"`，完全跳过了 GB18030/GBK/SHIFT_JIS/BIG5 等候选编码的探测。这意味着在无 iconv 构建中，所有非 BOM、非法 UTF-8、非明显 UTF-16 的文本都会被当作 Latin-1 解码，可能产生大量乱码文本进入最终 `MusicTag`。

这违背了"零信任编码"的设计初衷——在无 iconv 支持时，库无法区分"真的是 Latin-1"和"是 GBK 但没检测出来"。

**修复建议**：

- 方案 A：在无 iconv 构建时，对 `DetectLegacyLocalEncoding` 返回 `"latin-1"` 的结果标记 `encodingConfidence = low`，由上层决定是否接受
- 方案 B：在无 iconv 构建时返回空编码（`std::nullopt`），让调用方决定是否使用原始字节的 heuristic 或回退策略
- 方案 C：文档化无 iconv 构建的限制，建议始终启用 iconv

---

### 🟡 MEDIUM-002：FFmpeg avformat_open_input 无符号链接防护

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 文件系统安全 / TOCTOU |
| **触发位置** | `src/media/FfmpegSession.cpp` 第 68 行 |
| **严重程度** | 🟡 Medium |

**漏洞描述**：

`OpenContext()` 调用 `avformat_open_input(&formatContext, path.c_str(), ...)` 时，传入的是裸路径字符串。FFmpeg 内部不提供 `O_NOFOLLOW` 机制。如果调用方传入的路径指向一个符号链接，FFmpeg 会跟随该链接打开目标文件。这与封面缓存路径中使用的 `O_NOFOLLOW` + `symlink_status()` 强防护形成不对称。

**攻击/破坏场景**：

1. 攻击者在一个调用方可信的目录中创建符号链接，指向恶意文件
2. 调用方传入符号链接路径调用 `TagReader::Read()`
3. FFmpeg 跟随符号链接读取恶意文件
4. 虽然后续 parser 有边界检查，但 FFmpeg probe 阶段可能触发其内部的解析漏洞

**修复建议**：

```cpp
// 在 avformat_open_input 调用前添加符号链接检查：
std::error_code ec;
if (std::filesystem::is_symlink(path, ec) || ec) {
    throw std::runtime_error("TagReader: symbolic link not allowed: " + path.string());
}
// 或：先 realpath 解析，再传入解析后的路径
```

---

### 🟡 MEDIUM-003：NormalizeMetadata 对最终 UTF-8 字段无长度限制

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 资源耗尽 / DoS |
| **触发位置** | `src/text/TextNormalize.cpp` 第 162-183 行 (`NormalizeMetadata`) |
| **严重程度** | 🟡 Medium |

**漏洞描述**：

`NormalizeMetadata()` 对 `title`、`artist`、`album` 等字段做 `TrimText` + `IsValidUtf8` 校验，通过后直接写入 `MusicTag`。虽然原始输入阶段有 `kMaxTextFieldBytes`（通常 1 MiB）限制，但 `TrimText` 后的最终字符串大小没有独立的二次上限。在某些路径（如 UTF-8 直通路径，`DecodeTextToUtf8` 第 642-652 行），未调用 `appendChecked`（该函数有 `kMaxDecodedTextBytes` 限制），因此理论上可构造接近 1 MiB 的合法 UTF-8 文本进入最终 `MusicTag` 字段，对下游消费者造成意外负担。

**修复建议**：

```cpp
// 在 NormalizeMetadata() 中对每个字段增加长度裁剪：
constexpr std::size_t kMaxFinalTextFieldBytes = 65536; // 64 KiB

auto normalizeField = [&](std::string &field) {
    TrimText(field);
    if (field.size() > kMaxFinalTextFieldBytes) {
        field.resize(kMaxFinalTextFieldBytes); // 或清空
    }
    if (!IsValidUtf8(field)) {
        field.clear();
    }
};
```

---

### 🟡 MEDIUM-004：ID3 Frame Walker 遇畸形帧后全量丢弃后续合法帧

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 解析恢复缺陷 / 数据完整性 |
| **触发位置** | `src/formats/id3/Id3Frames.cpp` `ReadID3v22Frames()` / `ReadID3v23Or24Frames()` |
| **严重程度** | 🟡 Medium |

**漏洞描述**：

ID3v2 frame walker 在遇到第一个异常 frame（非法 frame ID、`frameSize == 0`、frame size 超过剩余 tag、非法 syncsafe frame size）时，使用 `break` 终止整个 tag 扫描，而非跳过该畸形区域后继续尝试解析后续 frame。这意味着攻击者只需在合法帧之前插入一个畸形帧，即可隐藏所有后续元数据。

**实际影响**：不是越界读写，但属于可控的元数据截断。`TryResyncId3v23Or24Frame()` 提供了部分恢复能力（逐字节扫描寻找下一个有效 frame header），但其触发条件有限——当前只在 syncsafe 验证失败时调用。

**修复建议**：

- 扩大 resync 触发范围：在遇到非法 frame ID、大小为 0 等场景也尝试 resync，而非直接 break
- 区分明确的 padding（全 0x00 区域应停止扫描）与畸形数据（应跳过并尝试恢复）
- 考虑增加 resync 的最大扫描距离限制（如 4096 字节）防止退化扫描

---

### 🟡 MEDIUM-005：ID3v2 后 ID3v1 兜底调用复用了可能已被污染的 stream

| 属性 | 值 |
|------|-----|
| **Bug 类别** | IO 状态污染 / 级联影响 |
| **触发位置** | `src/core/TagPipeline.cpp` `ReadMetadata()` 第 120-148 行 |
| **严重程度** | 🟡 Medium |

**漏洞描述**：

`ReadMetadata()` 中 ID3v2 路径先调用 ID3v2 parser，再调用 ID3v1 parser（兜底）。两个 parser 通过同一个 `ignoreMalformedMetadata` lambda 包裹。如果 ID3v2 parser 在 `ReadRange()` 操作后抛异常被吞噬，stream 可能处于 fail 状态（见 HIGH-001）。此时 ID3v1 parser 紧接着在同一 stream 上调用，将因流状态损坏而直接失败，即使文件确实包含合法的 ID3v1 tag。

**修复建议**：

在两次 parser 调用之间插入 `context.input.clear()`：

```cpp
// ReadMetadata() 中 ID3v2 分支：
ignoreMalformedMetadata([&] { 
    tagreader_id3::ReadID3v2Metadata(context, metadata); 
});
context.input.clear();  // 新增：在 ID3v1 调用前恢复流
ignoreMalformedMetadata([&] { 
    tagreader_id3::ReadID3v1Metadata(context, metadata); 
});
```

---

### 🔵 LOW-001：CoverDecoder 中 packet->data 的 const_cast 使用

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 代码健壮性 / 维护风险 |
| **触发位置** | `src/cover/CoverDecoder.cpp` 第 207 行 |
| **严重程度** | 🔵 Low |

**漏洞描述**：

`DecodeAndEncodeCoverPng()` 中通过 `packet->data = const_cast<uint8_t*>(data)` 将外部 buffer 指针赋给 `AVPacket`，且未设置 `packet->buf`。当前代码中 `av_packet_free` → `av_packet_unref` 不会尝试释放无 `buf` 的 packet，因此安全。但如果未来代码重构将 `buf` 设置为引用外部 buffer，或在 packet 使用后尝试 `av_packet_unref` 释放内存，则会导致 double-free 或释放非堆内存。

**修复建议**：

添加注释说明所有权语义：

```cpp
// data 为外部只读 buffer，packet 不持有所有权。av_packet_free 不会尝试释放。
packet->data = const_cast<uint8_t *>(data);
packet->size = static_cast<int>(size);
// packet->buf 保持 nullptr —— av_packet_unref 对此安全
```

---

### 🔵 LOW-002：Latin-1 解码遇 0x00 字节直接截断

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 数据完整性 |
| **触发位置** | `src/text/TextCodec.cpp` `ReadLatin1Text()` 第 463-466 行 |
| **严重程度** | 🔵 Low |

**漏洞描述**：

`ReadLatin1Text()` 在遇到字节 0x00 时立即截断返回。这是 C 风格字符串的处理方式。如果 Latin-1 编码的文本字段中合法包含 0x00 字节（虽然罕见，但在某些 binary-embedded 文本中可能出现），后续内容会被丢弃。

**修复建议**：

评估实际音频文件中 Latin-1 字段含 0x00 的概率。如果概率极低，当前行为可接受。否则，考虑将 0x00 替换为空格或 `U+FFFD` 而非截断。

---

### 🔵 LOW-003：UTF-16 无 BOM 嗅探启发式可能误判

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 编码检测精度 |
| **触发位置** | `src/text/TextCodec.cpp` `LooksLikeUtf16WithoutBom()` 第 56-123 行 |
| **严重程度** | 🔵 Low |

**漏洞描述**：

`LooksLikeUtf16WithoutBom()` 使用统计启发式：高/低字节空字节比例、ASCII 范围比例、可疑控制字符比例。阈值设置为 `nulOnHighByte * 3 >= units * 2`（至少 2/3 预期空字节）。对于恰好符合 ASCII 分布、低控制字符比例的畸形数据，可能被误判为 UTF-16，导致乱码解码。

**修复建议**：

考虑增加额外的信号检查，如：连续两个字节是否均为合法 Unicode 标量值（在 BOM-less 情况下难以验证），或将置信度信息传递给调用方。

---

### 🔵 LOW-004：LRC 歌词分钟数字段无合理上限

| 属性 | 值 |
|------|-----|
| **Bug 类别** | 输入验证宽松 |
| **触发位置** | `src/text/TextNormalize.cpp` `ParseLrcTimestamp()` 第 118-121 行 |
| **严重程度** | 🔵 Low |

**漏洞描述**：

`ParseDecimalU16Strict()` 允许分钟数字段达到 65535（受 `uint16_t` 约束）。虽然不会造成整数溢出，但 `[65535:59.999]` 约等于 45 天的时间戳在 LRC 歌词上下文中无实际意义，可能被用于构造畸形歌词数据。

**修复建议**：

增加分钟数上限（例如 999）：

```cpp
constexpr uint16_t kMaxLrcMinutes = 999;
// 在 ParseLrcTimestamp() 中添加：
if (minutes > kMaxLrcMinutes) return std::nullopt;
```

---

### 📐 架构缺陷

#### AD-001：ReadContext 共享可变状态缺乏阶段间恢复机制

所有 parser 复用同一个 `std::ifstream`（`ReadContext::input`），但 Pipeline 各阶段之间没有统一的流状态恢复点。`ReadRange()` 内部在每次调用前后执行 `clear()`/`seekg()`，这保证了单次调用的安全性，但无法防御异常吞噬后的跨阶段污染（见 HIGH-001）。

**建议**：在 Pipeline 的每个阶段入口处（`ReadMetadata()`、`ReadLyrics()` 开头）显式调用 `context.input.clear()`。

#### AD-002：Parser 错误被大量吞噬，缺乏诊断能力

`ReadMetadata()` 和 `ReadLyrics()` 的异常吞噬策略使得在 fuzzing 和调试中难以区分"格式合法但字段缺失"与"parser 遇到 malformed 后放弃"。这降低了安全测试和故障定位的效率。

**建议**：内部增加可选 diagnostics channel（如 `std::ostream* debugLog`），不改变 public API，但在 fuzz/minimization 时可输出 parser 失败原因。

#### AD-003：各 parser 中整数解析 helper 重复实现

ID3（`Id3Frames.cpp`）、Vorbis（`VorbisCommentParser.cpp`）、MP4（`Mp4Parser.cpp`）各自实现了 `ParseUInt16()`、`ParseSlashNumber()`、`ParseYearOnly()` 等相似逻辑。重复实现增加了规范分歧和维护风险。

**建议**：将这些 helper 抽取到 `src/text/` 或新建 `src/common/`，共享使用。

---

## 三、"零信任编码"策略审查总结

| 审查项 | 状态 | 评价 |
|--------|------|------|
| ID3 encoding byte 不盲信 | ✅ 通过 | UTF-8 声明后追加 `IsValidUtf8` 校验；UTF-16 声明要求 BOM 存在 |
| 无 BOM UTF-16 的 fallback sniffing | ⚠️ 部分 | 有启发式检测但可能误判（LOW-003） |
| UTF-8 合法性校验严格性 | ✅ 通过 | 过长序列、代理对、超出 0x10FFFF 均被拒绝 |
| iconv 编码候选探测 | ⚠️ 部分 | 有 iconv 时探测 8 种编码；无 iconv 时直接回退 latin-1（MEDIUM-001） |
| 转换后二次校验 | ✅ 通过 | `IsValidUtf8` + `IsMostlyPrintableText` 双重检查 |
| MP4/Vorbis 分支编码处理 | ✅ 通过 | 均通过 `DecodeTextToUtf8`/`DecodeRawText` 统一入口 |

总体评价：**"零信任"策略在核心路径上得到了良好落实**。主要薄弱点在于无 iconv 构建时的回退策略（MEDIUM-001）和 UTF-16 启发式检测的精度边界（LOW-003）。

---

## 四、整体安全等级评估

| 维度 | 等级 | 说明 |
|------|------|------|
| 内存安全 | 🟢 良好 | 未发现可确认的越界读写、UAF、double-free。`ReadRange()` 和 `ByteCursor` 提供了多层边界保护 |
| 整数安全 | 🟢 良好 | `TryAddUintmax`/`TryAddSize` 覆盖主要溢出路径；HIGH-002 为风格不一致而非实际可利用 |
| 拒绝服务 | 🟡 中等 | 各 parser 有独立资源上限，但缺乏全局 per-file CPU/IO budget；MEDIUM-003 为字段长度宽松 |
| 并发安全 | 🟢 良好 | 每次 `ReadTag()` 调用有独立 `ReadContext`；封面缓存使用 `O_EXCL + link()` 原子发布 |
| 数据完整性 | 🟡 中等 | ID3 frame resync 覆盖不足（MEDIUM-004）；IO 状态污染可导致级联字段缺失（HIGH-001） |
| 编码安全性 | 🟡 中等 | 核心路径"零信任"落实到位；无 iconv 构建存在盲区（MEDIUM-001） |

**综合安全等级**：🟡 中等偏上。代码具备良好的安全基础（统一 IO helper、多层边界检查、RAII 资源管理），核心风险集中在异常吞噬后的 IO 状态污染和个别 parser 的风格不一致上。建议优先修复 HIGH-001 和 HIGH-002，再逐步处理 MEDIUM 级问题。

---

## 五、建议的修复优先级

| 优先级 | 编号 | 修复项 |
|--------|------|--------|
| P0 | HIGH-001 | IO 流状态污染（在 `ignoreMalformedMetadata` catch 块中恢复 stream） |
| P0 | HIGH-002 | `ReadFlacLyrics()` 改用 `TryAddUintmax` |
| P1 | MEDIUM-001 | 无 iconv 构建的编码回退策略 |
| P1 | MEDIUM-002 | `avformat_open_input` 前增加符号链接检查 |
| P1 | MEDIUM-004 | ID3 frame walker 扩大 resync 触发范围 |
| P2 | MEDIUM-003 | NormalizeMetadata 增加最终字段长度限制 |
| P2 | MEDIUM-005 | ID3v2→ID3v1 调用间恢复 stream 状态 |
| P3 | AD-001~003 | 架构改进（阶段间 clear、diagnostics channel、helper 去重） |
