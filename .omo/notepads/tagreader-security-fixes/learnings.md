- P1：FLAC `ReadFlacMetadataBlocks()` 中 Vorbis comment 块的局部读/解析失败可以跳过当前块继续扫描后续块；PICTURE 分支不能放进同一个吞错路径，否则会吞掉 `cover cache` / `cover export` 失败。

- P2：`NormalizeLyrics()` 的 timed lyrics 去重应避免同 timestamp 组内逐项 `any_of`；先按 `(timestamp, text)` 排序再 `std::unique` 可保留 cap 后的确定性输出，并把大量同时间戳歌词从平方复杂度降为排序主导。

- S1：默认封面目录应与显式 `coverExportDir` 分开校验；单参数 `Read()` 优先 `XDG_RUNTIME_DIR/tagreader-covers`，回退到带 UID 的临时目录，并只对默认根目录拒绝 symlink/校验 owner/收紧权限，显式 symlink 目录策略保持到 S2 处理。

- S2：显式 `coverExportDir` 现在与默认目录一样先用 `symlink_status()` 检查目录本身；已有目录 symlink 在 `create_directories()`、探针和封面缓存写入前拒绝，正常显式目录仍走原探针写/读/删校验。

- S3：输入路径验证后必须立即打开一次并以该 fd 为唯一数据源；`fileSize`/`lastModified` 来自 `fstat()`，raw parser 走 `pread()`，FFmpeg 走同一 fd 支撑的自定义 `AVIOContext`，否则验证后替换路径会让 FFmpeg 和 raw parser 看到不同文件。

- S3 follow-up：custom `AVIOContext` 不是 `avformat_close_input()` 自动拥有的资源；`avformat_open_input()` 失败、`avformat_find_stream_info()` 失败和正常 `FormatContextDeleter` 路径都必须显式释放同一个保存下来的 `AVIOContext`，避免 opaque state/buffer 泄漏。

- S3 follow-up：`avformat_alloc_context()` 到 `ReadContext::formatContext.reset()` 之间要用本地 RAII guard 保护裸 `AVFormatContext*`；guard 只释放 format context 本体，custom `AVIOContext` 仍走各失败路径和 `FormatContextDeleter` 的显式 `FreeAvioContext()`，ownership 转移后必须 release 避免 double-free。

- S3 follow-up：`CreateAvioContext()` 内所有 raw allocation 在交给 `AVIOContext` 前都必须由本地 RAII guard 保护；`av_malloc()` buffer 只有在 `avio_alloc_context()` 成功后才 release 给 `FreeAvioContext()` 的既有 ownership 路径。
