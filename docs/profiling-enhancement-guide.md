# Tracy Profiling 增强指南

**日期**: 2026-07-05  
**目的**: 捕获之前未被追踪的 82% 执行时间

---

## 背景

根据 PNG 优化方法 4 的调研，之前的 Tracy profiling 数据显示：

| 操作 | 耗时 | 占比 |
|------|------|------|
| EncodePngWithOptions | 139.01 ms | 15.74% |
| 其他可见操作 | ~23 ms | ~2.6% |
| **未捕获时间** | **~720 ms** | **81.7%** ⚠️ |

**问题**: 82% 的执行时间未被 Tracy 捕获，导致无法确定真正的性能瓶颈。

**推断**: 未捕获时间可能包括：
- FFmpeg 解码内部: 40-50%
- 文件 I/O: 20-30%
- SQLite 写入: 10-15%

---

## 新增的 Profiling 标记

### 1. FFmpeg 操作（FfmpegSession.cpp）

#### avformat_open_input
```cpp
{
    TAGREADER_PROFILE_SCOPE_COLOR("avformat_open_input", TAGREADER_COLOR_FFMPEG);
    openResult = avformat_open_input(&formatContext, nullptr, nullptr, nullptr);
}
```

**作用**: 追踪 FFmpeg 容器打开时间  
**预期**: 可能占 5-10% 总时间

#### avformat_find_stream_info
```cpp
{
    TAGREADER_PROFILE_SCOPE_COLOR("avformat_find_stream_info", TAGREADER_COLOR_FFMPEG);
    infoResult = avformat_find_stream_info(formatContext, nullptr);
}
```

**作用**: 追踪流信息分析时间  
**预期**: 可能占 10-20% 总时间（需要解码部分帧）

---

### 2. 文件 I/O 操作（CoverCache.cpp）

#### AtomicWriteFileIfAbsent（总体）
```cpp
TAGREADER_PROFILE_SCOPE_COLOR("AtomicWriteFileIfAbsent", TAGREADER_COLOR_IO);
TAGREADER_PROFILE_VALUE(size);
```

**作用**: 追踪完整的封面文件写入操作  
**预期**: 可能占 10-20% 总时间

#### WriteAll（数据写入）
```cpp
{
    TAGREADER_PROFILE_SCOPE_COLOR("WriteAll", TAGREADER_COLOR_IO);
    WriteAll(fd.get(), data, size, tempPath);
}
```

**作用**: 追踪实际的数据写入时间  
**预期**: 可能占 5-10% 总时间

#### fsync（磁盘同步）
```cpp
{
    TAGREADER_PROFILE_SCOPE_COLOR("fsync", TAGREADER_COLOR_IO);
    if (::fsync(fd.get()) != 0) { ... }
}
```

**作用**: 追踪磁盘同步时间（强制刷新到物理磁盘）  
**预期**: 可能占 3-8% 总时间（取决于磁盘速度）

#### PublishFileIfAbsent（文件发布）
```cpp
{
    TAGREADER_PROFILE_SCOPE_COLOR("PublishFileIfAbsent", TAGREADER_COLOR_IO);
    publishResult = PublishFileIfAbsent(tempPath, finalPath);
}
```

**作用**: 追踪文件 link/rename 和目录 fsync 时间  
**预期**: 可能占 2-5% 总时间

---

## 如何使用新的 Profiling

### 步骤 1: 重新编译

```bash
cd /home/kaizen857/cppProject\(app_and_lib\)/TagReader
cmake --build build -j$(nproc)

cd /home/kaizen857/cppProject\(app_and_lib\)/Seriona_Backend
cmake --build build -j$(nproc)
```

### 步骤 2: 运行 Tracy profiling

```bash
# 启动 Tracy server（在另一个终端）
tracy-capture -o trace.tracy

# 运行 Seriona 冷扫描
cd /home/kaizen857/cppProject\(app_and_lib\)/Seriona_Backend
./build/seriona /home/kaizen857/Music
```

### 步骤 3: 分析新的 Tracy 数据

在 Tracy GUI 中查看：

1. **总体时间分布**:
   - 查看 Statistics 面板
   - 按 Total Time 排序
   - 确认新增的 profiling 标记是否出现

2. **关键指标**:
   - `avformat_open_input`: FFmpeg 容器打开
   - `avformat_find_stream_info`: 流信息分析
   - `image avcodec_receive_frame`: 图像解码（已有）
   - `AtomicWriteFileIfAbsent`: 文件写入总体
   - `WriteAll`: 数据写入
   - `fsync`: 磁盘同步
   - `PublishFileIfAbsent`: 文件发布
   - `EncodePngWithOptions`: PNG 编码（已有）

3. **预期结果**:
   - 可见时间从 ~18% 增加到 **80-90%**
   - 能够清楚看到真正的热点在哪里

---

## 预期的分析结果

### 场景 A: FFmpeg 解码是瓶颈（可能性 70%）

**Tracy 数据会显示**:
- `avformat_find_stream_info`: 占 15-25% 总时间
- `image avcodec_receive_frame`: 占 20-30% 总时间
- **总计**: FFmpeg 解码占 35-55% 总时间

**优化方向**:
1. 用 libjpeg-turbo 直接解码 JPEG cover（绕过 FFmpeg）
2. 用 libpng 直接解码 PNG cover（绕过 FFmpeg）
3. 优化 FFmpeg decoder 参数

**预期改善**: 20-40% 端到端（2-4 秒）

---

### 场景 B: 文件 I/O 是瓶颈（可能性 60%）

**Tracy 数据会显示**:
- `WriteAll`: 占 8-15% 总时间
- `fsync`: 占 5-12% 总时间
- `PublishFileIfAbsent`: 占 3-6% 总时间
- `ReadRange`（已有）: 占 5-10% 总时间
- **总计**: I/O 占 21-43% 总时间

**优化方向**:
1. 异步 I/O（io_uring）预读取文件
2. 批量 SQLite 写入（减少 fsync 次数）
3. mmap 替代 read/write
4. 考虑减少 fsync（对缓存文件可接受）

**预期改善**: 10-25% 端到端（1-3 秒）

---

### 场景 C: PNG 编码确实是主要瓶颈（可能性 30%）

**Tracy 数据会显示**:
- `EncodePngWithOptions`: 仍然占 40-50% 总时间
- FFmpeg + I/O 合计 < 30% 总时间

**优化方向**:
1. 使用 fpng（10-20x 加速）
2. 考虑 zlib-ng（1.2-1.5x 加速）

**预期改善**: 13-15% 端到端（1.5-1.8 秒）

---

## 下一步行动

### 立即（运行 profiling）

1. 重新编译 TagReader 和 Seriona（✅ 已完成）
2. 运行 Tracy profiling 收集新数据
3. 分析新的 Tracy 数据

### 短期（根据数据决策）

**若场景 A 成立**（FFmpeg 是瓶颈）:
- 实施 libjpeg-turbo + libpng 直接解码
- 预期改善: 20-40%

**若场景 B 成立**（I/O 是瓶颈）:
- 实施异步 I/O + 批量 SQLite
- 预期改善: 10-25%

**若场景 C 成立**（PNG 是瓶颈）:
- 实施 fpng
- 预期改善: 13-15%

### 长期（组合优化）

- 同时优化 FFmpeg、I/O、PNG
- 预期改善: 45-73% 端到端（5-8 秒）

---

## 参考文档

- **完整调研报告**: `png-optimization-method-4-research-report.md`
- **决策摘要**: `png-optimization-method-4-decision.md`（Seriona_Backend）
- **提交记录**: `6a4a96f` - perf: add detailed Tracy profiling

---

**下一步**: 运行 Tracy profiling，收集新数据，确认真正的性能瓶颈！
