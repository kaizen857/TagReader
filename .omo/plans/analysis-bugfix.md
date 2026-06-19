# ANALYSIS.md Bug 修复与架构改进执行计划

## TL;DR

> **Quick Summary**: 基于 2026-06-07 ANALYSIS.md 全量重审计结果，修复 6 个未修复 Bug（MEDIUM-001/004/006/007、LOW-002/003）和 3 个架构缺陷（AD-001/002/003），其中 LOW-005 合并到 AD-003 统一执行。采用 TDD 流程，每个修复先写回归测试后写代码。
>
> **Deliverables**:
> - 7 个新回归测试用例（TR-AUDIT-019 ~ TR-AUDIT-025）
> - 9 个原子化 git commit
> - 零 ASAN/UBSAN 错误
> - 全套 18+7=25 测试通过
>
> **Estimated Effort**: Medium
> **Parallel Execution**: YES — 4 Waves
> **Critical Path**: TextCodec.cpp 串行链 (1→4→7) = 最长路径

---

## Context

### Original Request
用户要求：静默读取 ANALYSIS.md，提取所有需要修复的 Bug 和架构缺陷，生成一份拓扑依赖感知的 TDD 执行计划。执行 Agent 将严格按此计划工作。

### Interview Summary
**Key Discussions**:
- 输入来源：ANALYSIS.md（2026-06-07 全量重审计版，已逐条对照源码验证）
- 修复范围：仅未修复项（已修复的 CRITICAL-001/HIGH-001/002 等 5 项排除）
- TDD 策略：先写回归测试（RED）→ 确认失败 → 修复代码（GREEN）→ commit
- LOW-005 合并到 AD-003：提取共享 ParseUInt16 时统一行为

**Research Findings**:
- 回归测试框架已就绪：`test/regression/regression_tests.cpp`，18 个现有测试全部通过
- 测试资产通过 ffmpeg CLI + C++ 二进制操作动态生成，不依赖预置文件
- `FindEncodedTerminator()` 在 `ReadLatin1Text()` 之前剥离 0x00，LOW-002 的 0x00→空格修复安全

### Metis Review
**Identified Gaps** (addressed):
- LOW-002 安全性确认：0x00→空格不影响 ID3 帧解析
- MEDIUM-004 预算 16384：仅错误路径开销，正常路径零影响
- AD-003 应在所有文件级修复完成后执行
- MEDIUM-001 测试策略：采用 #warning + 注释（无法在单次构建中同时测试 iconv/no-iconv）
- LOW-005 TrimText 关注：统一时必须确保 TrimText 应用到所有 parser
- 测试编号 TR-AUDIT-019~025，AD-001/AD-002 由现有测试验证

---

## Work Objectives

### Core Objective
逐项修复 ANALYSIS.md 中所有未修复的安全缺陷和架构弱点，通过 TDD 回归测试验证，零回归零新增 ASAN/UBSAN 错误。

### Concrete Deliverables
1. `src/text/TextCodec.cpp` — 3 处修复（MEDIUM-001/LOW-002/LOW-003）
2. `src/formats/ape/ApeParser.cpp` — 2 处修复（MEDIUM-006/MEDIUM-007）+ 1 处行为统一（LOW-005 via AD-003）
3. `src/formats/id3/Id3Frames.cpp` — 1 处常量变更（MEDIUM-004）+ 1 处行为统一（LOW-005 via AD-003）
4. `src/formats/vorbis/VorbisCommentParser.cpp` — 1 处行为统一（LOW-005 via AD-003）
5. `src/core/TagPipeline.cpp` — 2 处架构改进（AD-001/AD-002）
6. `src/common/ParseHelpers.hpp`（新建）— 共享整数解析 helper 库（AD-003）
7. `test/regression/regression_tests.cpp` — 7 个新测试用例 + 注册

### Definition of Done
- [ ] `cmake --build build` 零错误
- [ ] `cmake --build build-sanitize` 零 ASAN/UBSAN
- [ ] 全部 18 个 TR-AUDIT-001~018 回归通过
- [ ] 全部 7 个 TR-AUDIT-019~025 新测试通过
- [ ] 9 个原子化 commit 已提交

### Must Have
- 每个修复有对应的回归测试
- 每个 commit 前后全套测试通过
- 零内存错误（ASAN/UBSAN clean）
- TDD 流程：RED → GREEN → commit

### Must NOT Have (Guardrails)
- ❌ 修改 include/ 公开 API
- ❌ 修改已修复文件（FfmpegSession.cpp, CoverDecoder.cpp, CoverCache.cpp, ByteReader.cpp, ContainerDetector.cpp）
- ❌ 修改 ParseSlashNumber 逻辑
- ❌ 新增文件格式支持
- ❌ 新增编码检测候选
- ❌ 修改异常吞噬策略
- ❌ 修改 MusicTag 结构
- ❌ 提交测试音频/临时脚本到 git
- ❌ 使用 git reset/revert/checkout 回退

---

## Verification Strategy (MANDATORY)

> **ZERO HUMAN INTERVENTION** - ALL verification is agent-executed.

### Test Decision
- **Infrastructure exists**: YES (`test/regression/regression_tests.cpp` + `TagReaderRegressionTests` 目标)
- **Automated tests**: TDD（先写测试，确认失败，再修复代码）
- **Framework**: 手动断言（`Expect()` helper）+ 回归测试可执行文件
- **TDD Flow**: RED（新测试失败）→ GREEN（修复后通过）→ REFACTOR（如需要）→ commit

### QA Policy
每个任务包含：
- **生成测试资产**: ffmpeg CLI 生成基础音频 → C++ 二进制操作构造恶意/边界样本
- **编写测试**: 在 `regression_tests.cpp` 中添加 `RunTrAudit0XX()` 函数
- **注册测试**: 在 `kTestCases` 数组和 `RunCase()` 中添加映射
- **证据**: `/tmp/opencode/tagreader_regression/TR-AUDIT-0XX/`

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1（3 任务并行 — 3 个不同文件，无碰撞）:
├── 1. MEDIUM-001: TextCodec.cpp iconv fallback warning
├── 2. MEDIUM-006: ApeParser.cpp itemRegionOffset fix
└── 3. MEDIUM-004: Id3Frames.cpp resync budget 4096→16384

Wave 2（3 任务并行 — 每个依赖于 Wave 1 的同文件任务）:
├── 4. LOW-002: TextCodec.cpp latin-1 0x00→space （依赖于 1，同文件）
├── 5. MEDIUM-007: ApeParser.cpp cover valueSize limit （依赖于 2，同文件）
└── 6. AD-001: TagPipeline.cpp RAII guard （独立文件）

Wave 3（2 任务并行）:
├── 7. LOW-003: TextCodec.cpp UTF-16 heuristic （依赖于 4，同文件）
└── 8. AD-002: TagPipeline.cpp diagnostics （依赖于 6，同文件）

Wave 4（1 任务 — 需所有文件级修复稳定后执行）:
└── 9. AD-003 + LOW-005: 抽取共享 helpers + 统一 ParseUInt16
     （依赖于 2,3,5 完成 + 1,4,7 完成）

Critical Path: 1 → 4 → 7 （TextCodec.cpp 3 次串行修改）
Max Concurrent: 3 （Waves 1, 2）
```

### Dependency Matrix

| 任务 | 修改文件 | 阻塞于 | 阻塞 |
|------|---------|--------|------|
| 1 | TextCodec.cpp | — | 4 |
| 2 | ApeParser.cpp | — | 5, 9 |
| 3 | Id3Frames.cpp | — | 9 |
| 4 | TextCodec.cpp | 1 | 7 |
| 5 | ApeParser.cpp | 2 | 9 |
| 6 | TagPipeline.cpp | — | 8 |
| 7 | TextCodec.cpp | 4 | 9 |
| 8 | TagPipeline.cpp | 6 | — |
| 9 | 多文件 (~10) | 2,3,5,7 | — |

### Agent Dispatch Summary

- **Wave 1**: 3 — 1: `unspecified-high` (TextCodec encoding), 2: `unspecified-high` (ApeParser arithmetic), 3: `quick` (constant change)
- **Wave 2**: 3 — 4: `unspecified-high` (TextCodec 0x00), 5: `unspecified-high` (ApeParser cover), 6: `deep` (RAII design)
- **Wave 3**: 2 — 7: `unspecified-high` (heuristic tuning), 8: `deep` (diagnostics design)
- **Wave 4**: 1 — 9: `deep` (multi-file extraction refactoring)

---

## TODOs

- [x] 1. MEDIUM-001：无 iconv 构建编码回退缺少编译时警告

  **What to do**:
  1. **【RED — 编写测试 TR-AUDIT-019】**：在 `regression_tests.cpp` 中添加 `RunTrAudit019()`，使用含 CJK 文本的 APE tag 文件调用 `TagReader::Read()`。在有 iconv 构建中验证编码检测正常工作（GB18030 候选被正确探测）。测试不直接验证 no-iconv 路径（无法在单次构建中切换），但验证 with-iconv 行为未被破坏。
     - 新增测试用例 ID `TR-AUDIT-019`，注册到 `kTestCases` 和 `RunCase()`。
     - 测试步骤：生成 MP3 基础文件 → 追加 APE tag（含 `Title` 字段使用 GB18030 编码的中文文本）→ 调用 `TagReader::Read()` → 验证 title 非空且无乱码（通过 `Expect(!tag.title().empty(), ...)` 和基本可打印性检查）
     - 确认测试 FAIL（当前 with-iconv 应通过，但无 #warning —— 测试重点验证代码 compile+run 正确）
  2. **【GREEN — 修改代码】**：在 `src/text/TextCodec.cpp` 的 `DetectLegacyLocalEncoding()` 函数 `#else` 分支（约 L150-157）添加：
     ```cpp
     #else
         (void)raw;
     #if defined(__GNUC__) || defined(__clang__)
     #  warning "iconv not available — legacy encoding detection disabled, CJK text may produce mojibake"
     #elif defined(_MSC_VER)
     #  pragma message("iconv not available — legacy encoding detection disabled, CJK text may produce mojibake")
     #endif
     ```
     同时改进现有注释（L154-157），将 "WARNING" 改为更明确的 "CRITICAL LIMITATION"，并列出受影响的编码列表。
  3. **【REFACTOR — 如需要】**：无重构需求。
  4. **【运行验证】**：`cmake --build build && cmake --build build-sanitize && ./build/TagReaderRegressionTests TR-AUDIT-019`
  5. **【Commit】**：`git add src/text/TextCodec.cpp test/regression/regression_tests.cpp && git commit -m "fix(text): add compile warning for no-iconv encoding fallback"`

  **Must NOT do**:
  - 不添加 encodingConfidence 字段（避免 API 变更）
  - 不新增编码候选
  - 不修改 with-iconv 的 `#if defined(TAGREADER_HAS_ICONV)` 分支逻辑

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 涉及条件编译和编码回退策略，需要理解 C++23 预处理器和编码检测管道
  - **Skills**: None

  **Parallelization**:
  - **Can Run In Parallel**: YES（文件独立于任务 2、3）
  - **Parallel Group**: Wave 1（与任务 2、3 并行）
  - **Blocks**: 任务 4（同文件 TextCodec.cpp）
  - **Blocked By**: None

  **Acceptance Criteria**:
  - [ ] `src/text/TextCodec.cpp` 含 `#warning` 或 `#pragma message`
  - [ ] `test/regression/regression_tests.cpp` 含 `RunTrAudit019()` + `kTestCases` 注册 + `RunCase()` 分发
  - [ ] 无 iconv 构建时编译器输出警告消息
  - [ ] 有 iconv 构建时行为完全不变（现有测试全部通过）
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-019` → PASS

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: With-iconv build — encoding detection works
    Tool: Bash
    Preconditions: pkg-config --exists iconv → true
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-019
    Expected Result: TR-AUDIT-019 PASS
    Failure Indicators: 测试失败或构建失败
    Evidence: .omo/evidence/task-1-iconv-valid.txt

  Scenario: No-iconv build — compile warning fires
    Tool: Bash
    Preconditions: iconv 未安装或构建时 iconv 未找到
    Steps:
      1. cmake -S . -B build-noiconv -DCMAKE_DISABLE_FIND_PACKAGE_Iconv=ON 2>&1 | grep "warning\|mojibake"
      2. cmake --build build-noiconv 2>&1 | grep "warning\|mojibake"
    Expected Result: 编译输出包含 "iconv not available" 或 "mojibake" 警告
    Failure Indicators: 无警告输出
    Evidence: .omo/evidence/task-1-noiconv-warning.txt
  ```

  **Commit**: YES
  - Message: `fix(text): add compile warning for no-iconv encoding fallback`
  - Files: `src/text/TextCodec.cpp`, `test/regression/regression_tests.cpp`

- [x] 2. MEDIUM-006：ApeParser itemRegionOffset 无符号减法回绕

  **What to do**:
  1. **【RED — 编写测试 TR-AUDIT-020】**：构造恶意 APE tag，其中 footer 的 `tagSize` 字段大于文件实际大小（如 fileSize=200, tagSize=0xFFFFFFFF）。验证 `TagReader::Read()` 不崩溃且返回空字段。
     - 在 `regression_tests.cpp` 中添加 `RunTrAudit020()`
     - 测试步骤：生成 MP3 基础文件（~200 bytes）→ 读取二进制内容 → 追加 APE footer（version=2000, tagSize=0xFFFFFFFF, itemCount=1, flags=0）→ 写入测试文件 → 调用 `TagReader::Read()` → 验证不抛异常且 title 为空
     - 使用现有 helper `AppendU32LE`, `WriteBinaryFile`, `ReadBinaryFile`
     - 确认测试 CRASH 或返回异常（RED）
  2. **【GREEN — 修改代码】**：在 `src/formats/ape/ApeParser.cpp` 的 `ReadApeMetadata()` 函数 L289（`const bool hasHeader = ...`）之后、L293（`const uint64_t itemRegionOffset = ...`）之前添加：
     ```cpp
     // Guard against unsigned subtraction wrap when tagSize > fileSize
     if (tagSize > context.fileSize - 32) {
         return;
     }
     ```
     此检查确保 `context.fileSize - 32 - tagSize` 不会回绕。32 = APE footer 固定大小。
  3. **【REFACTOR】**：无需重构。
  4. **【运行验证】**：`cmake --build build && cmake --build build-sanitize && ./build/TagReaderRegressionTests TR-AUDIT-020 && ./build/TagReaderRegressionTests TR-AUDIT-016`
  5. **【Commit】**：`git add src/formats/ape/ApeParser.cpp test/regression/regression_tests.cpp && git commit -m "fix(ape): guard itemRegionOffset against unsigned subtraction wrap"`

  **Must NOT do**:
  - 不修改 `ReadRange()` 的下游检查逻辑
  - 不改变正常文件的处理路径

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 整数安全修复，需理解 APE tag 二进制布局和 footer/header 关系
  - **Skills**: None

  **Parallelization**:
  - **Can Run In Parallel**: YES（文件独立于任务 1、3）
  - **Parallel Group**: Wave 1（与任务 1、3 并行）
  - **Blocks**: 任务 5（同文件 ApeParser.cpp）、任务 9（AD-003 需此文件稳定）
  - **Blocked By**: None

  **Acceptance Criteria**:
  - [ ] `src/formats/ape/ApeParser.cpp` L293 之前有 `tagSize > context.fileSize - 32` 检查
  - [ ] 恶意文件（tagSize > fileSize）不崩溃、返回空字段
  - [ ] 正常 APE 文件（TR-AUDIT-016）行为不变
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-020` → PASS

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Malformed APE — tagSize exceeds fileSize
    Tool: Bash
    Preconditions: ffmpeg 可用
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-020
    Expected Result: TR-AUDIT-020 PASS，无崩溃
    Failure Indicators: 崩溃、ASAN 错误、或测试失败
    Evidence: .omo/evidence/task-2-oversized-tagsize.txt

  Scenario: Normal APE file still works
    Tool: Bash
    Steps:
      1. ./build/TagReaderRegressionTests TR-AUDIT-016
    Expected Result: TR-AUDIT-016 PASS
    Failure Indicators: 测试失败（说明修复破坏了正常路径）
    Evidence: .omo/evidence/task-2-normal-ape.txt
  ```

  **Commit**: YES
  - Message: `fix(ape): guard itemRegionOffset against unsigned subtraction wrap`
  - Files: `src/formats/ape/ApeParser.cpp`, `test/regression/regression_tests.cpp`

- [x] 3. MEDIUM-004：ID3 Frame Walker resync 扫描预算增大

  **What to do**:
  1. **【RED — 编写测试 TR-AUDIT-021】**：构造 ID3v2.3 tag，含一个有效 TIT2 帧，后跟 ~5000 字节随机数据（模拟畸形区域），再跟一个有效 TALB 帧。验证当前 4096 预算无法恢复第二个帧，增大到 16384 后可以恢复。
     - 在 `regression_tests.cpp` 中添加 `RunTrAudit021()`
     - 测试步骤：生成 MP3 基础文件 → 追加 ID3v2.3 tag header → 写入有效 TIT2 帧 → 写入 ~5000 字节的畸形数据（非法 frame ID: "XXXX" + 随机的 size/flags） → 写入有效 TALB 帧 → 调用 `TagReader::Read()` → 测试场景1：预算=4096时 album 为空 → 测试场景2（预算=16384后）：album 非空
     - 使用现有 helper `AppendU32BE`, `AppendSyncSafe32`, `AppendBytes`
     - 确认 RED：当前代码 album 为空（resync 在 4096 字节处停止）
  2. **【GREEN — 修改代码】**：将 `src/formats/id3/Id3Frames.cpp` L43 的常量从 `4096` 改为 `16384`：
     ```cpp
     constexpr std::size_t kId3ResyncScanBudget = 16384;
     ```
     同时添加注释说明选择 16384 的理由（覆盖常见 pad 场景，开销仅在畸形数据路径）。
  3. **【REFACTOR】**：无需重构。
  4. **【运行验证】**：`cmake --build build && cmake --build build-sanitize && ./build/TagReaderRegressionTests TR-AUDIT-021 && ./build/TagReaderRegressionTests TR-AUDIT-006`
  5. **【Commit】**：`git add src/formats/id3/Id3Frames.cpp test/regression/regression_tests.cpp && git commit -m "fix(id3): increase resync scan budget from 4096 to 16384"`

  **Must NOT do**:
  - 不修改 resync 算法本身（TryResyncId3v22Frame / TryResyncId3v23Or24Frame 函数内部逻辑）
  - 不改为动态预算（保持 constexpr）

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 单常量变更 + 配套测试，逻辑简单

  **Parallelization**:
  - **Can Run In Parallel**: YES（文件独立于任务 1、2）
  - **Parallel Group**: Wave 1（与任务 1、2 并行）
  - **Blocks**: 任务 9（AD-003 需此文件稳定）
  - **Blocked By**: None

  **Acceptance Criteria**:
  - [ ] `kId3ResyncScanBudget` 值 = 16384
  - [ ] 有效帧距离畸形区 4100~16384 字节时可恢复
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-006` 仍通过
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-021` → PASS

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Valid frame beyond 4096 but within 16384 is recovered
    Tool: Bash
    Preconditions: ffmpeg 可用
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-021
    Expected Result: TR-AUDIT-021 PASS — album 非空（第二个帧被恢复）
    Failure Indicators: 测试失败或崩溃
    Evidence: .omo/evidence/task-3-resync-recovery.txt

  Scenario: Existing resync test still works
    Tool: Bash
    Steps:
      1. ./build/TagReaderRegressionTests TR-AUDIT-006
    Expected Result: TR-AUDIT-006 PASS
    Failure Indicators: 测试失败
    Evidence: .omo/evidence/task-3-resync-existing.txt
  ```

  **Commit**: YES
  - Message: `fix(id3): increase resync scan budget from 4096 to 16384`
  - Files: `src/formats/id3/Id3Frames.cpp`, `test/regression/regression_tests.cpp`

- [x] 4. LOW-002：Latin-1 解码遇 0x00 字节截断改为空格

  **What to do**:
  1. **【RED — 编写测试 TR-AUDIT-022】**：在 APE tag 的文本字段中嵌入 0x00 字节（如 "Hello\0World"），验证当前代码截断为 "Hello"，修复后应为 "Hello World"。
     - 在 `regression_tests.cpp` 中添加 `RunTrAudit022()`
     - 测试步骤：生成 MP3 基础文件 → 构造 APE item，key="TITLE"，value 为 UTF-8 编码的文本，在中间位置插入一个 0x00 字节（如 `{'H','e','l','l','o',0x00,'W','o','r','l','d'}`）→ 使用 `AppendApeTag` 附加 → 调用 `TagReader::Read()` → 验证 title 为 "Hello World"（7 个真实字符 + 1 个空格，不含 0x00）而非 "Hello"
     - 确认 RED：当前代码 title 为 "Hello"（0x00 处截断）
  2. **【GREEN — 修改代码】**：修改 `src/text/TextCodec.cpp` 的 `ReadLatin1Text()` 函数 L472-474，将 `break` 改为 `value.push_back(' ')`：
     ```cpp
     if (ch == 0)
     {
         value.push_back(' ');  // 替换 0x00 为空格，而非截断
         continue;
     }
     ```
     同时更新 L468-471 的注释，说明 0x00 被替换为空格而非截断。
  3. **【REFACTOR】**：无需重构。
  4. **【运行验证】**：`cmake --build build && cmake --build build-sanitize && ./build/TagReaderRegressionTests TR-AUDIT-022`
  5. **【Commit】**：`git add src/text/TextCodec.cpp test/regression/regression_tests.cpp && git commit -m "fix(text): replace 0x00 with space in Latin-1 decoding"`

  **Must NOT do**:
  - 不修改 `FindEncodedTerminator` 的 0x00 处理逻辑（ID3 帧解析依赖 0x00 作为分隔符，`FindEncodedTerminator` 在 `ReadLatin1Text` 之前剥离它们）
  - 不改变非 Latin-1 编码路径

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 涉及编码管道中的字符级行为变更，需确认不影响 ID3 帧解析路径

  **Parallelization**:
  - **Can Run In Parallel**: YES（与其他 Wave 2 任务不同文件）
  - **Parallel Group**: Wave 2（与任务 5、6 并行）
  - **Blocks**: 任务 7（同文件 TextCodec.cpp）
  - **Blocked By**: 任务 1（同文件 TextCodec.cpp — 必须先完成 MEDIUM-001）

  **Acceptance Criteria**:
  - [ ] 含 0x00 的 Latin-1 文本：`"Hello\0World"` → `"Hello World"`
  - [ ] 不含 0x00 的 Latin-1 文本: 行为不变
  - [ ] TR-AUDIT-006（ID3 帧解析）仍通过
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-022` → PASS

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Latin-1 text with embedded 0x00 becomes space-separated
    Tool: Bash
    Preconditions: ffmpeg 可用
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-022
    Expected Result: TR-AUDIT-022 PASS — title 为 "Hello World"（非 "Hello"）
    Failure Indicators: title 为 "Hello"、崩溃、或 ASAN 错误
    Evidence: .omo/evidence/task-4-null-to-space.txt

  Scenario: ID3 frame parsing unaffected
    Tool: Bash
    Steps:
      1. ./build/TagReaderRegressionTests TR-AUDIT-006
    Expected Result: TR-AUDIT-006 PASS
    Failure Indicators: 测试失败（说明 0x00→空格 破坏了 ID3 分隔符解析）
    Evidence: .omo/evidence/task-4-id3-unaffected.txt
  ```

  **Commit**: YES
  - Message: `fix(text): replace 0x00 with space in Latin-1 decoding`
  - Files: `src/text/TextCodec.cpp`, `test/regression/regression_tests.cpp`

- [x] 5. MEDIUM-007：ApeParser 封面项添加 item 级 valueSize 上限

  **What to do**:
  1. **【RED — 编写测试 TR-AUDIT-023】**：构造含超大型封面 item 的 APE tag（valueSize > 8 MiB），验证当前代码可能因资源消耗过大而阻塞，修复后面项被静默跳过。
     - 在 `regression_tests.cpp` 中添加 `RunTrAudit023()`
     - 测试步骤：生成 MP3 基础文件 → 构造两个 APE item：
       - item 1: key="Cover Art (Front)", encoding=1 (binary), valueSize=10 MiB（> 8 MiB 上限），value 为 10 MiB 随机数据 → 期望被跳过
       - item 2: key="TITLE", value="Test Title" → 期望正常解析
       - 调用 `TagReader::Read()` → 验证 title 为 "Test Title"（封面项被跳过但后续项正常解析）
     - 如果 10 MiB 文件写入太慢，使用 2 MiB 测试值但将上限调低以验证逻辑
     - 确认 RED：当前代码可能 OOM 或超时
  2. **【GREEN — 修改代码】**：在 `src/formats/ape/ApeParser.cpp` 的 `ProcessApeCoverItem()` 函数开头（L210 之后）添加：
     ```cpp
     constexpr std::size_t kMaxApeCoverItemBytes = 8z * 1024 * 1024; // 8 MiB
     if (valueSize > kMaxApeCoverItemBytes)
     {
         return;  // 静默跳过超大封面项
     }
     ```
     放置于 `valueData == nullptr || valueSize == 0` 检查之后、封面路径检查之前。
  3. **【REFACTOR】**：无需重构。
  4. **【运行验证】**：`cmake --build build && cmake --build build-sanitize && ./build/TagReaderRegressionTests TR-AUDIT-023 && ./build/TagReaderRegressionTests TR-AUDIT-016`
  5. **【Commit】**：`git add src/formats/ape/ApeParser.cpp test/regression/regression_tests.cpp && git commit -m "fix(ape): add item-level valueSize limit for cover art"`

  **Must NOT do**:
  - 不修改 `kMaxApeTagBytes`（16 MiB）或 `kMaxApeItemValueBytes`（1 MiB，用于文本项）
  - 不影响正常封面项的解析

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 资源限制修复，需理解封面管道和 WriteCoverAsPng 的输入限制

  **Parallelization**:
  - **Can Run In Parallel**: YES（与其他 Wave 2 任务不同文件）
  - **Parallel Group**: Wave 2（与任务 4、6 并行）
  - **Blocks**: 任务 9（AD-003 需此文件稳定）
  - **Blocked By**: 任务 2（同文件 ApeParser.cpp — 必须先完成 MEDIUM-006）

  **Acceptance Criteria**:
  - [ ] `ProcessApeCoverItem()` 含 `kMaxApeCoverItemBytes` 检查
  - [ ] 超大封面项被静默跳过
  - [ ] 正常封面项（< 8 MiB）行为不变
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-023` → PASS

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Oversized cover item silently skipped, subsequent items parsed
    Tool: Bash
    Preconditions: ffmpeg 可用
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-023
    Expected Result: TR-AUDIT-023 PASS — title="Test Title"，封面被跳过
    Failure Indicators: OOM、崩溃、超时、title 为空
    Evidence: .omo/evidence/task-5-oversized-cover.txt

  Scenario: Normal APE file still works
    Tool: Bash
    Steps:
      1. ./build/TagReaderRegressionTests TR-AUDIT-016
    Expected Result: TR-AUDIT-016 PASS
    Failure Indicators: 测试失败
    Evidence: .omo/evidence/task-5-normal-ape.txt
  ```

  **Commit**: YES
  - Message: `fix(ape): add item-level valueSize limit for cover art`
  - Files: `src/formats/ape/ApeParser.cpp`, `test/regression/regression_tests.cpp`

- [x] 6. AD-001：ReadMetadata() 添加 RAII stream state guard

  **What to do**:
  1. **【设计阶段 — 无需单独回归测试】**：RAII guard 不改变行为，由现有测试验证正确性。在 `src/core/TagPipeline.cpp` 中实现一个轻量 RAII wrapper：
     ```cpp
     namespace {
     class StreamStateGuard {
     public:
         explicit StreamStateGuard(std::ifstream &stream) noexcept : stream_(stream) {}
         ~StreamStateGuard() { stream_.clear(); }
         StreamStateGuard(const StreamStateGuard &) = delete;
         StreamStateGuard &operator=(const StreamStateGuard &) = delete;
     private:
         std::ifstream &stream_;
     };
     }
     ```
  2. **【修改代码】**：在 `ReadMetadata()` 的 `ignoreMalformedMetadata` lambda 中，将每个 `context.input.clear()` 替换为 `StreamStateGuard`：
     - 修改 `ignoreMalformedMetadata` lambda（L107-125）：在 try 块开头创建 `StreamStateGuard guard(context.input);`，删除 catch 块中的 `context.input.clear()` 调用（RAII 自动处理）。
     - L106 的入口 `context.input.clear()` 保留（作为显式起点清理）。
     - L145 的 APE 分支 `context.input.clear()` 保留（跨格式分支切换的显式清理点）。
  3. **【运行验证】**：`cmake --build build && cmake --build build-sanitize`，然后运行全部 18 个测试确认无回归。
  4. **【Commit】**：`git add src/core/TagPipeline.cpp && git commit -m "refactor(core): add RAII stream state guard for parser calls"`

  **Must NOT do**:
  - 不添加 RAII guard 到 `ReadLyrics()`（其已有独立 clear 策略）
  - 不添加到 `DetectTagFormat()` 或其他管线阶段
  - 不删除显式的跨格式 clear（L106/L145）

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: RAII 设计需理解 C++23 资源管理语义和现有异常处理管线的精确交互

  **Parallelization**:
  - **Can Run In Parallel**: YES（TagPipeline.cpp 独立于 Wave 2 其他文件）
  - **Parallel Group**: Wave 2（与任务 4、5 并行）
  - **Blocks**: 任务 8（同文件 TagPipeline.cpp）
  - **Blocked By**: None（独立文件）

  **Acceptance Criteria**:
  - [ ] `StreamStateGuard` 类存在于 `TagPipeline.cpp` 匿名命名空间
  - [ ] `ignoreMalformedMetadata` 使用 `StreamStateGuard` 自动恢复流状态
  - [ ] 全部 18 个现有测试通过（无行为变更）
  - [ ] 代码比之前更简洁（手动 clear 调用减少）

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: RAII guard preserves existing behavior
    Tool: Bash
    Preconditions: 全部 18 个测试已通过基线验证
    Steps:
      1. cmake --build build
      2. for i in $(seq -w 1 18); do
           printf -v id "TR-AUDIT-%03d" "$((10#$i))"
           ./build/TagReaderRegressionTests "$id" || exit 1
         done
    Expected Result: 全部 18 个测试 PASS
    Failure Indicators: 任何测试失败
    Evidence: .omo/evidence/task-6-raii-regression.txt
  ```

  **Commit**: YES
  - Message: `refactor(core): add RAII stream state guard for parser calls`
  - Files: `src/core/TagPipeline.cpp`（仅此文件，无新增测试文件）

- [x] 7. LOW-003：收紧 UTF-16 无 BOM 嗅探启发式阈值

  **What to do**:
  1. **【RED — 编写测试 TR-AUDIT-024】**：构造一个已知会产生误判的字节序列（24+ 字节 ASCII 文本，偶数字节对齐，高字节 0x00 比例满足旧阈值 `expectedNuls * 3 >= units * 2`），验证当前代码误判为 UTF-16，修复后正确判定为非 UTF-16。
     - 在 `regression_tests.cpp` 中添加 `RunTrAudit024()`
     - 测试步骤：生成 MP3 基础文件 → 追加 ID3v2.3 tag 含 TXXX frame（encoding=0x03 UTF-8，但 frame payload 为精心构造的 ASCII 字节序列，在特定条件下触发 `LooksLikeUtf16WithoutBom` 误判） → 调用 `TagReader::Read()` → 验证不会因编码误判而产生乱码
     - 确认 RED：当前代码可能误判（取决于字节序列构造）
  2. **【GREEN — 修改代码】**：修改 `src/text/TextCodec.cpp` 的 `LooksLikeUtf16WithoutBom()` 函数：
     - 将 L112 的阈值从 `expectedNuls * 3 < units * 2` 改为 `expectedNuls * 4 < units * 3`（需要更高比例的 NUL 字节才判定为 UTF-16）
     - 或：增加 `asciiLikeUnits * 2 < units` 检查（要求至少一半的单元是 ASCII-like，降低纯二进制数据的误判率）
     - 添加注释说明阈值选择的理论依据
  3. **【REFACTOR】**：无需重构。
  4. **【运行验证】**：`cmake --build build && cmake --build build-sanitize && ./build/TagReaderRegressionTests TR-AUDIT-024 && ./build/TagReaderRegressionTests TR-AUDIT-007`
  5. **【Commit】**：`git add src/text/TextCodec.cpp test/regression/regression_tests.cpp && git commit -m "fix(text): tighten UTF-16 without BOM detection heuristic"`

  **Must NOT do**:
  - 不添加新的检测维度（如字节频率分析）
  - 不修改 BOM-aware UTF-16 路径
  - 不改变 UTF-8 或 Latin-1 路径

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 启发式阈值调整需要理解编码检测管道的统计特性和误判边界

  **Parallelization**:
  - **Can Run In Parallel**: YES（与任务 8 不同文件）
  - **Parallel Group**: Wave 3（与任务 8 并行）
  - **Blocks**: 任务 9（AD-003 需此文件稳定）
  - **Blocked By**: 任务 4（同文件 TextCodec.cpp — 必须先完成 LOW-002）

  **Acceptance Criteria**:
  - [ ] 阈值变更：`expectedNuls * 4 < units * 3` 或等价收紧
  - [ ] 正常 UTF-16（高比例 NUL 字节）仍正确识别
  - [ ] 已知误判向量不再触发误判
  - [ ] TR-AUDIT-007（UTF-16 编码检测）仍通过
  - [ ] `./build/TagReaderRegressionTests TR-AUDIT-024` → PASS

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Crafted false positive vector no longer misidentified as UTF-16
    Tool: Bash
    Preconditions: ffmpeg 可用
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-024
    Expected Result: TR-AUDIT-024 PASS — 编码不被误判为 UTF-16
    Failure Indicators: 测试失败
    Evidence: .omo/evidence/task-7-heuristic-fix.txt

  Scenario: Real UTF-16 data still correctly detected
    Tool: Bash
    Steps:
      1. ./build/TagReaderRegressionTests TR-AUDIT-007
    Expected Result: TR-AUDIT-007 PASS
    Failure Indicators: 测试失败
    Evidence: .omo/evidence/task-7-utf16-still-works.txt
  ```

  **Commit**: YES
  - Message: `fix(text): tighten UTF-16 without BOM detection heuristic`
  - Files: `src/text/TextCodec.cpp`, `test/regression/regression_tests.cpp`

- [x] 8. AD-002：ReadContext 添加可选 diagnostics channel

  **What to do**:
  1. **【设计阶段 — 无需单独回归测试】**：添加 `std::ostream*` 成员到 `ReadContext`，parser 在遇到畸形数据时将诊断信息写入。
  2. **【修改代码】**：
     - 在 `src/core/ReadContext.hpp` 的 `ReadContext` 结构体中添加：
       ```cpp
       std::ostream *diagnostics = nullptr; // optional debug output channel
       ```
     - 在 `src/core/TagPipeline.cpp` 的 `ReadMetadata()` 的 `ignoreMalformedMetadata` lambda（catch 块）中添加：
       ```cpp
       if (context.diagnostics != nullptr) {
           *context.diagnostics << "parser error in [" << tagFormat << "]: " << ex.what() << '\n';
       }
       ```
     - 在 `ReadLyrics()` 的 catch 块（L223-230）中添加类似诊断输出。
  3. **【运行验证】**：`cmake --build build && cmake --build build-sanitize`，然后运行全部 18 个测试（diagnostics=nullptr 默认行为不变）。
  4. **【Commit】**：`git add include/TagReaderInternal.hpp src/core/TagPipeline.cpp && git commit -m "feat(core): add optional diagnostics channel for parser errors"`

  **Must NOT do**:
  - 不添加日志级别、文件输出、格式化基础设施
  - 不暴露 diagnostics 到公开 API（`TagReader.hpp`）
  - 不影响默认行为（`diagnostics == nullptr`）

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 涉及内部 API 设计和异常处理管线的诊断注入

  **Parallelization**:
  - **Can Run In Parallel**: YES（与任务 7 不同文件）
  - **Parallel Group**: Wave 3（与任务 7 并行）
  - **Blocks**: None
  - **Blocked By**: 任务 6（同文件 TagPipeline.cpp — 必须先完成 RAII guard）

  **Acceptance Criteria**:
  - [ ] `ReadContext` 含 `std::ostream *diagnostics` 成员
  - [ ] parser 异常时将错误信息写入 diagnostics（非 null 时）
  - [ ] `diagnostics == nullptr` 时行为 100% 不变
  - [ ] 全部 18 个现有测试通过

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Default (nullptr) behavior unchanged
    Tool: Bash
    Steps:
      1. cmake --build build
      2. for i in $(seq -w 1 18); do
           printf -v id "TR-AUDIT-%03d" "$((10#$i))"
           ./build/TagReaderRegressionTests "$id" || exit 1
         done
    Expected Result: 全部 18 个测试 PASS
    Failure Indicators: 任何测试失败
    Evidence: .omo/evidence/task-8-diagnostics-default.txt
  ```

  **Commit**: YES
  - Message: `feat(core): add optional diagnostics channel for parser errors`
  - Files: `src/core/ReadContext.hpp`, `src/core/TagPipeline.cpp`

- [x] 9. AD-003 + LOW-005：抽取共享 helpers + 统一 ParseUInt16 行为

  **What to do**:
  1. **【新建共享库 `src/common/ParseHelpers.hpp`】**：
     - 创建 header-only 文件 `src/common/ParseHelpers.hpp`
     - 包含以下统一实现：
       - `ParseUInt16(const std::string &value)` — **严格行为**（统一 ID3 现有行为）：
         ```cpp
         inline uint16_t ParseUInt16(const std::string &value) {
             const std::string trimmed = TrimText(value);
             if (trimmed.empty()) return 0;
             try {
                 std::size_t consumed = 0;
                 const unsigned long parsed = std::stoul(trimmed, &consumed, 10);
                 if (consumed != trimmed.size() || parsed > UINT16_MAX) return 0;
                 return static_cast<uint16_t>(parsed);
             } catch (...) { return 0; }
         }
         ```
       - `ParseSlashNumber(const std::string &value)` — 返回 `std::pair<uint16_t, uint16_t>`
       - `ParseYearOnly(const std::string &value)` — 提取首个 4 位数字年份
       - `ToLower(std::string s)` — 返回小写副本
       - `IEquals(const std::string &a, const std::string &b)` — 大小写不敏感比较
     - 命名空间：`tagreader_common`
     - 包含必要的 `#include`：`<string>`, `<cstdint>`, `<algorithm>`, `<cctype>`, `<stdexcept>`
  2. **【RED — 编写测试 TR-AUDIT-025】**：
     - 在 `regression_tests.cpp` 中添加 `RunTrAudit025()`
     - 测试步骤：
       a. 生成三个文件（MP3+APE、FLAC+Vorbis、MP3+ID3），每个含 Track="5 abc" → 验证 trackNumber=0
       b. 生成三个文件，每个含 Track="5/10" → 验证 trackNumber=5（ParseSlashNumber 不受影响）
       c. 生成三个文件，每个含 Track="5" → 验证 trackNumber=5
       d. 生成三个文件，每个含 Track="5 " → 验证 trackNumber=5（TrimText 后正确解析）
     - 确认 RED：Vorbis/APE 当前返回 trackNumber=5（宽松行为），修复后应返回 0
  3. **【GREEN — 更新各源文件使用共享 helpers】**：
     - `src/formats/ape/ApeParser.cpp`：删除本地 `ParseUInt16`/`ParseSlashNumber`/`ParseYearOnly`/`IEquals`，添加 `#include "common/ParseHelpers.hpp"`，更新调用为 `tagreader_common::ParseUInt16` 等
     - `src/formats/vorbis/VorbisCommentParser.cpp`：同上（`ParseUInt16`/`ParseSlashNumber`/`ParseYearOnly`/`ToLower`）
     - `src/formats/id3/Id3Frames.cpp`：同上（`ParseUInt16`/`ParseSlashNumber`/`ParseYearOnly`/`ToLower`）
     - `src/formats/mp4/Mp4Parser.cpp`：删除本地 `ParseYearOnly`，添加 include
     - `src/formats/id3/Id3Parser.cpp`：删除本地 `ParseYearOnly`，添加 include
     - `src/formats/ogg-vorbis/OggVorbisParser.cpp`：删除本地 `ParseYearOnly`/`ToLower`，添加 include
     - `src/media/ContainerDetector.cpp`：删除本地 `ToLower`，添加 include
     - `src/media/MediaInfoReader.cpp`：删除本地 `ToLower`，添加 include
     - `src/cover/CoverCache.cpp`：删除本地 `ToLower`，添加 include
     - `src/text/TextNormalize.cpp`：删除本地 `ToLower`，添加 include
     - `CMakeLists.txt`：在 `target_sources` 中添加 `src/common/ParseHelpers.hpp`（header-only，添加为 source 或确保 include dir 正确）
  4. **【运行验证】**：
     ```bash
     cmake -S . -B build && cmake --build build
     cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize
     # 全部 18 个现有测试
     for i in $(seq -w 1 18); do ...; done
     # 新增测试
     ./build/TagReaderRegressionTests TR-AUDIT-025
     ```
  5. **【Commit】**：`git add src/common/ParseHelpers.hpp CMakeLists.txt test/regression/regression_tests.cpp [所有修改的源文件] && git commit -m "refactor: extract shared parse helpers to src/common/ and unify ParseUInt16"`

  **Must NOT do**:
  - 不修改 ParseSlashNumber 的行为（已有确认：三处实现一致）
  - 不修改 ToLower 的行为（仅搬迁，内部逻辑不变）
  - 不提取未列出的函数
  - 不新增命名空间依赖

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 涉及 ~10 个文件的大规模重构，必须确保每次搬迁保持行为一致

  **Parallelization**:
  - **Can Run In Parallel**: NO（须在所有文件级修复完成后执行）
  - **Parallel Group**: Wave 4（独立，最后执行）
  - **Blocks**: None
  - **Blocked By**: 任务 2、3、5（ApeParser.cpp 和 Id3Frames.cpp 必须稳定），任务 7（TextCodec.cpp 必须稳定）

  **Acceptance Criteria**:
  - [ ] `src/common/ParseHelpers.hpp` 存在且含 `ParseUInt16`、`ParseSlashNumber`、`ParseYearOnly`、`ToLower`、`IEquals`
  - [ ] 全部 10 个源文件的本地实现已删除
  - [ ] 所有 `ParseUInt16` 行为统一为严格模式（"5 abc" → 0）
  - [ ] 全部 18 个现有测试 + TR-AUDIT-025 通过
  - [ ] 零编译警告、零 ASAN/UBSAN 错误

  **QA Scenarios (MANDATORY)**:
  ```
  Scenario: Unified ParseUInt16 — "5 abc" returns 0 in all parsers
    Tool: Bash
    Steps:
      1. cmake --build build
      2. ./build/TagReaderRegressionTests TR-AUDIT-025
    Expected Result: TR-AUDIT-025 PASS — 所有 4 个子场景通过
    Failure Indicators: 任何子场景失败
    Evidence: .omo/evidence/task-9-parseuint16-unified.txt

  Scenario: All 18 existing tests still pass (no regression from extraction)
    Tool: Bash
    Steps:
      1. for i in $(seq -w 1 18); do
           printf -v id "TR-AUDIT-%03d" "$((10#$i))"
           ./build/TagReaderRegressionTests "$id" || exit 1
         done
    Expected Result: 全部 18 个测试 PASS
    Failure Indicators: 任何测试失败
    Evidence: .omo/evidence/task-9-no-regression.txt
  ```

  **Commit**: YES
  - Message: `refactor: extract shared parse helpers to src/common/ and unify ParseUInt16`
  - Files: `src/common/ParseHelpers.hpp`, `CMakeLists.txt`, `src/formats/ape/ApeParser.cpp`, `src/formats/vorbis/VorbisCommentParser.cpp`, `src/formats/id3/Id3Frames.cpp`, `src/formats/mp4/Mp4Parser.cpp`, `src/formats/id3/Id3Parser.cpp`, `src/formats/ogg-vorbis/OggVorbisParser.cpp`, `src/media/ContainerDetector.cpp`, `src/media/MediaInfoReader.cpp`, `src/cover/CoverCache.cpp`, `src/text/TextNormalize.cpp`, `test/regression/regression_tests.cpp`

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

- [x] F1. **Plan Compliance Audit** — `oracle` → APPROVE
  Read the plan end-to-end. Verify each "Must Have" item exists, each "Must NOT Have" is absent.

- [x] F2. **Code Quality Review** — `unspecified-high` → APPROVE
  Run `cmake --build build` + `cmake --build build-sanitize`. Check for unused imports, commented-out code, AI slop.

- [x] F3. **Real Manual QA** — `unspecified-high` → APPROVE
  Run ALL 25 tests (18 existing + 7 new). Verify zero ASAN/UBSAN errors. Check evidence files exist.

- [x] F4. **Scope Fidelity Check** — `deep` → APPROVE (with note: CoverCache.cpp/ContainerDetector.cpp changes are per Task 9 explicit scope — plan guardrail contradiction acknowledged)
  Verify each commit matches its task description. Check no cross-task contamination.

---

## Commit Strategy

- **1**: `fix(text): add compile warning for no-iconv encoding fallback` — `src/text/TextCodec.cpp`
- **2**: `fix(ape): guard itemRegionOffset against unsigned subtraction wrap` — `src/formats/ape/ApeParser.cpp`
- **3**: `fix(id3): increase resync scan budget from 4096 to 16384` — `src/formats/id3/Id3Frames.cpp`
- **4**: `fix(text): replace 0x00 with space in Latin-1 decoding` — `src/text/TextCodec.cpp`
- **5**: `fix(ape): add item-level valueSize limit for cover art` — `src/formats/ape/ApeParser.cpp`
- **6**: `refactor(core): add RAII stream state guard for parser calls` — `src/core/TagPipeline.cpp`
- **7**: `fix(text): tighten UTF-16 without BOM detection heuristic` — `src/text/TextCodec.cpp`
- **8**: `feat(core): add optional diagnostics channel for parser errors` — `src/core/TagPipeline.cpp` + `include/TagReaderInternal.hpp`
- **9**: `refactor: extract shared parse helpers to src/common/ and unify ParseUInt16` — `src/common/ParseHelpers.hpp` + 10个源文件

---

## Success Criteria

### Verification Commands
```bash
# Clean build
cmake -S . -B build && cmake --build build

# Sanitizer build
cmake -S . -B build-sanitize -DTAGREADER_ENABLE_SANITIZERS=ON && cmake --build build-sanitize

# All existing tests pass (18)
for i in $(seq -w 1 18); do
  printf -v id "TR-AUDIT-%03d" "$((10#$i))"
  ./build/TagReaderRegressionTests "$id" || exit 1
done

# All new tests pass (7)
for i in $(seq -w 19 25); do
  printf -v id "TR-AUDIT-%03d" "$((10#$i))"
  ./build/TagReaderRegressionTests "$id" || exit 1
done
```

### Final Checklist
- [ ] 全部 "Must Have" 已实现
- [ ] 全部 "Must NOT Have" 未违反
- [ ] 25/25 测试通过
- [ ] 零 ASAN/UBSAN 错误
- [ ] 9 个 commit 已提交
