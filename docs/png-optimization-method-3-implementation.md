# PNG 编码优化方法 3 实施报告

## 优化内容

**优化方法 3**：移除每封面 `std::async`，改成顺序编码

## 实施日期

2026-07-05

## 问题分析

### 原始实现

```cpp
std::future<bool> fullFuture = std::async(std::launch::async, [&]() {
    // 编码 full-size PNG
    PngEncodeOptions encOpts;
    std::vector<uint8_t> png = EncodePngWithOptions(decoded, encOpts);
    return AtomicWriteFileIfAbsent(fullPath, png.data(), png.size());
});

std::future<bool> thumbFuture = std::async(std::launch::async, [&]() {
    // 编码 thumbnail PNG
    PngEncodeOptions encOpts;
    encOpts.compressionLevel = static_cast<int>(options.pngCompression);
    std::vector<uint8_t> png = EncodePngWithOptions(thumbnail, encOpts);
    return AtomicWriteFileIfAbsent(thumbPath, png.data(), png.size());
});

fullFuture.get();
thumbFuture.get();
```

### 并发问题

- **Seriona scanner**: 32-way worker pool（基于 `std::thread::hardware_concurrency()`）
- **每个封面**: 2 个 `std::async` 任务（full + thumbnail）
- **结果**: 64+ 线程同时竞争 32 逻辑核
- **问题**:
  - Oversubscription（线程数超过 CPU 核数）
  - Context switch 开销
  - Cache thrashing（缓存颠簸）
  - 调度抖动

### Oracle 调研结论

- PNG 编码是 **CPU-bound**，不是 I/O-bound
- Full-size 编码耗时 560ms per 2400x2400 cover
- Thumbnail 编码很快（<50ms）
- **推荐方案**: 顺序编码（sequential encoding）
- **预期改善**: 10-20% wall time，CPU 利用率从 80-90% 提升到 95-100%

## 实施方案

### 新实现

```cpp
// 顺序编码：先 full-size，后 thumbnail
// 避免嵌套并发（scanner 已有 32-way worker pool）
bool fullSuccess = false;
if (!std::filesystem::exists(fullPath))
{
    PngEncodeOptions encOpts;
    std::vector<uint8_t> png = EncodePngWithOptions(decoded, encOpts);
    if (!png.empty())
    {
        fullSuccess = AtomicWriteFileIfAbsent(fullPath, png.data(), png.size());
    }
}
else
{
    fullSuccess = true;
}

bool thumbSuccess = true;
if (options.generateThumbnail && thumbnail.frame != nullptr)
{
    if (!std::filesystem::exists(thumbPath))
    {
        PngEncodeOptions encOpts;
        encOpts.compressionLevel = static_cast<int>(options.pngCompression);
        std::vector<uint8_t> png = EncodePngWithOptions(thumbnail, encOpts);
        if (!png.empty())
        {
            thumbSuccess = AtomicWriteFileIfAbsent(thumbPath, png.data(), png.size());
        }
        else
        {
            thumbSuccess = false;
        }
    }
}
```

### 代码变更

**TagReader 仓库** (commit: `ab7573a`)

- **文件**: `src/cover/CoverCache.cpp`
- **删除**: `std::async` 调用，`<future>` 头文件
- **添加**: 顺序编码逻辑，架构决策注释

## 验证

### 功能正确性

✅ 所有封面相关测试通过（11/11）:

```bash
cd TagReader && ctest --test-dir build/profile -R "cover" --output-on-failure
# 100% tests passed, 0 tests failed out of 11
```

### 预期性能改善

根据 Oracle 分析：

| 指标 | 优化前 | 优化后 | 改善 |
|---|---|---|---|
| Peak threads | 64+ | 32-34 | -50% |
| CPU utilization | 80-90% | 95-100% | +10-15% |
| Wall time | X 秒 | 0.8-0.9X 秒 | -10-20% |
| Context switches/sec | 数千 | 数百 | -80% |

## 实际验证建议

### 真实音乐库测试

用户音乐库规模：**4910 个文件，168GB**

**测试方法**:

1. **冷扫描测试**（删除缓存后扫描）
   ```bash
   # 删除旧缓存
   rm -rf ~/.local/share/seriona/cache/*.sqlite
   rm -rf ~/.local/share/seriona/covers/*
   
   # 运行 seriona 扫描
   time seriona /home/kaizen857/Music/
   ```

2. **记录指标**
   - 总扫描时间（wall time）
   - 峰值线程数：`ps -eLf | grep seriona | wc -l`
   - CPU 利用率：`top` 或 `htop`（多核累加可能 >100%）
   - Context switches：`pidstat -w -p <pid> 1`

3. **对比基准**
   - 如需对比优化前性能，需要恢复到 commit `ab7573a` 之前
   - 建议：记录当前性能作为新基准，未来优化时对比

### 观察要点

- ✅ **线程数应该稳定在 32-34**（32 worker + 主线程 + 少量辅助）
- ✅ **CPU 利用率应接近 100%**（假设系统为 32 逻辑核 = 3200%）
- ✅ **扫描过程流畅，无明显卡顿**
- ✅ **所有封面正确生成**（full + thumbnail）

## 技术细节

### 为什么顺序编码优于并发？

1. **PNG 编码是单线程 CPU 密集计算**
   - libpng/zlib 默认实现不支持内部并行
   - 每个编码任务独占一个 CPU 核

2. **Scanner worker pool 已饱和 CPU**
   - 32 worker 同时跑 PNG 编码时，CPU 利用率接近 100%
   - 再加 async 并发只会增加调度开销，无法提升吞吐

3. **Thumbnail 编码很快**
   - 顺序执行 thumbnail 的边际成本 <50ms
   - 不值得为它开新线程（线程创建/销毁成本 ~1-5μs，但调度争抢代价更高）

### 何时需要有界 pool？

**当前不需要**，因为：
- PNG 编码是 CPU-bound
- Worker pool 已经最优利用 CPU

**需要 pool 的场景**:
- I/O 瓶颈显现（磁盘 `%util` > 80%，CPU 利用率 < 70%）
- 编码内部可并行（如分块 SIMD，但当前 libpng 不支持）

## 提交记录

**TagReader**:
```
ab7573a perf(cover): remove std::async to avoid nested concurrency
```

**关键文件**:
- `src/cover/CoverCache.cpp`: 移除 `std::async`，改为顺序编码
- 功能测试：✅ 通过
- 性能预测：10-20% 改善（基于 Oracle 分析）

## 下一步

1. **用户验证**: 在真实音乐库上运行冷扫描，记录性能指标
2. **监控**: 观察实际使用中的 CPU 利用率和扫描速度
3. **可选**: 如需精确对比，恢复到优化前代码并运行相同测试

## 相关文档

- 优化方法文档：`docs/cover-png-optimization-methods.md`
- Oracle 调研：Session `ses_0d0518fa6ffe0SL36OXgXPkrHJ`
- 性能分析：优化方法 1 已完成（移除中间 PNG 往返）
- 性能分析：优化方法 2 已完成（PNG encoder 参数优化）
