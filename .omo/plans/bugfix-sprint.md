# bugfix-sprint：TagReader 安全审计缺陷修复

## TL;DR

> **Quick Summary**: 修复 ANALYSIS.md 中 11 个安全审计缺陷（2 High + 5 Medium + 4 Low），覆盖 IO 流状态污染、整数溢出保护、编码零信任回退、符号链接防护、字段长度限制、ID3 恢复、LRC 上限。
>
> **Deliverables**:
> - 7 个源码文件精确修复（无重构、无额外副作用）
> - 每个任务含动态生成的畸形测试音频 + 验证脚本
> - 原子化 git commit，每任务 1 个 commit
>
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 7 个任务全部并行（文件完全独立，无冲突）
> **Critical Path**: 无（纯并行）

---

## Context

### 原始需求

基于 `ANALYSIS.md` 中的完整安全审计结果，修复以下缺陷：

| 编号 | 严重度 | 文件 | 简述 |
|------|--------|------|------|
| HIGH-001 | 🟠 | `src/core/TagPipeline.cpp` | 异常吞噬后 IO 流 failbit 残留 |
| HIGH-002 | 🟠 | `src/formats/flac/FlacParser.cpp` | `fileSize - cursor` 无符号回绕 |
| MEDIUM-001 | 🟡 | `src/text/TextCodec.cpp` | 无 iconv 时直接回退 latin-1 |
| MEDIUM-002 | 🟡 | `src/media/FfmpegSession.cpp` | FFmpeg 无符号链接防护 |
| MEDIUM-003 | 🟡 | `src/text/TextNormalize.cpp` | 最终字段无长度上限 |
| MEDIUM-004 | 🟡 | `src/formats/id3/Id3Frames.cpp` | 畸形帧后丢弃所有后续帧 |
| MEDIUM-005 | 🟡 | `src/core/TagPipeline.cpp` | ID3v2→ID3v1 间 stream 污染 |
| LOW-001 | 🔵 | `src/cover/CoverDecoder.cpp` | const_cast 缺少所有权注释 |
| LOW-002 | 🔵 | `src/text/TextCodec.cpp` | Latin-1 遇 0x00 截断 |
| LOW-003 | 🔵 | `src/text/TextCodec.cpp` | UTF-16 无 BOM 启发式误判 |
| LOW-004 | 🔵 | `src/text/TextNormalize.cpp` | LRC 分钟数无上限 |

另含架构改进 AD-001（阶段间 stream clear）。

### 文件依赖拓扑分析

```
文件 → 涉及的 Bug：
├── src/core/TagPipeline.cpp          ← HIGH-001, MEDIUM-005, AD-001   (3 bugs，同文件)
├── src/text/TextCodec.cpp            ← MEDIUM-001, LOW-002, LOW-003   (3 bugs，同文件)
├── src/text/TextNormalize.cpp        ← MEDIUM-003, LOW-004            (2 bugs，同文件)
├── src/formats/flac/FlacParser.cpp   ← HIGH-002                      (1 bug)
├── src/media/FfmpegSession.cpp       ← MEDIUM-002                    (1 bug)
├── src/formats/id3/Id3Frames.cpp     ← MEDIUM-004                    (1 bug)
└── src/cover/CoverDecoder.cpp        ← LOW-001                       (1 bug)
```

**关键结论**：7 个文件两两互不重叠。每文件内部有多个修改点，但由单一任务原子完成（文件内修改由执行 Agent 自行串行）。**所有 7 个任务可以全并行启动**。

---

## Work Objectives

### Core Objective

将 ANALYSIS.md 中 P0/P1/P2 优先级的 11 个安全缺陷一次性修复，每个修复附带可自动验证的测试用例。

### Concrete Deliverables

- 7 个原子化 git commit
- 每个 commit 仅含源码修改（测试脚本不提交）
- 修复后全部现有测试通过（`TagReaderRegressionTests` 回归绿灯）

### Must Have

- HIGH-001 + MEDIUM-005 + AD-001：TagPipeline.cpp 中 4 处 stream clear 恢复
- HIGH-002：FlacParser.cpp 中 TryAddUintmax 替换
- MEDIUM-001：TextCodec.cpp 中无 iconv 时的编码置信度标记
- MEDIUM-002：FfmpegSession.cpp 中符号链接拒绝

### Must NOT Have

- 不重构无关代码（如整个函数重写）
- 不修改 public API 签名（TagReader::Read 不变）
- 不引入新依赖
- 不修改 CMakeLists.txt（除非为回归测试增加必需的编译目标）
- 不提交测试音频文件和生成脚本到 git

---

## Verification Strategy (MANDATORY)

### Test Decision

- **Infrastructure exists**: YES（TagReaderRegressionTests 已存在）
- **Automated tests**: Tests-after（先修 Bug，再写验证测试）
- **Framework**: 手动编译临时测试程序（链接 TagReaderCore），验证后删除
- **Agent-Executed QA**: 全部

### QA Policy

所有测试通过以下方式执行：
- **API/Backend**: 编译临时 .cpp 测试文件 → 链接 TagReaderCore 静态库 → 运行验证
- **测试二进制生成介质**: Python 脚本（struct.pack 构造畸形字节）+ 系统命令（ln -s 创建符号链接）
- **目录使用**: `/tmp` 用于一次性测试产物；`./tmp` 用于复用的生成脚本（执行时自动创建）
- **证据保存**: 测试输出保存到 `.omo/evidence/task-{N}-*.log`

---

## Execution Strategy

```
全部并行 — 7 个任务同时启动，互不阻塞
```

```
Wave 1 (7 tasks PARALLEL — 所有文件独立):
├── Task 1: HIGH-002 — FlacParser.cpp TryAddUintmax [quick]
├── Task 2: MEDIUM-002 — FfmpegSession.cpp symlink guard [quick]
├── Task 3: MEDIUM-004 — Id3Frames.cpp frame resync [deep]
├── Task 4: LOW-001 — CoverDecoder.cpp const_cast comment [quick]
├── Task 5: HIGH-001 + MEDIUM-005 + AD-001 — TagPipeline.cpp stream recovery [deep]
├── Task 6: MEDIUM-001 + LOW-002 + LOW-003 — TextCodec.cpp encoding fixes [deep]
└── Task 7: MEDIUM-003 + LOW-004 — TextNormalize.cpp field limits [quick]
```

---

## TODOs

- [x] 1. HIGH-002 — FlacParser.cpp：ReadFlacLyrics() 改用 TryAddUintmax

  **What to do**:
  1. 打开 `src/formats/flac/FlacParser.cpp`，定位 `ReadFlacLyrics()` 函数中第 293 行附近的 `blockSize > context.fileSize - cursor` 检查
  2. 将其替换为 `TryAddUintmax(cursor, blockSize, blockEnd)` 模式（参照同文件 `ReadFlacMetadataBlocks()` 第 192 行的写法）
  3. 其他代码逻辑不做任何修改

  **Must NOT do**:
  - 不要修改 `ReadFlacMetadataBlocks()` 的现有代码
  - 不要重构 `ReadFlacLyrics()` 的完整实现
  - 不要改动函数签名

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 单文件单函数单行替换，无复杂逻辑
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 2-7 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `src/formats/flac/FlacParser.cpp` 第 293 行已改为 `TryAddUintmax` 模式
  - [ ] 编译通过：`cmake --build build 2>&1 | grep -i "error"` 无输出

  **QA Scenarios**:

  ```
  Scenario: cursor > fileSize 时不回绕（HAPPY PATH — 安全防护验证）
    Tool: Bash (g++)
    Preconditions: TagReaderCore 已编译（cmake --build build）
    Steps:
      1. 创建 /tmp/test_flac_overflow.cpp，内容如下：
         - #include "io/ByteReader.hpp"
         - 调用 TryAddUintmax(cursor=100, blockSize=50, blockEnd)
         - 调用 TryAddUintmax(cursor=UINTMAX_MAX-10, blockSize=20, blockEnd) 期望返回 false（溢出）
         - 调用 TryAddUintmax(cursor=100, blockSize=50, blockEnd) 并检查 blockEnd == 150
         - 输出 "PASS" 或 "FAIL"
      2. 编译：g++ -std=c++23 -I include -I src /tmp/test_flac_overflow.cpp src/io/ByteReader.cpp -o /tmp/test_flac_overflow
      3. 运行：/tmp/test_flac_overflow
    Expected Result: 输出 "PASS"（所有 TryAddUintmax 调用行为正确）
    Failure Indicators: 输出 "FAIL" 或编译失败
    Evidence: .omo/evidence/task-1-overflow.log
  ```

  **Commit**: YES
  - Message: `fix: ReadFlacLyrics uses TryAddUintmax to prevent unsigned wrap`
  - Files: `src/formats/flac/FlacParser.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 2. MEDIUM-002 — FfmpegSession.cpp：avformat_open_input 前增加符号链接检查

  **What to do**:
  1. 打开 `src/media/FfmpegSession.cpp`，定位 `OpenContext()` 函数中 `avformat_open_input()` 调用（约第 68 行）
  2. 在调用前添加：使用 `std::filesystem::is_symlink(path)` 检查
  3. 如果是符号链接，抛出带清晰错误信息的 `std::runtime_error`
  4. 需要 `#include <system_error>`（若文件头部尚未包含）

  **Must NOT do**:
  - 不要修改 `avformat_open_input` 的其他参数
  - 不要添加 `realpath()` 解析（符号链接应拒绝而非跟随）
  - 不要改动其他函数的逻辑

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 单文件、单函数、前置检查插入，逻辑简单
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 1,3-7 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `src/media/FfmpegSession.cpp` OpenContext() 中 avformat_open_input 调用前有 is_symlink 检查
  - [ ] 编译通过

  **QA Scenarios**:

  ```
  Scenario: 传入符号链接被拒绝（HAPPY PATH）
    Tool: Bash
    Preconditions: 存在可用的音频文件（通过 find 或 python 脚本创建最小合法 MP3）
    Steps:
      1. python3 -c "
import struct
# 创建最小合法 MP3 frame (ID3v2 header + MPEG frame)
data = b'ID3' + struct.pack('>I', 0) + b'\x00\x00'  # 空 ID3v2 tag
data += b'\xff\xfb\x90\x00'  # MPEG frame header
with open('/tmp/test_real.mp3', 'wb') as f: f.write(data)
"
      2. ln -sf /tmp/test_real.mp3 /tmp/test_symlink.mp3
      3. ./build/TagReaderTest /tmp/test_symlink.mp3
    Expected Result: 程序以错误退出（exit code != 0），stderr 包含 "symbolic link" 或 "symlink"
    Failure Indicators: 程序正常执行（exit code 0）或崩溃
    Evidence: .omo/evidence/task-2-symlink-reject.log

  Scenario: 传入普通文件正常通过（回归验证）
    Tool: Bash
    Steps:
      1. ./build/TagReaderTest /tmp/test_real.mp3
    Expected Result: 程序正常退出或报告标签缺失（非符号链接错误），exit code 为 0 或 1（文件无可用标签场景）
    Failure Indicators: 程序因符号链接错误退出（exit code 2 且 stderr 含 "symlink"）
    Evidence: .omo/evidence/task-2-normal-file.log
  ```

  **Commit**: YES
  - Message: `fix: reject symbolic links in FfmpegSession::OpenContext`
  - Files: `src/media/FfmpegSession.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 3. MEDIUM-004 — Id3Frames.cpp：ID3 frame walker 扩大 resync 触发范围

  **What to do**:
  1. 打开 `src/formats/id3/Id3Frames.cpp`
  2. 定位 `ReadID3v23Or24Frames()` 和 `ReadID3v22Frames()` 函数
  3. 当前逻辑：遇到非法 frame ID 或 `frameSize == 0` 时直接 `break`
  4. 新逻辑：在 `break` 之前，先尝试调用 `TryResyncId3v22Frame()` / `TryResyncId3v23Or24Frame()` 进行恢复
  5. 若 resync 成功（返回有效的 cursor），继续扫描；若失败，保持原 `break` 行为
  6. **关键**：resync 仅尝试有限次（建议最多 1 次尝试），且限制扫描预算为 4096 字节——防止退化扫描

  **Must NOT do**:
  - 不要修改 padding（全 0x00）的检测逻辑——padding 检测到仍应停止
  - 不要改动 frame 解析的内部逻辑
  - 不要修改 resync 函数本身的实现（它们已存在且工作正常）

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 涉及 ID3 状态机恢复逻辑，需要仔细处理 resync 正确性和扫描预算
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 1-2,4-7 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `ReadID3v23Or24Frames()` 和 `ReadID3v22Frames()` 在 break 前有 resync 尝试
  - [ ] resync 扫描预算 ≤ 4096 字节
  - [ ] 编译通过
  - [ ] 现有回归测试 `./build/TagReaderRegressionTests TR-AUDIT-006` PASS

  **QA Scenarios**:

  ```
  Scenario: 畸形帧后跟合法帧 — 合法帧应被扫描到（HAPPY PATH）
    Tool: Bash (python3 + TagReaderTest)
    Preconditions: none
    Steps:
      1. python3 << 'EOF' 生成测试文件
import struct
# ID3v2.3 tag: header + 畸形 frame + 合法 TIT2 frame + 合法 TPE1 frame
tag_body = b''
# 畸形 frame: 非法 frame ID "XXXX" + size=10 + flags=0 + 10字节垃圾数据
tag_body += b'XXXX' + struct.pack('>I', 10) + b'\x00\x00' + b'\x00' * 10
# 合法 TIT2 frame: "TIT2" + size=5 + flags=0 + encoding=3(UTF-8) + "Test"
tag_body += b'TIT2' + struct.pack('>I', 5) + b'\x00\x00' + b'\x03' + b'Test'
# ID3 header: "ID3" + version 3.0 + flags=0 + syncsafe size
id3_size = len(tag_body)
id3_header = b'ID3' + b'\x03\x00' + b'\x00' + struct.pack('>I', ((id3_size >> 21) & 0x7F) << 24 | ((id3_size >> 14) & 0x7F) << 16 | ((id3_size >> 7) & 0x7F) << 8 | (id3_size & 0x7F))
# MPEG frame header (让 FFmpeg 识别为音频)
mpeg = b'\xff\xfb\x90\x00'
with open('/tmp/test_id3_resync.mp3', 'wb') as f:
    f.write(id3_header + tag_body + mpeg)
EOF
      2. ./build/TagReaderTest /tmp/test_id3_resync.mp3
    Expected Result: title 字段输出 "Test"（跳过了畸形帧 "XXXX"）
    Failure Indicators: title 为空（resync 未生效，整个 tag 被跳过）
    Evidence: .omo/evidence/task-3-resync-title.log

  Scenario: 全 padding 应正常停止（边界条件）
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成 ID3v2.3 tag，body 全部为 0x00 字节（模拟 padding）
      2. ./build/TagReaderTest /tmp/test_id3_padding.mp3
    Expected Result: 正常退出，title 为空（padding 后无合法帧）
    Failure Indicators: 程序崩溃或挂起
    Evidence: .omo/evidence/task-3-padding-stop.log

  Scenario: 退化扫描预算限制（安全验证）
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. 生成 ID3v2.3 tag，前 5000 字节为随机非零字节（无有效 frame header）
      2. ./build/TagReaderTest /tmp/test_id3_degen.mp3
    Expected Result: 在不超时的情况下正常退出（resync 扫描被预算限制截断）
    Failure Indicators: 程序运行超过 5 秒（退化扫描未受限）
    Evidence: .omo/evidence/task-3-budget.log
  ```

  **Commit**: YES
  - Message: `fix: expand ID3 frame resync to cover illegal frame IDs and zero-size frames`
  - Files: `src/formats/id3/Id3Frames.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 4. LOW-001 — CoverDecoder.cpp：const_cast 处添加所有权注释

  **What to do**:
  1. 打开 `src/cover/CoverDecoder.cpp`，定位 `DecodeAndEncodeCoverPng()` 函数中 `packet->data = const_cast<uint8_t*>(data)` 语句（约第 207 行）
  2. 在 const_cast 语句前添加注释，说明：
     - `data` 为外部只读 buffer
     - `packet` 不持有所有权（`packet->buf` 保持 nullptr）
     - `av_packet_free` 不会尝试释放外部内存
  3. **仅添加注释，不修改任何代码逻辑**

  **Must NOT do**:
  - 不要修改 const_cast 为其他写法
  - 不要添加 `packet->buf = av_buffer_create(...)` 等内存管理逻辑
  - 不要改动函数其他部分

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 纯注释添加，零逻辑修改
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 1-3,5-7 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `src/cover/CoverDecoder.cpp` 中 const_cast 语句上方有清晰的所有权说明注释
  - [ ] 编译通过（注释不影响编译，但确认文件语法完整）

  **QA Scenarios**:

  ```
  Scenario: 编译通过确认语法无损坏
    Tool: Bash (cmake)
    Steps:
      1. cmake --build build 2>&1
    Expected Result: 编译成功无错误
    Failure Indicators: 编译错误（注释语法问题导致）
    Evidence: .omo/evidence/task-4-build.log
  ```

  **Commit**: YES
  - Message: `docs: document AVPacket external buffer ownership in CoverDecoder`
  - Files: `src/cover/CoverDecoder.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 5. HIGH-001 + MEDIUM-005 + AD-001 — TagPipeline.cpp：IO 流状态恢复三重修复

  **What to do**:
  打开 `src/core/TagPipeline.cpp`，在同一文件中完成 **4 处**修改：

  **修改点 A（HIGH-001）** — `ignoreMalformedMetadata` lambda 的 catch 块（约第 104-119 行）：
  - 在 `catch (const std::filesystem::filesystem_error &)` 块中添加 `context.input.clear();`
  - 在 `catch (const std::runtime_error &ex)` 块的非 throw 路径中添加 `context.input.clear();`

  **修改点 B（MEDIUM-005）** — `ReadMetadata()` ID3v2 分支中 ID3v1 调用前（约第 140-145 行区域）：
  - 在 ID3v2 parser 调用和 ID3v1 parser 调用之间插入 `context.input.clear();`

  **修改点 C（AD-001）** — `ReadMetadata()` 函数入口（parser 调用之前）：
  - 在函数体开头插入 `context.input.clear();` 作为防御性重置

  **修改点 D（AD-001）** — `ReadLyrics()` 函数入口（`is_open()` 检查通过后）：
  - 在 `is_open()` 检查通过后、parser 调用前插入 `context.input.clear();`

  **Must NOT do**:
  - 不要修改 `IsCoverExportOrCacheError()` 函数的逻辑
  - 不要修改 parser 调用顺序
  - 不要删除或改写已有的异常处理代码（仅在 catch 块内新增 clear 调用）

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 涉及核心管线的异常安全和 IO 状态管理，4 处修改需精确放置
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 1-4,6-7 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `ignoreMalformedMetadata` lambda 的两个 catch 块中均有 `context.input.clear()`
  - [ ] `ReadMetadata()` ID3v1 调用前有 `context.input.clear()`
  - [ ] `ReadMetadata()` 和 `ReadLyrics()` 入口处有 `context.input.clear()`
  - [ ] 编译通过

  **QA Scenarios**:

  ```
  Scenario: 畸形 ID3v2 metadata + 合法歌词 — 歌词仍可解析（HAPPY PATH）
    Tool: Bash (python3 + TagReaderTest)
    Preconditions: TagReaderCore 已编译
    Steps:
      1. python3 << 'PYEOF' 生成测试文件
import struct
# 畸形 TIT2 frame (frameSize=0xFFFFFFFF 超出剩余tag) + 合法 USLT 歌词
tag_body = b'TIT2' + struct.pack('>I', 0xFFFFFFFF) + b'\x00\x00'
uslt_text = b'\x03eng\x00\x00hello world'
tag_body += b'USLT' + struct.pack('>I', len(uslt_text)) + b'\x00\x00' + uslt_text
id3_size = len(tag_body)
syncsafe = lambda n: ((n>>21)&0x7F)<<24|((n>>14)&0x7F)<<16|((n>>7)&0x7F)<<8|(n&0x7F)
id3_hdr = b'ID3\x03\x00\x00' + struct.pack('>I', syncsafe(id3_size))
with open('/tmp/test_stream_pollution.mp3', 'wb') as f:
    f.write(id3_hdr + tag_body + b'\xff\xfb\x90\x00')
PYEOF
      2. ./build/TagReaderTest /tmp/test_stream_pollution.mp3
    Expected Result: lyricsCount > 0（歌词 "hello world" 被成功解析）
    Failure Indicators: lyricsCount == 0
    Evidence: .omo/evidence/task-5-lyrics-after-bad-metadata.log

  Scenario: ID3v2 header 损坏但 ID3v1 尾有数据 — ID3v1 仍被读取
    Tool: Bash (python3 + TagReaderTest)
    Steps:
      1. python3 << 'PYEOF' 生成含损坏 ID3v2 + 合法 ID3v1 尾的文件
import struct
bad_id3 = b'ID3\x04\x00\x00' + b'\x00\x00\x00\x00'
body = b'\xff\xfb\x90\x00' + b'\x00' * 1024
id3v1 = b'TAG' + b'ID3v1Title'.ljust(30, b'\x00') + b'Artist'.ljust(30, b'\x00') + b'Album'.ljust(30, b'\x00') + b'2024'.ljust(4, b'\x00') + b'Comment'.ljust(28, b'\x00') + b'\x00' + b'\x01'
with open('/tmp/test_id3v1_fallback.mp3', 'wb') as f:
    f.write(bad_id3 + body + id3v1)
PYEOF
      2. ./build/TagReaderTest /tmp/test_id3v1_fallback.mp3
    Expected Result: title 包含 "ID3v1Title"
    Failure Indicators: title 为空
    Evidence: .omo/evidence/task-5-id3v1-fallback.log
  ```

  **Commit**: YES
  - Message: `fix: recover IO stream state after swallowed exceptions in TagPipeline`
  - Files: `src/core/TagPipeline.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 6. MEDIUM-001 + LOW-002 + LOW-003 — TextCodec.cpp：编码零信任文档化加固

  **What to do**:
  打开 `src/text/TextCodec.cpp`，完成 **3 处**文档化修改（纯注释，零逻辑变更）：

  **修改点 A（MEDIUM-001）** — `DetectLegacyLocalEncoding()` 约第 150 行：
  - 在 `#else` 分支的 `return "latin-1";` 前添加多行注释，标注无 iconv 构建的限制：
    ```
    // WARNING: 无 iconv 支持时跳过 GB18030/GBK/SHIFT_JIS/BIG5 等编码探测，
    // 所有非 BOM、非 UTF-8、非明显 UTF-16 的文本均按 Latin-1 回退处理。
    // CJK 文本在此构建中可能产生乱码。建议生产环境启用 iconv 支持。
    ```

  **修改点 B（LOW-002）** — `ReadLatin1Text()` 约第 463 行：
  - 在 `if (static_cast<unsigned char>(byte) == 0x00) { break; }` 处添加注释：
    ```
    // Latin-1 编码的音频标签文本字段极少含内部 0x00 字节。
    // 当前按 C 风格字符串截断处理；若未来需要支持含 0x00 的 binary-embedded 文本，可改为替换为空格。
    ```

  **修改点 C（LOW-003）** — `LooksLikeUtf16WithoutBom()` 约第 56 行：
  - 在函数顶部添加注释标注启发式阈值的已知限制：
    ```
    // 已知限制：统计启发式（nulOnHighByte * 3 >= units * 2）可能在恰好符合
    // ASCII 分布、低控制字符比例的畸形数据上产生误判，将非 UTF-16 数据误识别为 UTF-16。
    ```

  **Must NOT do**:
  - 不要修改任何一行代码逻辑
  - 不要引入新依赖或修改 CMakeLists.txt

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 纯注释添加，零逻辑修改
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 1-5,7 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `DetectLegacyLocalEncoding()` 无 iconv 分支有注释标注
  - [ ] `ReadLatin1Text()` 0x00 截断处有设计理由注释
  - [ ] `LooksLikeUtf16WithoutBom()` 顶部有误判风险注释
  - [ ] 编译通过

  **QA Scenarios**:

  ```
  Scenario: 编译通过确认语法无损坏
    Tool: Bash (cmake)
    Steps:
      1. cmake --build build 2>&1
    Expected Result: 编译成功，无新增错误或警告
    Failure Indicators: 编译错误
    Evidence: .omo/evidence/task-6-build.log
  ```

  **Commit**: YES
  - Message: `docs: document encoding zero-trust limitations in TextCodec`
  - Files: `src/text/TextCodec.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

- [x] 7. MEDIUM-003 + LOW-004 — TextNormalize.cpp：字段长度限制与 LRC 分钟上限

  **What to do**:
  打开 `src/text/TextNormalize.cpp`，完成 **2 处**修改：

  **修改点 A（MEDIUM-003）** — `NormalizeMetadata()` 函数（约第 162-183 行）：
  - 在现有 `TrimText` 和 `IsValidUtf8` 校验之间，增加字段长度限制
  - 添加常量：`constexpr std::size_t kMaxFinalTextFieldBytes = 65536;`（64 KiB）
  - 若 `field.size() > kMaxFinalTextFieldBytes`，截断到限制大小
  - 对所有字段（title、artist、album、albumArtist、genre、composer）统一应用

  **修改点 B（LOW-004）** — `ParseLrcTimestamp()` 函数（约第 118-121 行）：
  - 在 `ParseDecimalU16Strict()` 解析分钟数后，增加合理性上限检查
  - 添加常量：`constexpr uint16_t kMaxLrcMinutes = 999;`
  - 若 `minutes > kMaxLrcMinutes`，返回 `std::nullopt`

  **Must NOT do**:
  - 不要修改 trim 逻辑本身
  - 不要改动其他解析函数的签名
  - 截断时不要改变 UTF-8 完整性（应在合法 code point 边界截断）

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 两处独立的上限检查添加，逻辑简单
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1（与 Task 1-6 并行）
  - **Blocks**: 无
  - **Blocked By**: 无

  **Acceptance Criteria**:
  - [ ] `NormalizeMetadata()` 中有 `kMaxFinalTextFieldBytes = 65536` 长度截断
  - [ ] `ParseLrcTimestamp()` 中有 `kMaxLrcMinutes = 999` 上限检查
  - [ ] 编译通过

  **QA Scenarios**:

  ```
  Scenario: 超长字段被截断（HAPPY PATH）
    Tool: Bash (g++)
    Preconditions: TagReaderCore 已编译
    Steps:
      1. 创建 /tmp/test_field_limit.cpp，构造超长 UTF-8 字符串（70 KiB 的 "A"），调用 NormalizeMetadata
      2. 编译并运行：验证输出字符串长度 ≤ 65536
    Expected Result: 截断后长度 ≤ 65536，且不因非 code-point 边界截断导致非法 UTF-8
    Failure Indicators: 输出长度 > 65536 或 NotValidUtf8
    Evidence: .omo/evidence/task-7-field-limit.log

  Scenario: LRC 超限分钟数被拒绝（边界条件）
    Tool: Bash (g++)
    Preconditions: TagReaderCore 已编译
    Steps:
      1. 创建 /tmp/test_lrc_minutes.cpp，构造 LRC 行 "[65535:00.00]test"，调用文本解析
      2. 验证分钟数 65535 > 999 导致返回 nullopt 或歌词为空
    Expected Result: 歌词未被接受（timestamp 被拒绝）
    Failure Indicators: 歌词被正常接受
    Evidence: .omo/evidence/task-7-lrc-minutes.log

  Scenario: 合法分钟数（≤999）正常解析（回归验证）
    Tool: Bash (g++)
    Steps:
      1. 使用分钟数=59 的合法 LRC 行进行测试
      2. 验证歌词被正常解析
    Expected Result: 歌词解析成功
    Evidence: .omo/evidence/task-7-lrc-normal.log
  ```

  **Commit**: YES
  - Message: `fix: add max field length and LRC minutes cap in TextNormalize`
  - Files: `src/text/TextNormalize.cpp`
  - Pre-commit: `cmake --build build 2>&1 | tail -5`

---

## Final Verification Wave (MANDATORY — after ALL 7 tasks)

> 4 个 review 验证并行执行。全部必须 PASS。

- [x] F1. **Build Integrity Check** — `quick`
  运行 `cmake --build build 2>&1`，确认 7 个 commit 后编译零错误零警告。运行 `./build/TagReaderRegressionTests` 确认所有已有 TR-AUDIT 用例 PASS。
  Output: `Build [PASS/FAIL] | RegressionTests [N pass/N fail] | VERDICT`

- [x] F2. **Git History Check** — `quick`
  运行 `git log --oneline -7`，确认恰好 7 个新 commit，每个 commit 的消息格式符合 `fix:` 或 `docs:` 约定。运行 `git diff HEAD~7 --stat` 确认仅修改了 7 个目标 .cpp 文件。
  Output: `Commits [7/7] | Files [7 expected] | VERDICT`

- [x] F3. **Evidence Completeness** — `quick`
  检查 `.omo/evidence/` 目录，确认每个 task 至少生成了 1 个证据文件（task-1 到 task-7 均有 .log 文件）。
  Output: `Evidence [N/7 tasks] | VERDICT`

- [x] F4. **Working Tree Cleanliness** — `quick`
  运行 `git status`，确认无未提交修改（所有 .cpp 修改已 commit）。确认 /tmp 下的测试文件已被清理。确认 ./tmp 下无遗留物。
  Output: `Git [CLEAN/DIRTY] | /tmp [CLEAN/DIRTY] | VERDICT`

---

## Commit Strategy

所有 7 个 task 均独立 commit，消息格式：

- Task 1: `fix: ReadFlacLyrics uses TryAddUintmax to prevent unsigned wrap`
- Task 2: `fix: reject symbolic links in FfmpegSession::OpenContext`
- Task 3: `fix: expand ID3 frame resync to cover illegal frame IDs and zero-size frames`
- Task 4: `docs: document AVPacket external buffer ownership in CoverDecoder`
- Task 5: `fix: recover IO stream state after swallowed exceptions in TagPipeline`
- Task 6: `docs: document encoding zero-trust limitations in TextCodec`
- Task 7: `fix: add max field length and LRC minutes cap in TextNormalize`

---

## Success Criteria

### Verification Commands
```bash
# 完整构建
cmake --build build 2>&1 | tail -3
# Expected: 无 "error:"

# 回归测试全绿
./build/TagReaderRegressionTests 2>&1 | tail -5
# Expected: 所有已实现用例 PASS

# Git 历史干净
git log --oneline -7
# Expected: 7 个新 commit，消息如上
```

### Final Checklist
- [ ] 7 个 commit 全部存在且消息格式正确
- [ ] `cmake --build build` 零错误
- [ ] `TagReaderRegressionTests` 全部 PASS
- [ ] `git status` 干净（无未提交修改）
- [ ] `.omo/evidence/` 中每个 task 有证据文件
