# ape-tag-support：为 TagReader 新增 APEv2 标签解析

## TL;DR

> **Quick Summary**: 在 C++23 TagReader 音频标签库中新增 APEv2 解析能力，覆盖 Monkey's Audio、Musepack、WavPack 和 MP3+APE 容器，优先级 APE > ID3v2 > ID3v1，含封面和歌词。
> 
> **Deliverables**:
> - 2 个新文件（`ApeParser.{hpp,cpp}`） + 1 个 limits 头文件
> - 6 个已有文件修改（枚举、检测、管线、媒体信息、CMake）
> - 2-3 个 TR-AUDIT 回归测试用例
> 
> **Estimated Effort**: Medium
> **Parallel Execution**: YES — 3 waves
> **Critical Path**: Wave 1 → Wave 2 → Wave 3

---

## Context

### 原始需求

为 TagReader 添加 APE tag（APEv2）解析支持。APE tag 是 Monkey's Audio 的原生标签格式，也被 Musepack、WavPack 等容器采用，可附加在 MP3 文件尾部 ID3v1 之前。

### 关键决策

- **文件范围**：所有含 APE tag 的音频容器（.ape, .mpc, .wv, MP3+APE），仅音频不处理视频
- **优先级**：APE > ID3v2 > ID3v1（APE 为主，ID3v2 补缺，ID3v1 兜底）
- **封面 + 歌词**：全部支持（COVER ART FRONT → FFmpeg 解码；LYRICS → UTF-8 文本）
- **读写范围**：仅解析读取，不实现标签写入
- **测试策略**：Tests-after — 先实现后补 2-3 个 TR-AUDIT 回归用例

### 格式规范摘要

| 结构 | 大小 | 描述 |
|------|------|------|
| Header/Footer | 32 B | Preamble "APETAGEX" + version(LE32) + tagSize(LE32) + itemCount(LE32) + flags(LE32) + reserved |
| Item | ≥10 B | valueSize(LE32) + flags(LE32) + key(ASCII\0) + value(variable) |

- **端序**：全部小端（LE）
- **文本编码**：item flags bit2-1 = 00 → UTF-8；01 → Binary（封面）
- **检测位置**：文件末尾最后 32 字节（footer）→ 通过 tagSize 反向定位 header
- **推荐布局**（文件尾）：`[Header 32B] [Items...] [Footer 32B]`

### 标准字段映射

| APE Key | MusicTag 字段 |
|---------|-------------|
| Title | title |
| Artist | artist |
| Album | album |
| Album Artist | albumArtist |
| Composer | composer |
| Genre | genre |
| Year | year |
| Track | trackNumber |
| Disc | discNumber |
| COVER ART (FRONT) | coverPath |
| LYRICS | lyrics |

---

## Work Objectives

### Core Objective

实现 APEv2 标签的二进制解析，覆盖全部标准元数据字段、封面、歌词，并集成到现有格式检测/管线调度框架。

### Concrete Deliverables

- `src/formats/ape/ApeLimits.hpp` — 解析器资源上限常量
- `src/formats/ape/ApeParser.hpp` — 公共 API 声明
- `src/formats/ape/ApeParser.cpp` — 解析实现
- `src/core/TagFormat.hpp` — 新增 `Ape` 枚举值
- `src/core/ReadContext.hpp` — 新增 `DetectedContainer::Ape`
- `src/media/ContainerDetector.cpp` — 新增 `HasApeFooter()` + 检测逻辑
- `src/media/MediaInfoReader.cpp` — 新增 `Ape` 容器格式名处理
- `src/core/TagPipeline.cpp` — 新增 Ape 分支（Metadata + Lyrics）
- `CMakeLists.txt` — 新增源文件
- `test/regression/regression_tests.cpp` — 2-3 个 APE 回归用例

### Must Have

- APE footer 优先检测（ID3 header 前检查 APE footer → 确保 MP3+APE 优先级正确）
- ReadMetadata Ape case：APE 主路径 + MP3 容器时 ID3v2→ID3v1 回退补缺
- 所有 LE32 整数通过 `ReadLE32()` 解析
- item 遍历预算 ≤ 4096 项，tag 总大小 ≤ 16 MiB
- 封面 COVER ART (FRONT) 复用现有 `WriteCoverAsPng` 管线

### Must NOT Have

- 不实现 APE tag 写入
- 不解析 APEv1（version=1000）：检测到后记录警告并跳过
- 不处理非封面二进制 item 和外部引用 item（静默跳过）
- 不修改已有格式的解析逻辑
- 不引入新依赖

---

## Verification Strategy

### Test Decision

- **Infrastructure exists**: YES（TagReaderRegressionTests）
- **Automated tests**: Tests-after
- **Framework**: 手动编译的 TR-AUDIT 风格回归测试 + python3 生成畸形测试文件

### QA Policy

- **API/Backend**：TagReaderTest 程序 + 动态生成的 APE/MP3 测试文件
- **测试文件生成**：python3 + struct.pack（LE32 整数 + APETAGEX 签名）
- **证据保存**：`.omo/evidence/task-{N}-*.log`

---

## Execution Strategy

```
Wave 1 (Start Immediately — 枚举 + CMake):
├── Task 1: TagFormat.hpp + ReadContext.hpp 枚举变更 [quick]
└── Task 2: CMakeLists.txt + ApeLimits.hpp 基础文件 [quick]

Wave 2 (After Wave 1 — 核心实现，MAX PARALLEL):
├── Task 3: ApeParser.hpp + ApeParser.cpp 解析器 [deep]
├── Task 4: ContainerDetector.cpp 检测逻辑 [deep]
├── Task 5: TagPipeline.cpp 管线集成 [quick]
└── Task 6: MediaInfoReader.cpp 格式名 [quick]

Wave 3 (After Wave 2 — 测试):
└── Task 7: TR-AUDIT 回归测试用例 [deep]

Critical Path: Task 1 → Task 3 → Task 7
Parallel Speedup: ~55% faster than sequential
Max Concurrent: 4 (Wave 2)
```

---

## TODOs

- [x] 1. TagFormat.hpp + ReadContext.hpp — 枚举值新增

  **What to do**:
  1. `src/core/TagFormat.hpp`：在 `TagFormat` 枚举末尾（`Mp4` 之后）新增 `Ape`
  2. `src/core/ReadContext.hpp`：在 `DetectedContainer` 枚举末尾新增 `Ape`

  **Must NOT do**:
  - 不修改已有枚举值的顺序
  - 不新增其他 include

  **Recommended Agent Profile**: `quick` — 两行枚举值添加，零逻辑

  **Parallelization**:
  - **Wave**: 1，与 Task 2 并行
  - **Blocks**: Task 3,4,5,6
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `TagFormat::Ape` 存在于 `src/core/TagFormat.hpp`
  - [ ] `DetectedContainer::Ape` 存在于 `src/core/ReadContext.hpp`
  - [ ] 编译通过

  **QA Scenarios**:
  ```
  Scenario: 编译验证枚举完整
    Tool: Bash (cmake)
    Steps: cmake --build build 2>&1
    Expected: 编译成功，所有目标链接
    Evidence: .omo/evidence/task-1-build.log
  ```

  **Commit**: YES
  - Message: `feat: add Ape to TagFormat and DetectedContainer enums`
  - Files: `src/core/TagFormat.hpp`, `src/core/ReadContext.hpp`

- [x] 2. CMakeLists.txt + ApeLimits.hpp — 基础文件创建

  **What to do**:
  1. 创建 `src/formats/ape/` 目录
  2. 创建 `src/formats/ape/ApeLimits.hpp`，定义：
     ```cpp
     namespace tagreader_ape {
     constexpr std::size_t kMaxApeTagBytes = 16z * 1024 * 1024;  // 16 MiB
     constexpr std::size_t kMaxApeItems = 4096;                   // 最大 item 数
     constexpr std::size_t kMaxApeItemValueBytes = 1z * 1024 * 1024; // 单 item 1 MiB
     }
     ```
  3. 编辑 `CMakeLists.txt`：在 `add_library(TagReaderCore STATIC` 块中 `src/formats/mp4/Mp4Parser.cpp` 之后，新增：
     ```
     src/formats/ape/ApeLimits.hpp
     src/formats/ape/ApeParser.cpp
     ```
  4. 创建空的 `src/formats/ape/ApeParser.cpp`（仅含 `#include "formats/ape/ApeParser.hpp"` 占位），确保 CMake 可编译

  **Must NOT do**:
  - 不修改已有源文件的顺序
  - 不修改 CMake 的编译选项

  **Recommended Agent Profile**: `quick` — 目录 + 头文件 + CMake 行追加

  **Parallelization**:
  - **Wave**: 1，与 Task 1 并行
  - **Blocks**: Task 3,4,5,6
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `src/formats/ape/ApeLimits.hpp` 存在且含 3 个 constexpr 常量
  - [ ] `CMakeLists.txt` 包含 `src/formats/ape/ApeParser.cpp`
  - [ ] `cmake --build build` 编译成功（含空 ApeParser.cpp 占位）

  **QA Scenarios**:
  ```
  Scenario: 编译验证 CMake 正确引用新文件
    Tool: Bash (cmake)
    Steps:
      1. mkdir -p src/formats/ape
      2. cmake -S . -B build && cmake --build build 2>&1
    Expected: 编译成功，ApeParser.cpp 出现在编译输出中
    Evidence: .omo/evidence/task-2-build.log
  ```

  **Commit**: YES
  - Message: `feat: add ApeLimits and CMake integration for APE parser`
  - Files: `src/formats/ape/ApeLimits.hpp`, `src/formats/ape/ApeParser.cpp`, `CMakeLists.txt`

- [x] 3. ApeParser.hpp + ApeParser.cpp — 核心解析器实现

  **What to do**:
  1. 创建 `src/formats/ape/ApeParser.hpp`：
     ```cpp
     #ifndef TAGREADER_FORMATS_APE_APEPARSER_HPP
     #define TAGREADER_FORMATS_APE_APEPARSER_HPP
     #include "core/RawTagData.hpp"
     #include "core/ReadContext.hpp"
     namespace tagreader_ape {
     void ReadApeMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
     void ReadApeLyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
     }
     #endif
     ```

  2. 在 `ApeParser.cpp` 中实现完整解析逻辑：

  **ReadApeMetadata 流程**：
  a. 读取文件末尾 32 字节 → 验证 `"APETAGEX"` 前导码
  b. `ReadLE32()` 读取 version/tagSize/itemCount/flags
  c. 若 version < 2000 → 记录警告，返回（APEv1 不支持）
  d. 若 `tagSize > kMaxApeTagBytes` 或 `itemCount > kMaxApeItems` → 拒绝（`throw std::runtime_error`）
  e. 检查 flags.HasHeader（bit31）：若是，header 在 `fileSize - tagSize - 32` 处，items 从 `fileSize - tagSize` 开始；若否，items 从 `fileSize - tagSize` 开始
  f. `ReadRange()` 读取整个 item 区域
  g. 迭代 itemCount 次：
     - `ReadLE32` 读取 valueSize / flags
     - 读取 key（ASCII 字节直到 0x00 终止符）
     - 读取 value（valueSize 字节）
     - 根据 encoding flag（bit2-1）：00=UTF-8 text → `ReadUtf8Text()`；01=Binary → 仅处理 COVER ART；10=ExtRef → 跳过
     - **字段映射**（大小写不敏感，first-write-wins）：
       - `"title"` → metadata.title
       - `"artist"` → metadata.artist
       - `"album"` → metadata.album
       - `"album artist"` → metadata.albumArtist
       - `"composer"` → metadata.composer
       - `"genre"` → metadata.genre
       - `"year"` → ParseYear → metadata.year
       - `"track"` → ParseSlashNumber().first → metadata.trackNumber
       - `"disc"` → ParseSlashNumber().first → metadata.discNumber
     - 封面处理：`"COVER ART (FRONT)"` 或 `"COVER ART (BACK)"` + binary flag → 按规范去除可选 description 前缀 → `WriteCoverAsPng(data, size, pixFmt, width, height, context)`
  h. item 遍历窗口预算：`kMaxApeItems` 项，超出拒绝

  **ReadApeLyrics 流程**：
  - 复用上述 footer 定位和 item 遍历逻辑
  - 查找 key 为 `"LYRICS"` 或 `"UNSYNCED LYRICS"` 或 `"UNSYNCEDLYRICS"` 的 item
  - 值解码为 UTF-8 → `lyrics.text` 或通过 `ReadLyricsFromPlainText()` 解析为 timedLines

  **Must NOT do**:
  - 不实现 APEv1（version=1000）的完整解析
  - 不处理非封面二进制 item（静默 `continue`）
  - 不处理外部引用 item（静默 `continue`）
  - 不修改已有 parser 的实现

  **Recommended Agent Profile**: `deep` — 完整二进制协议解析，需处理多容器、多编码、异常安全

  **Parallelization**:
  - **Wave**: 2，与 Task 4,5,6 并行
  - **Blocks**: Task 7
  - **Blocked By**: Task 1,2

  **Acceptance Criteria**:
  - [ ] `ApeParser.hpp` 声明 `ReadApeMetadata` + `ReadApeLyrics`
  - [ ] `ApeParser.cpp` 实现完整 footer 定位（32B → "APETAGEX" 验证 → tagSize 反推）
  - [ ] item 遍历含预算限制（kMaxApeItems=4096, kMaxApeTagBytes=16MiB）
  - [ ] key 匹配大小写不敏感
  - [ ] 封面 COVER ART 正确处理 description 前缀
  - [ ] `cmake --build build` 编译通过（含 ApeParser.cpp）

  **QA Scenarios**:

  ```
  Scenario: 合法 APEv2 tag — 完整字段读取（HAPPY PATH）
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. python3 生成含 APEv2 footer + header + 标准字段 items 的 .ape 文件
         - ApeParser body: Header("APETAGEX"+LE32(2000)+LE32(tagSize)+LE32(itemCount)+flags+reserved)
         - Items: Title("Test Title")+Artist("Test Artist")+Album("Test Album")+Track("3/12")+Year("2024")
         - Footer: 同 header, bit29=0
      2. ./build/TagReaderTest /tmp/test_ape_valid.ape
    Expected: title="Test Title", artist="Test Artist", album="Test Album", trackNumber=3, year=2024
    Evidence: .omo/evidence/task-3-valid-ape.log

  Scenario: APE footer 无 header (v1 风格布局) — 仍正常解析
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成仅含 footer (flags.HasHeader=0) 的 APE tag
      2. ./build/TagReaderTest 验证
    Expected: 字段正常解析，与有 header 版本结果一致
    Evidence: .omo/evidence/task-3-no-header.log

  Scenario: 畸形 tag — tagSize 超出文件范围 → 安全拒绝
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成 tagSize=0xFFFFFFFF 的恶意 footer
      2. ./build/TagReaderTest
    Expected: 异常被 ignoreMalformedMetadata 吞掉，程序不崩溃，metadata 为空
    Evidence: .omo/evidence/task-3-malformed-size.log
  ```

  **Commit**: YES
  - Message: `feat: implement APEv2 tag parser (metadata + lyrics + cover)`
  - Files: `src/formats/ape/ApeParser.hpp`, `src/formats/ape/ApeParser.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 4. ContainerDetector.cpp — APE footer 检测 + 容器映射

  **What to do**:
  1. 在 `src/media/ContainerDetector.cpp` 匿名命名空间中新增：
     ```cpp
     bool HasApeFooter(tagreader_core::ReadContext &context, uint32_t &tagSize, uint32_t &itemCount, uint32_t &flags)
     ```
     实现：`ReadRange(context.input, context.fileSize - 32, 32)` → 验证 "APETAGEX" → `ReadLE32()` 提取 version/tagSize/itemCount/flags → 若 version<2000 返回 false

  2. 修改 `DetectTagFormat()`：
     - **在 ID3 header 检查之前**（关键！），新增 APE footer 优先检测：
       - 若 header 为 "ID3" 且 `HasApeFooter()` → 立即返回 `TagFormat::Ape`
       - 这确保 MP3+APE 文件的 APE 优先级
     - 在 container name fallback 区域，新增以下匹配（在 `Unknown` 返回之前）：
       - 若 container 含 `"ape"` / `"mpc"` / `"mpc8"` / `"wv"` / `"tak"` / `"tta"` → 返回 `TagFormat::Ape`

  3. 修改 `ContainerFromTagFormat()`：
     ```cpp
     case TagFormat::Ape:
         return DetectedContainer::Ape;
     ```

  **Must NOT do**:
  - 不修改已有格式的检测逻辑（ID3/FLAC/Ogg/MP4）
  - APE footer 检查不在 MP3+APE 之外的文件上做不必要的末尾读取——仅当 header==ID3 或 container 匹配 APE 格式时才触发

  **Recommended Agent Profile**: `deep` — 涉及检测顺序的关键架构决策，需精确放置以避免 MP3+APE 优先级错误

  **Parallelization**:
  - **Wave**: 2，与 Task 3,5,6 并行
  - **Blocks**: Task 7
  - **Blocked By**: Task 1,2

  **Acceptance Criteria**:
  - [ ] `HasApeFooter()` 存在且正确解析 32 字节 footer
  - [ ] `DetectTagFormat()` 中 APE footer 检查在 ID3 header 检查之前
  - [ ] `ContainerFromTagFormat()` 含 `TagFormat::Ape → DetectedContainer::Ape`
  - [ ] 编译通过

  **QA Scenarios**:
  ```
  Scenario: MP3+APE 文件 → 检测为 Ape 而非 Id3v2（优先级验证）
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成: [ID3v2 header at 0] + [MPEG frame] + [APE footer at tail]
         - ID3 header: "ID3" + version + size + flags
         - APE footer: "APETAGEX" + LE32(2000) + size + count + flags
      2. ./build/TagReaderTest 2>&1 | grep -i "format"
    Expected: 输出显示 format 为 "ape" 或类似（非 "mp3"），确认 APE 被优先检测
    Evidence: .omo/evidence/task-4-ape-priority.log

  Scenario: 纯 Monkey's Audio (.ape) → 检测为 Ape
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成 Monkey's Audio container header + APE tag
      2. ./build/TagReaderTest
    Expected: 正常解析 APE metadata，不因 container 未知而失败
    Evidence: .omo/evidence/task-4-pure-ape.log
  ```

  **Commit**: YES
  - Message: `feat: add APE footer detection in ContainerDetector`
  - Files: `src/media/ContainerDetector.cpp`

- [x] 5. TagPipeline.cpp — 管线集成（Metadata + Lyrics 分支）

  **What to do**:
  1. 在 `src/core/TagPipeline.cpp` 头部新增：
     ```cpp
     #include "formats/ape/ApeParser.hpp"
     ```

  2. 在 `ReadMetadata()` 的 switch 中，`case TagFormat::Id3v2:` 之前插入：
     ```cpp
     case TagFormat::Ape:
         ignoreMalformedMetadata([&]()
                                 { tagreader_ape::ReadApeMetadata(context, metadata); });
         context.input.clear();
         // MP3+APE: ID3 fallback for fields APE didn't provide
         {
             const std::string containerLower = ToLower(context.containerName);
             if (containerLower.find("mp3") != std::string::npos ||
                 containerLower.find("mpeg") != std::string::npos)
             {
                 ignoreMalformedMetadata([&]()
                                         { tagreader_id3::ReadID3v2Metadata(context, metadata); });
                 context.input.clear();
                 ignoreMalformedMetadata([&]()
                                         { tagreader_id3::ReadID3v1Metadata(context, metadata); });
             }
         }
         break;
     ```

  3. 在 `ReadLyrics()` 的 switch 中，`case TagFormat::Unknown:` 之前插入：
     ```cpp
     case TagFormat::Ape:
         tagreader_ape::ReadApeLyrics(context, lyrics);
         break;
     ```

  **Must NOT do**:
  - 不修改已有 parser 的调用顺序
  - 不修改 APE case 之外的其他分支

  **Recommended Agent Profile**: `quick` — include + 两个 switch case 插入

  **Parallelization**:
  - **Wave**: 2，与 Task 3,4,6 并行
  - **Blocks**: Task 7
  - **Blocked By**: Task 1,2

  **Acceptance Criteria**:
  - [ ] `#include "formats/ape/ApeParser.hpp"` 存在于 TagPipeline.cpp
  - [ ] `ReadMetadata()` switch 含 `case TagFormat::Ape:`（MP3 容器时含 ID3 回退）
  - [ ] `ReadLyrics()` switch 含 `case TagFormat::Ape:`
  - [ ] 编译通过

  **QA Scenarios**:
  ```
  Scenario: 端到端 — APE metadata 经管线写入 MusicTag
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. python3 生成含完整 APE metadata 的测试文件
      2. ./build/TagReaderTest /tmp/test_e2e_ape.ape
    Expected: MusicTag 含 title/artist/album/year 等字段
    Evidence: .omo/evidence/task-5-e2e.log
  ```

  **Commit**: YES
  - Message: `feat: wire APE parser into TagPipeline (metadata + lyrics)`
  - Files: `src/core/TagPipeline.cpp`

- [x] 6. MediaInfoReader.cpp — 格式名标准化

  **What to do**:
  1. 在 `NormalizeContainerFormatName()` 的 switch 中新增：
     ```cpp
     case DetectedContainer::Ape:
         break;  // fall through to Unknown → uses FFmpeg containerName
     ```
     由于 `Ape` 和 `Unknown` 都走 `NormalizeFormatName(path, containerName)` 路径（FFmpeg 提供准确的容器名：ape/mpc/wv），可以让 `Ape` case 直接 fall through 到 `Unknown`。

  **Must NOT do**:
  - 不移除已有容器的 case
  - 不硬编码 "ape" 字符串（使用 FFmpeg 名以确保 Musepack→"mpc", WavPack→"wv"）

  **Recommended Agent Profile**: `quick` — 一行 fall-through

  **Parallelization**:
  - **Wave**: 2，与 Task 3,4,5 并行
  - **Blocks**: Task 7
  - **Blocked By**: Task 1,2

  **Acceptance Criteria**:
  - [ ] `NormalizeContainerFormatName()` 含 `case DetectedContainer::Ape:`
  - [ ] 编译通过

  **QA Scenarios**:
  ```
  Scenario: Monkey's Audio 文件 → format 字段为 "ape"
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成 .ape 测试文件
      2. ./build/TagReaderTest /tmp/test_ape.ape | grep -i "format"
    Expected: format 包含 "ape"
    Evidence: .omo/evidence/task-6-format-name.log
  ```

  **Commit**: YES
  - Message: `feat: add APE container format name in MediaInfoReader`
  - Files: `src/media/MediaInfoReader.cpp`

- [x] 7. TR-AUDIT 回归测试 — 2-3 个 APE 测试用例

  **What to do**:
  1. 创建 `test/regression/ape/` 目录，含动态生成测试文件的 Python 脚本
  2. 在 `test/regression/regression_tests.cpp` 中新增 2-3 个测试函数：

  **TR-AUDIT-016** — 合法 APEv2 全字段验证：
  - 动态生成 .ape 文件：Header("APETAGEX"+LE32(2000)+size+count+flags) + Items(Title/Artist/Album/Track/Year) + Footer
  - 验证 TagReaderTest 输出含所有字段

  **TR-AUDIT-017** — 畸形 APE tag 容错：
  - 动态生成 .ape 文件：tagSize 超过 kMaxApeTagBytes → 验证 parser 拒绝
  - itemCount 异常大 → 验证 parser 拒绝
  - version=1000 (APEv1) → 验证 parser 跳过

  **TR-AUDIT-018** — MP3+APE 优先级验证：
  - 动态生成 MP3+APE 混合文件
  - 验证 APE 字段优先于 ID3v2 字段
  - 验证 ID3v2 补缺 APE 未提供的字段

  **Must NOT do**:
  - 不提交生成的 .ape/.mp3 测试文件到 git
  - 不修改已有 TR-AUDIT 用例
  - 测试文件生成脚本放入 `test/regression/ape/generate.py`

  **Recommended Agent Profile**: `deep` — 需理解 TR-AUDIT 回归框架、python3 struct.pack 二进制生成、跨格式优先级验证

  **Parallelization**:
  - **Wave**: 3（依赖所有核心实现完成）
  - **Blocks**: 无
  - **Blocked By**: Task 3,4,5,6

  **Acceptance Criteria**:
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-016` PASS
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-017` PASS
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-018` PASS
  - [ ] 全部已有 TR-AUDIT 用例无回归

  **QA Scenarios**:
  ```
  Scenario: TR-AUDIT-016 — 全字段 APE 解析
    Tool: Bash
    Steps:
      1. python3 test/regression/ape/generate.py 016 → 生成测试文件
      2. ./build/TagReaderRegressionTests TR-AUDIT-016
    Expected: PASS，输出验证所有字段值正确
    Evidence: .omo/evidence/task-7-audit-016.log

  Scenario: TR-AUDIT-017 — 畸形容错
    Tool: Bash
    Steps:
      1. python3 test/regression/ape/generate.py 017 → 生成畸形文件
      2. ./build/TagReaderRegressionTests TR-AUDIT-017
    Expected: PASS，所有畸形场景被安全拒绝，无崩溃
    Evidence: .omo/evidence/task-7-audit-017.log
  ```

  **Commit**: YES
  - Message: `test: add APEv2 regression test cases`
  - Files: `test/regression/regression_tests.cpp`, `test/regression/ape/generate.py`

---

## Final Verification Wave

- [x] F1. **Build Integrity Check** — `quick`
  运行 `cmake --build build 2>&1`，确认零错误零警告。运行 `./build/TagReaderRegressionTests --list` 确认新增 TR-AUDIT 用例可见。
  Output: `Build [PASS/FAIL] | Tests visible [N/expected] | VERDICT`

- [x] F2. **Git History Check** — `quick`
  确认恰好 7 个 commit，消息格式符合 `feat:` / `fix:` 约定。`git diff --stat` 仅含目标文件。
  Output: `Commits [N/7] | Files [N expected] | VERDICT`

- [x] F3. **Regression Test Pass** — `quick`
  运行全部 TR-AUDIT 用例，确认无回归。新增 APE 用例 PASS。
  Output: `All existing [PASS/FAIL] | New APE [PASS/FAIL] | VERDICT`

- [x] F4. **Working Tree Cleanliness** — `quick`
  `git status` 干净，`/tmp` 测试文件清理。
  Output: `Git [CLEAN/DIRTY] | VERDICT`

---

## Momus Verdict

**Verdict**: OKAY
**Date**: 2026-06-04
**Verification**: All referenced files and functions exist. All TODO items have concrete acceptance criteria. QA scenarios executable. No blocking issues.

---

## Commit Strategy

- Task 1: `feat: add Ape to TagFormat and DetectedContainer enums`
- Task 2: `feat: add ApeLimits and CMake integration for APE parser`
- Task 3: `feat: implement APEv2 tag parser (metadata + lyrics + cover)`
- Task 4: `feat: add APE footer detection in ContainerDetector`
- Task 5: `feat: wire APE parser into TagPipeline (metadata + lyrics)`
- Task 6: `feat: add APE container format name in MediaInfoReader`
- Task 7: `test: add APEv2 regression test cases`

---

## Success Criteria

### Verification Commands
```bash
cmake --build build 2>&1 | tail -3
# Expected: 无 "error:"

./build/TagReaderRegressionTests --list | grep APE
# Expected: 输出新增 TR-AUDIT 用例 ID

./build/TagReaderRegressionTests TR-AUDIT-016
# Expected: PASS
```

### Final Checklist
- [x] 7 个 commit 存在且消息正确
- [x] `cmake --build build` 零错误
- [x] `TagReaderRegressionTests` 全部 PASS
- [x] `git status` 干净
- [x] `.omo/evidence/` 中每 task 有证据文件
