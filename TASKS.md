# TagReader 修复任务计划

## 总体目标

- 以 `DESIGN.md` 为约束修复 `BUGS.md` 中已确认缺陷。
- 保持轻量 C++23 实现，不引入 TagLib 等标签库。
- FFmpeg 只用于文件 probe、容器识别、音频流定位、基础媒体信息和封面图像解码/PNG 编码；标题、歌手、专辑、歌词等标签字段仍必须从文件原始字节直接解析。
- 所有对外文本字段最终必须为 UTF-8。
- 封面块必须从文件原始字节解析得到，导出时统一转为 PNG。
- 每个阶段完成后运行 `cmake --build build`；如 `build/` 不存在，先运行 `cmake -S . -B build`。
- 本地没有现成音频样本时，必须自行构建最小音频样本和标签样本，再用 `./build/TagReaderTest <audio-file-path>` 验证。

## 当前代码基线

- 主实现集中在 `src/TagReader.cpp`。
- 对外入口为 `TagReader::Read(const std::filesystem::path&)`。
- 当前流程为：`ValidatePath()` -> `OpenContext()` -> `DetectStream()` -> `ReadMediaInfo()` -> `ReadMetadata()` -> `ReadLyrics()` -> `BuildMusicTag()`。
- `ReadContext` 同时保存 `std::ifstream input` 和 `AVFormatContext`。
- 当前 CMake 已链接 FFmpeg 组件：`libavformat`、`libavcodec`、`libavutil`、`libswscale`。
- 当前封面导出路径由 `MakeCoverPathForAudioFile()` 生成，后缀固定 `.png`。
- 当前 `WriteCoverAsPng()` 只识别 PNG/JPEG：PNG 直接写出，JPEG 通过 MJPEG decoder + PNG encoder 转码，其他格式直接返回空路径。
- 当前 MP4 `covr` 还要求 `dataType == 13` 对应 JPEG、`dataType == 14` 对应 PNG。
- 当前 Ogg/FLAC Vorbis Comment、ID3v1、ID3v2.2/2.3/2.4、MP4 `moov/udta/meta/ilst` 已有基本解析实现。

## 阶段 1：音频流选择简化修复

### 1.1 现状确认

- 已确认 `DetectStream()` 当前实现位于 `src/TagReader.cpp:575-635`，确实是顺序扫描 `formatContext->streams`，遇到第一个 `AVMEDIA_TYPE_AUDIO` 就写入 `audioStreamIndex`，未使用 `av_find_best_stream()`。
- 已确认 `ReadMediaInfo()` 当前实现位于 `src/TagReader.cpp:1635-1697`，采样率、码率、声道数、位深都只读取 `audioStreamIndex` 对应的 `AVStream::codecpar`；时长和偏移量优先使用容器级 `duration/start_time`，缺失时才回退到该音频流级字段。
- 已确认当前代码基线与 `BUGS.md` 第 1 条一致：在存在多个音频流时，当前实现没有“默认主音频流”判断，只会绑定第一个音频流。
- 已确认本库面向普通音乐播放器，不面向视频播放器或专业多轨播放器；阶段 1 的目标应是接入 FFmpeg 的标准 best-stream 选择，而不是自定义复杂多轨评分规则。

### 1.2 设计主音频流选择策略

- 已定稿：主音频流选择以 FFmpeg 的标准 best-stream 判定为准，直接调用 `av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)`。
- 已定稿：当 `av_find_best_stream()` 返回值大于等于 0 时，直接将其写入 `audioStreamIndex`，不再做二次评分或重排。
- 已定稿：当 `av_find_best_stream()` 返回值小于 0 时，才回退到当前的顺序扫描逻辑，选择第一个 `AVMEDIA_TYPE_AUDIO` 作为极简兼容 fallback。
- 已定稿：不实现 `default disposition`、`comment disposition`、声道数、采样率、码率等自定义评分策略；这些策略属于专业轨道选择器范畴，不符合本库“普通音乐播放器默认读取”的定位。
- 已定稿：如果 `av_find_best_stream()` 失败且顺序扫描也找不到音频流，则继续抛出 `no audio stream found in input file`，保持现有失败语义。
- 已定稿：阶段 1 只改变 `audioStreamIndex` 的来源，不改变容器识别、metadata 分发、歌词分发和 `ReadMediaInfo()` 的字段读取方式；后者仍然只消费最终选中的 `audioStreamIndex`。

### 1.3 修改代码

- 已完成：`DetectStream()` 现在先调用 `av_find_best_stream(context.formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)`。
- 已完成：当 `av_find_best_stream()` 返回值大于等于 0 时，直接将返回值写入 `context.audioStreamIndex`。
- 已完成：原先“第一个音频流”的顺序扫描逻辑已保留为 fallback，仅在 `av_find_best_stream()` 失败时执行。
- 已完成：`audioStreamIndex` 仍然只保存在 `ReadContext` 中，未改动 metadata 分发、lyrics 分发和 `ReadMediaInfo()` 的读取接口。
- 已完成：本阶段未修改 tag 字段解析逻辑。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 1.4 构建样本

- 已完成：确认本机存在 `ffmpeg` CLI：`/usr/bin/ffmpeg`。
- 已完成：在 `/tmp/opencode/tagreader_stage1_4` 生成单音轨样本 `single_track.wav`，用于确认默认单音轨场景不回退。
- 已完成：在 `/tmp/opencode/tagreader_stage1_4` 生成双音轨样本 `dual_track_default_second.mka`，第二条音轨带 `default` disposition。
- 已完成：在 `/tmp/opencode/tagreader_stage1_4` 生成双音轨差异采样率样本 `dual_track_default_second_diff_rate.mka`，其中第一条音轨为 `44100 Hz`，第二条默认音轨为 `48000 Hz`，用于让 `TagReaderTest` 输出能直接区分选中了哪条流。
- 已完成：暂未构造 `av_find_best_stream()` 失败但顺序扫描仍能找到音频流的真实样本；fallback 目前通过代码路径保留，后续如有需要可补异常样本。
- 已完成：已运行 `./build/TagReaderTest` 验证上述样本：
  - `single_track.wav` 输出 `sampleRate: 44100`、`bitRate: 705600`、`channels: 1`。
  - `dual_track_default_second.mka` 输出 `sampleRate: 44100`、`bitRate: 705600`、`channels: 1`。
  - `dual_track_default_second_diff_rate.mka` 输出 `sampleRate: 48000`、`bitRate: 768000`、`channels: 1`，说明当前实现已采用 FFmpeg 选中的默认第二音轨，而不是固定取第一个音频流。

### 1.5 验收标准

- 已完成：普通单音轨音乐文件行为不回退。样本 `single_track.wav` 输出 `sampleRate: 44100`、`bitRate: 705600`、`channels: 1`，行为符合预期。
- 已完成：多音频流样本使用 `av_find_best_stream()` 返回的音频流。样本 `dual_track_default_second_diff_rate.mka` 输出 `sampleRate: 48000`、`bitRate: 768000`，说明当前代码已选中默认第二音轨，而不是固定取第一音轨。
- 已确认：`av_find_best_stream()` 失败时仍保留第一个音频流 fallback。该点当前通过代码路径保留实现，阶段 1.4 中已记录暂未构造出稳定的真实异常样本。
- 已完成：无音频流文件仍明确失败。样本 `/tmp/opencode/tagreader_stage1_4/no_audio.mp4` 运行 `./build/TagReaderTest` 输出 `TagReader error: no audio stream found in input file`。
- 已完成：构建通过。阶段 1.3 修改后已执行 `cmake --build build` 并成功生成 `TagReaderCore` 与 `TagReaderTest`。

## 阶段 2：UTF-16 损坏输入边界修复

### 2.1 现状确认

- 已确认 `TryReadUtf16Text()` 当前实现位于 `src/TagReader.cpp:1184-1265`，循环条件确实是 `for (std::size_t i = start; i + 1 < size; i += 2)`。
- 已确认当前实现没有在进入循环前校验 `size - start` 是否为偶数；因此当 UTF-16 原始字节长度为奇数时，最后 1 个残留字节会被静默忽略，函数仍然会在 `value = TrimText(...)` 后返回 `true`。
- 已确认当前 surrogate 校验只覆盖“能读到完整 2-byte code unit 之后”的情况；对于单独残留的最后 1 字节，并不会触发失败路径。
- 已确认 UTF-16 解码入口包括：
  - `ReadId3ByteString()` 中 `encoding == 1` 走 `ReadUtf16TextWithBom()`，`encoding == 2` 走 `ReadUtf16Text(..., true)`，位置在 `src/TagReader.cpp:1328-1363`。
  - 编码嗅探路径中 `DetectTextEncoding()` 会返回 `utf-16le` / `utf-16be`，随后 `DecodeTextToUtf8(..., "utf-16le/be")` 调用 `ReadUtf16Text(...)`，位置在 `src/TagReader.cpp:3495-3563`。
- 已确认当前代码基线与 `BUGS.md` 第 5 条一致：奇数长度的损坏 UTF-16 输入当前可能被当作成功解码处理，而不是明确判定失败。

### 2.2 修改代码

- 已完成：在 `TryReadUtf16Text()` 中增加了长度校验，位置在 BOM 识别逻辑之后、主解码循环之前。
- 已完成：现在会检查 `size - start` 是否为偶数；去除 BOM 后的有效 payload 只要是奇数长度，就立即返回失败。
- 已完成：对于原始输入长度不足以构成完整 UTF-16 code unit 的情况，现在会在进入解码循环前直接失败，不再静默忽略最后 1 个字节。
- 已确认：未配对 surrogate 的现有失败行为保持不变，没有放宽原有校验。
- 已确认：合法 UTF-16LE/BE、有 BOM 和无 BOM 的正常解码路径未改动。
- 已完成：修改后已运行 `cmake --build build`，构建通过。

### 2.3 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage2_3` 生成阶段 2.3 样本目录。
- 已完成：使用 `ffmpeg` 生成最小 MP3 音频底座 `base.mp3`，后续样本都在其前面拼接 ID3v2.3 tag。
- 已完成：生成奇数长度损坏 UTF-16 样本 `id3v23_tit2_utf16_odd.mp3`：
  - `TIT2` 使用 ID3v2.3 文本编码标记 `0x01`（UTF-16 with BOM）。
  - payload 为 `FF FE` BOM 后跟奇数长度 UTF-16LE 原始字节，故意制造不完整 code unit。
- 已完成：生成合法 UTF-16LE BOM 对照样本 `id3v23_tit2_utf16le_bom.mp3`：
  - `TIT2` 使用 `0x01`。
  - 文本内容为 `Valid LE`，以合法 UTF-16LE + BOM 存储。
- 已完成：生成合法 UTF-16BE 对照样本 `id3v23_tit2_utf16be.mp3`：
  - `TIT2` 使用 `0x02`（UTF-16BE without BOM）。
  - 文本内容为 `Valid BE`，以合法 UTF-16BE 存储。
- 已完成：这些样本将直接用于阶段 2.4 验收，验证“奇数长度 UTF-16 失败、合法 UTF-16 继续成功”的行为。

### 2.4 验收标准

- 已完成：损坏奇数长度 UTF-16 字段不会写入乱码。样本 `/tmp/opencode/tagreader_stage2_3/id3v23_tit2_utf16_odd.mp3` 运行 `TagReaderTest` 后输出 `title:` 为空，没有写入截断乱码。
- 已完成：合法 UTF-16 字段仍能输出 UTF-8。样本 `/tmp/opencode/tagreader_stage2_3/id3v23_tit2_utf16le_bom.mp3` 输出 `title: Valid LE`，样本 `/tmp/opencode/tagreader_stage2_3/id3v23_tit2_utf16be.mp3` 输出 `title: Valid BE`。
- 已完成：`cmake --build build` 通过。阶段 2.4 验收前已重新运行构建，输出为 `ninja: no work to do.`。

## 阶段 3：ID3v2.2 同步歌词 `SLT` 支持

### 3.1 现状确认

- 已确认 `ReadID3v22LyricsFrames()` 当前实现位于 `src/TagReader.cpp:2997-3040`，只对 `frameId == "ULT"` 做了解析，并将其作为纯文本歌词写入 `lyrics.text`。
- 已确认 `ReadID3v22LyricsFrames()` 中没有 `SLT` 分支；当前源码直接保留注释 `v2.2 SLT is intentionally skipped until timestamp conversion is implemented.`，即明确跳过 `SLT`。
- 已确认 `ReadID3v23Or24LyricsFrames()` 当前实现位于 `src/TagReader.cpp:3042-3218`，已经具备 `SYLT` 解析逻辑：
  - 读取 `encoding`、`timestampFormat`、`contentType`、descriptor。
  - 仅接受 `timestampFormat == 2`。
  - 将 32-bit 毫秒时间戳转换为 `std::chrono::microseconds(timestampMs * 1000)`。
  - 对文本终止符、descriptor 和时间戳长度都做了边界检查。
- 已确认阶段 3 的实现基础已经存在：ID3v2.3/v2.4 `SYLT` 可作为 ID3v2.2 `SLT` 的主要参考实现，而当前缺口仅在 `ReadID3v22LyricsFrames()` 内部。

### 3.2 明确协议字段

- 已定稿：ID3v2.2 `SLT` 按以下结构解析：
  - `text encoding`: 1 byte
  - `language`: 3 bytes
  - `timestamp format`: 1 byte
  - `content type`: 1 byte
  - `content descriptor`: 按 `text encoding` 解释的字符串，以对应终止符结束
  - 后续内容为重复的 `text + terminator + 32-bit timestamp` 条目序列
- 已定稿：实现时复用现有 ID3 文本辅助逻辑，不单独发明新的编码解析路径；`text encoding` 的解释继续沿用 `ReadId3ByteString()`、`FindEncodedTerminator()` 和 `EncodedTerminatorWidth()`。
- 已定稿：只支持 `timestampFormat == 2` 的 milliseconds，与当前 ID3v2.3/v2.4 `SYLT` 保持一致；时间戳统一转换为 `std::chrono::microseconds(static_cast<int64_t>(timestampMs) * 1000)`。
- 已定稿：`timestampFormat == 1`（MPEG frames）本阶段不做帧号到时间的换算，直接安全跳过，不写入错误的同步歌词。
- 已定稿：未知 `timestampFormat` 同样安全跳过，不抛异常、不生成半成品歌词。
- 已定稿：`content type` 本阶段不作为过滤条件，仅解析但不据此拒绝歌词；保持与当前 `SYLT` 路径处理方式一致。
- 已定稿：`content descriptor` 仅用于跳过，不写入 `Lyrics`，与当前 `USLT` / `SYLT` 的处理风格保持一致。
- 已定稿：每个 `SLT` 条目都必须满足“文本终止符存在且后续至少还有 4 字节 timestamp”；任一条目不满足时停止解析当前帧剩余内容，但不越界。

### 3.3 修改代码

- 已完成：在 `ReadID3v22LyricsFrames()` 中增加了 `SLT` 分支，位置仍保持在该分发函数内部，没有把实现塞回 `ReadLyrics()`。
- 已完成：实现中复用了现有辅助逻辑 `FindEncodedTerminator()`、`EncodedTerminatorWidth()`、`ReadId3ByteString()`，没有新增重复的文本解码路径。
- 已完成：`SLT` 解析现在会读取 `encoding`、`timestampFormat`、`contentType`、descriptor，并在 `timestampFormat == 2` 时解析重复的 `text + terminator + 32-bit timestamp` 条目。
- 已完成：每一条同步歌词都带有边界检查：
  - descriptor 终止符不存在时不进入条目解析；
  - 文本终止符不存在时停止当前帧解析；
  - 文本后不足 4 字节 timestamp 时停止当前帧解析。
- 已完成：空文本行不会写入 `timedLines`；只有 `TrimText()` 后非空的歌词行才会被保留。
- 已完成：`ReadID3v22LyricsFrames()` 现已改为先收集 `ULT` 候选和 `SLT` 候选，再在函数尾部按“同步歌词优先于纯文本歌词”的策略写入 `lyrics`。
- 已完成：如果 `SLT` 成功解析出 `timedLines`，则优先输出同步歌词；只有 `SLT` 为空时，才回退到 `ULT` 纯文本歌词。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 3.4 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage3_4` 生成阶段 3.4 样本目录。
- 已完成：使用 `ffmpeg` 生成最小 MP3 音频底座 `base.mp3`，后续样本都在其前面拼接 ID3v2.2 tag。
- 已完成：生成 `ULT` 纯文本歌词样本 `id3v22_ult.mp3`，文本内容为两行普通歌词，用于验证 v2.2 纯文本歌词路径仍然可用。
- 已完成：生成 `SLT` milliseconds 样本 `id3v22_slt_ms.mp3`，包含至少两条同步歌词：
  - `Line one` @ `500 ms`
  - `Line two` @ `1250 ms`
- 已完成：生成 `SLT` `timestampFormat == 1` 样本 `id3v22_slt_frames.mp3`，用于验证 MPEG frame timestamp 会被安全跳过。
- 已完成：生成截断 `SLT` 样本 `id3v22_slt_truncated.mp3`，descriptor 和文本完整，但故意截断最后 1 个 timestamp 字节，用于验证不会越界或崩溃。
- 已完成：这些样本将直接用于阶段 3.5 验收，验证“milliseconds 成功、frame timestamp 跳过、截断安全停止”的行为。

### 3.5 验收标准

- 已完成：`SLT` milliseconds 输出 `lyricsCount > 0`。样本 `/tmp/opencode/tagreader_stage3_4/id3v22_slt_ms.mp3` 运行 `TagReaderTest` 后输出 `lyricsCount: 2`，符合毫秒同步歌词预期。
- 已完成：`timestampFormat == 1` 不产生错误歌词。样本 `/tmp/opencode/tagreader_stage3_4/id3v22_slt_frames.mp3` 输出 `lyricsCount: 0`，说明 MPEG frame timestamp 被安全跳过，没有写入错误歌词。
- 已完成：截断样本不会崩溃。样本 `/tmp/opencode/tagreader_stage3_4/id3v22_slt_truncated.mp3` 运行完成并输出 `lyricsCount: 0`，没有异常退出。
- 已完成：`ULT` 纯文本路径回归正常。样本 `/tmp/opencode/tagreader_stage3_4/id3v22_ult.mp3` 输出 `lyricsCount: 2`，说明新增 `SLT` 支持没有破坏原有 v2.2 纯文本歌词读取。
- 已完成：构建通过。阶段 3.5 验收前已重新运行 `cmake --build build`，输出为 `ninja: no work to do.`。

## 阶段 4：MP4 歌词 64 位 atom 尺寸修复

### 4.1 现状确认

- 已确认 `ReadMP4AtomTree()` 当前实现位于 `src/TagReader.cpp:2588-2677`，已经处理 `atomSize == 1` 的 64 位扩展尺寸，并通过 `TryAddUintmax()` 计算 `atomEnd`，对 metadata 主路径的溢出保护相对完整。
- 已确认 `ReadMP4ItemAtom()` 当前实现位于 `src/TagReader.cpp:2679-2812`，也已经处理 `size == 1` / `childSize == 1` 的 64 位扩展尺寸，并在多处使用 `TryAddUintmax()` 校验 `sizeEnd`、`atomLimit`、`childEnd`、`childLimit`。
- 已确认 `ReadMP4LyricsAtomTree()` 当前实现位于 `src/TagReader.cpp:3370-3421`，虽然读取了 `atomSize == 1` 的 64 位扩展尺寸，但后续仍直接使用 `const std::uintmax_t atomEnd = cursor + static_cast<std::uintmax_t>(atomSize);`，没有像 metadata 路径那样使用 `TryAddUintmax()` 做溢出保护。
- 已确认 `ReadMP4LyricsAtomTree()` 当前对 `atomSize == 0` 直接 `return`，与 metadata 路径“扩展到当前 limit”的处理策略也不一致。
- 已确认 `ReadMP4LyricsItem()` 当前实现位于 `src/TagReader.cpp:3720-3776`，在读取 `size == 1` 的 64 位扩展尺寸后，仍然继续使用 `if (size < 8 || size > limit - cursor)` 判断，因此 largesize 场景会因为原始 32-bit `size` 仍等于 `1` 而直接返回，不能正确处理 64 位 item。
- 已确认 `ReadMP4LyricsItem()` 还存在与 metadata 路径不一致的边界处理：
  - 没有使用 `TryAddUintmax()` 计算 `cursor + size`；
  - `ReadRange(..., cursor + size - payloadOffset)` 仍依赖直接加法；
  - `offset + 8 > limit` 这类判断也未使用统一的无溢出写法。
- 已确认当前代码基线与 `BUGS.md` 第 3 条一致：MP4 歌词路径对 64 位 atom 尺寸的支持不完整，主要缺口集中在 `ReadMP4LyricsItem()`，次要缺口集中在 `ReadMP4LyricsAtomTree()` 的溢出保护不一致。

### 4.2 抽出或复用 MP4 atom 边界逻辑

- 已定稿：优先新增一个文件内静态小工具函数，例如 `ReadMp4AtomHeader(...)`，统一 metadata 路径与 lyrics 路径的 atom 边界处理，而不是继续在 4 个函数里各自复制 `size == 1` / `size == 0` / `cursor + size` 逻辑。
- 已定稿：该 helper 至少统一处理以下字段和规则：
  - 8 字节普通 atom header 读取。
  - `size == 1` 时继续读取 64 位 `largesize`。
  - `size == 0` 时将 atom 终点解释为当前 `limit`。
  - 统一产出 `atomType`、`headerSize`、`payloadOffset`、`atomEnd`、`atomSize`。
  - 所有 `offset + 8`、`offset + 16`、`cursor + size`、`payloadOffset <= atomEnd` 都通过 `TryAddUintmax()` 或等价无溢出写法校验。
- 已定稿：helper 失败时返回 `false`，由调用方在当前扫描层直接 `return` 或安全终止，不抛新异常，保持现有 MP4 路径的容错风格。
- 已定稿：metadata 路径与 lyrics 路径对 `size == 0` 的语义必须统一为“延伸到当前 limit”，不能一条路径支持、另一条路径直接 `return`。
- 已定稿：`meta` full box 的 `childOffset = cursor + 12` 这类固定偏移不必全部抽入 helper，但所有参与计算的加法必须改为无溢出形式；helper 只负责 atom header 与边界本身。
- 已定稿：后续 4.3 中优先先让 `ReadMP4LyricsAtomTree()` 和 `ReadMP4LyricsItem()` 改用该 helper；如改动足够小且不引入回归，也可顺手让 `ReadMP4AtomTree()` / `ReadMP4ItemAtom()` 复用同一 helper，减少双轨维护成本。

### 4.3 修改代码

- 已完成：新增文件内静态 helper `ReadMp4AtomHeader(...)`，统一处理 MP4 atom 的普通 8 字节 header、`size == 1` 的 64 位 largesize、`size == 0` 延伸到当前 `limit`、`payloadOffset`、`atomEnd` 和无溢出边界计算。
- 已完成：`ReadMP4LyricsAtomTree()` 已改为使用 `ReadMp4AtomHeader(...)`，不再直接使用 `cursor + atomSize` 计算 `atomEnd`；`meta` full box 的 `childOffset` 也改为通过 `TryAddUintmax()` 计算。
- 已完成：`ReadMP4LyricsItem()` 已改为使用 `ReadMp4AtomHeader(...)`，现在能够正确处理 `size == 1` 的 largesize item，不会再因为原始 32-bit `size == 1` 而在 `size < 8` 判断处提前返回。
- 已完成：`ReadMP4LyricsItem()` 中 `ReadRange(..., cursor + size - payloadOffset)` 这类直接加法已移除，改为基于 helper 提供的 `atomEnd - payloadOffset` 计算读取范围。
- 已完成：所有阶段 4.3 涉及的 `offset + 8`、`offset + 16`、`payloadOffset + 4`、`cursor + size` 等关键加法都已改为 `TryAddUintmax()` 或等价无溢出写法。
- 已完成：为减少双轨维护，metadata 路径 `ReadMP4AtomTree()` 与 `ReadMP4ItemAtom()` 也顺手复用了同一个 helper，统一了 `size == 0`、largesize 和边界处理语义。
- 已确认：`ReadMP4Lyrics()` 仍然只做分发，没有被塞入具体 atom 解析细节。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 4.4 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage4_4` 生成阶段 4.4 样本目录。
- 已完成：生成普通 32 位 atom size 的最小 MP4 歌词样本 `m4a_lyrics_plain.m4a`，结构为 `ftyp -> moov -> udta -> meta -> ilst -> ©lyr -> data`，其中 `data` payload 为 UTF-8 文本歌词。
- 已完成：生成 `©lyr` item 使用 64 位 largesize 的样本 `m4a_lyrics_item_largesize.m4a`，用于覆盖 lyrics item 自身的 largesize 解析路径。
- 已完成：生成 `data` 子 atom 使用 64 位 largesize 的样本 `m4a_lyrics_data_largesize.m4a`，用于覆盖歌词 item 内部 `data` atom 的 largesize 解析路径。
- 已完成：生成截断 largesize 样本 `m4a_lyrics_item_largesize_truncated.m4a`，将 `©lyr` item 的 32 位 `size` 改为 `1`，但故意只保留不完整的 64 位扩展尺寸字节，用于验证代码会安全返回而不是越界。
- 已完成：由于上述最小结构样本没有真实音频流，额外基于 `base_lyrics.m4a` 派生了带真实音频流的有效验收样本：
  - `m4a_lyrics_plain_real_audio.m4a`
  - `m4a_lyrics_item_largesize_real_audio.m4a`
  - `m4a_lyrics_data_largesize_real_audio.m4a`
  - `m4a_lyrics_item_largesize_truncated_real_audio.m4a`
- 已完成：阶段 4.5 的最终验收将以这些“真实音频流 + 覆盖对应 atom 结构”的样本为主；最小结构样本保留为纯结构调试参考。

### 4.5 验收标准

- 已完成：普通 MP4 歌词仍能读取。样本 `/tmp/opencode/tagreader_stage4_4/m4a_lyrics_plain_real_audio.m4a` 运行 `TagReaderTest` 后输出 `lyricsCount: 1`。
- 已完成：64 位 atom size MP4 歌词能读取。样本 `/tmp/opencode/tagreader_stage4_4/m4a_lyrics_item_largesize_real_audio.m4a` 和 `/tmp/opencode/tagreader_stage4_4/m4a_lyrics_data_largesize_real_audio.m4a` 均输出 `lyricsCount: 1`，说明 item largesize 与 data largesize 两条路径都已生效。
- 已完成：截断或溢出 atom 不崩溃、不越界。样本 `/tmp/opencode/tagreader_stage4_4/m4a_lyrics_item_largesize_truncated_real_audio.m4a` 运行完成并输出 `lyricsCount: 0`，没有异常退出。
- 已完成：构建通过。阶段 4.5 验收前已重新运行 `cmake --build build`，输出为 `ninja: no work to do.`。
- 构建通过。

## 阶段 5：MP4 freeform 歌词支持

### 5.1 现状确认

- 已确认 `ReadLyrics()` 当前在 MP4 分支只调用 `ReadMP4Lyrics(context, lyrics)`，位置在 `src/TagReader.cpp:3478-3508`，没有任何额外的 MP4 歌词补充来源。
- 已确认 `ReadMP4LyricsAtomTree()` 当前实现位于 `src/TagReader.cpp:3325-3376`，在 `ilst` 层只对 `atomType == ©lyr` 调用 `ReadMP4LyricsItem()`；`moov`、`udta`、`meta`、`ilst` 仅作为递归容器处理，没有识别 `----` item。
- 已确认 `ReadMP4LyricsItem()` 当前实现位于 `src/TagReader.cpp:3687-3743`，只会在 `atomType == ©lyr` 且 `dataType == 1 || dataType == 0` 时把 `data` payload 解释为歌词文本。
- 已确认当前源码中不存在 `----`、`mean`、`name`、`com.apple.iTunes`、`LYRICS` 等 freeform MP4 歌词关键字的解析逻辑。
- 已确认当前代码基线与 `BUGS.md` 第 4 条一致：MP4 歌词路径目前只支持 `moov/udta/meta/ilst/©lyr`，常见 freeform 歌词 `----:com.apple.iTunes:LYRICS` 尚未支持。

### 5.2 明确 freeform 结构

- 已定稿：在 `ilst` 层除了现有 `©lyr` 之外，还要识别 `----` item，并把它作为 MP4 freeform 候选歌词项送入专门解析函数。
- 已定稿：freeform item 内部至少解析 3 类子 atom：
  - `mean`：通常应为 `com.apple.iTunes`
  - `name`：通常应为 `LYRICS`
  - `data`：实际歌词文本
- 已定稿：`mean`、`name`、`data` 都按 full box 风格处理；读取 payload 时需要跳过 `version/flags` 与 type indicator / locale 等固定头部字段，不能把这些头部字节直接当正文。
- 已定稿：仅当 `mean` 精确等于 `com.apple.iTunes`，且 `name` 在大小写归一后等于 `lyrics` 时，才把对应 `data` 认定为歌词；其他 freeform `----` 字段全部忽略，避免误识别。
- 已定稿：freeform `data` 的文本解码规则与现有 `©lyr` 保持一致：
  - `dataType == 1` 按 UTF-8 验证并解码
  - `dataType == 0` 走 `DecodeRawText()`
  - 其他 `dataType` 一律跳过
- 已定稿：freeform 路径只产出纯文本歌词，不在本阶段新增同步时间戳协议；如文本本身包含 LRC 时间标签，后续仍由现有 `ReadLyricsFromPlainText()` 负责拆行与时间解析。
- 已定稿：优先级保持“`©lyr` 高于 freeform”不变；如果同一文件同时存在 `©lyr` 和 `----:com.apple.iTunes:LYRICS`，则 `©lyr` 优先，freeform 只在当前 `lyrics` 仍为空时补充。

### 5.3 修改代码

- 已完成：在 MP4 lyrics atom 扫描中新增了 `----` item 分支；`ReadMP4LyricsAtomTree()` 现在在 `ilst` 层遇到 `----` 时，会把该 item 送入专门的 freeform 歌词解析函数。
- 已完成：新增小函数 `ReadMP4FreeformLyricsItem(...)`，位置在 `src/TagReader.cpp` 的 MP4 歌词相关实现区域，专门负责解析 freeform `mean` / `name` / `data` 子 atom。
- 已完成：`ReadMP4FreeformLyricsItem(...)` 现在会：
  - 读取 `mean` 与 `name` 的 payload，并跳过前 4 字节 full box `version/flags`；
  - 读取 `data` payload，并按当前 MP4 文本规则跳过 8 字节 `dataType/locale` 头部；
  - `dataType == 1` 时按 UTF-8 验证，`dataType == 0` 时走 `DecodeRawText()`。
- 已完成：只有当 `mean == "com.apple.iTunes"` 且 `ToLower(name) == "lyrics"` 时，才会把 `data` 文本送入 `ReadLyricsFromPlainText()`；普通 `----` 其他字段不会误识别为歌词。
- 已完成：当前实现保持 `©lyr` 优先。`ReadMP4LyricsAtomTree()` 只有在 `lyrics.text` 与 `lyrics.timedLines` 仍为空时，才会对 `----` freeform item 进行补充解析，因此 `©lyr` 和 freeform 同时存在时仍由 `©lyr` 优先。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 5.4 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage5_4` 生成阶段 5.4 样本目录。
- 已完成：所有样本均基于带真实音频流的 `base_lyrics.m4a` 作为底座，只替换 `moov/udta/meta/ilst` 中的歌词 item，确保样本能稳定进入当前 MP4 歌词解析路径。
- 已完成：生成 `©lyr` 样本 `m4a_clyr_only.m4a`，其中 `ilst` 仅包含 `©lyr -> data`，用于验证原有 `©lyr` 路径仍然可用。
- 已完成：生成 freeform 歌词样本 `m4a_freeform_lyrics_only.m4a`，其中 `ilst` 仅包含 `---- -> mean(com.apple.iTunes) -> name(LYRICS) -> data`，用于验证 freeform 歌词新增支持。
- 已完成：生成普通 `----` 非歌词样本 `m4a_freeform_nonlyrics.m4a`，其中 `mean` 仍为 `com.apple.iTunes`，但 `name` 为 `COMMENT`，用于验证不会误识别为歌词。
- 已完成：生成 `©lyr` + freeform 同时存在样本 `m4a_clyr_and_freeform.m4a`，用于验证 `©lyr` 优先、freeform 仅在歌词为空时补充的策略。

### 5.5 验收标准

- 已完成：freeform 歌词能输出 `lyricsCount > 0`。样本 `/tmp/opencode/tagreader_stage5_4/m4a_freeform_lyrics_only.m4a` 运行 `TagReaderTest` 后输出 `lyricsCount: 2`，说明 `----:com.apple.iTunes:LYRICS` 路径已经生效。
- 已完成：非歌词 freeform 不产生歌词。样本 `/tmp/opencode/tagreader_stage5_4/m4a_freeform_nonlyrics.m4a` 输出 `lyricsCount: 0`，说明普通 `----` 字段未被误识别为歌词。
- 已完成：同时存在时优先 `©lyr`。样本 `/tmp/opencode/tagreader_stage5_4/m4a_clyr_and_freeform.m4a` 输出 `lyricsCount: 1`，与 `m4a_clyr_only.m4a` 的 `lyricsCount: 1` 一致，说明在 `©lyr` 与 freeform 同时存在时仍由 `©lyr` 提供歌词结果。
- 已完成：`©lyr` 原有路径未回归。样本 `/tmp/opencode/tagreader_stage5_4/m4a_clyr_only.m4a` 输出 `lyricsCount: 1`，说明新增 freeform 支持没有破坏现有 `©lyr` 解析。
- 已完成：构建通过。阶段 5.5 验收前已重新运行 `cmake --build build`，输出为 `ninja: no work to do.`。

## 阶段 6：封面导出基础重构

### 6.1 现状确认

- 已确认 `ImageFormat` 当前只定义了 `Unknown`、`Png`、`Jpeg` 三种枚举值，位置在 `src/TagReader.cpp:269-274`。
- 已确认 `DetectImageFormat()` 当前只识别 PNG 签名和 JPEG 签名，其他所有图像字节都会返回 `ImageFormat::Unknown`，位置在 `src/TagReader.cpp:276-294`。
- 已确认 `WriteCoverAsPng()` 当前实现位于 `src/TagReader.cpp:434-463`：
  - PNG 直接写出为 `.png`
  - JPEG 走 `ConvertJpegToPng()` 转码
  - 其他格式在 `DetectImageFormat()` 阶段直接返回空路径
- 已确认 `ConvertJpegToPng()` 当前实现位于 `src/TagReader.cpp:370-432`，固定使用 `avcodec_find_decoder(AV_CODEC_ID_MJPEG)` 和 MJPEG decoder，无法解码 BMP、WEBP、GIF、TIFF 等非 JPEG 图片。
- 已确认当前封面导出入口已经集中：
  - ID3v2.2 `PIC` -> `WriteCoverAsPng()`，位置在 `src/TagReader.cpp:2192`
  - ID3v2.3/2.4 `APIC` -> `WriteCoverAsPng()`，位置在 `src/TagReader.cpp:2318`
  - FLAC `PICTURE` -> `WriteCoverAsPng()`，位置在 `src/TagReader.cpp:2643`
  - MP4 `covr` -> `WriteCoverAsPng()`，位置在 `src/TagReader.cpp:2893`
- 已确认当前 `CMakeLists.txt` 已链接 `libavformat`、`libavcodec`、`libavutil`、`libswscale`，因此阶段 6 的封面导出基础重构可以优先复用现有 FFmpeg 依赖，不需要为了图像转码先引入 OpenCV。

### 6.2 设计新的封面转码接口

- 已定稿：保留 `WriteCoverAsPng(audioPath, data, size)` 作为唯一公共封面导出入口，ID3 / FLAC / MP4 现有调用点不改签名，只替换其内部实现。
- 已定稿：`WriteCoverAsPng()` 的内部流程统一为：
  - 如果输入字节已经是 PNG，可直接写出，以保持最快路径。
  - 否则先走通用图像解码，得到 `AVFrame`。
  - 再把解码帧转换到 PNG encoder 可接受的像素格式。
  - 最后统一编码为 PNG 字节并写入临时 `.png` 文件。
- 已定稿：当前阶段优先复用现有 FFmpeg 依赖实现通用图像解码，不先引入 OpenCV；只有在后续样本验证证明 FFmpeg 路径无法满足目标格式时，才重新评估额外依赖。
- 已定稿：通用图像解码优先走 `libavcodec` 路径，使用“按图像格式选择 decoder -> `AVPacket` 输入原始字节 -> `AVFrame` 解码输出”的方式；如单纯 `libavcodec` 路径无法稳定覆盖目标格式，再在后续阶段评估 `avformat_open_input()` + 内存 IO 的方案。
- 已定稿：现有 `EncodeFrameAsPng()` 保留并继续复用，但 `ConvertJpegToPng()` 将被更通用的接口取代或下沉为兼容包装；后续新增函数名以能力表达为准，例如：
  - `DecodeImageBytesToFrame()`：负责把任意可解码图像字节解到 `AVFrame`
  - `ConvertImageToPng()`：负责从任意图像字节产出 PNG 字节
- 已定稿：像素格式转换仍统一通过 `sws_scale` 路径处理，但后续实现要避免把输入限制死在 MJPEG；转换目标优先保持为 PNG encoder 当前稳定支持的 RGB 路径，必要时在后续阶段再扩展 alpha 保留能力。

### 6.3 修改依赖与资源管理

- 已完成：确认当前 `CMakeLists.txt` 已包含 `libavcodec`、`libavutil`、`libswscale`，阶段 6.3 不需要新增依赖即可继续推进封面通用解码准备。
- 已完成：当前阶段未引入 `avio_alloc_context` / `AVIOContext`。原因是阶段 6.2 已定稿“优先走 `libavcodec` 直接解码图像字节”的路径，现阶段尚不需要 demuxer 内存 IO；如后续样本验证证明必须走 `avformat_open_input()` 内存 IO，再单独补 `AVIOContext` 的 RAII 管理。
- 已完成：继续沿用现有 FFmpeg RAII 风格，保持 `AVCodecContext`、`AVPacket`、`AVFrame`、`SwsContext` 都通过自定义 deleter 管理，没有引入裸资源泄露点。
- 已完成：为后续封面通用解码路径补了基础字节封装辅助 `ReadImageBytes(...)`，把“原始图像字节 -> `AVPacket` 内容”的准备逻辑收口到现有 `AVPacket` 生命周期管理之下，避免后续在多个图像 decoder 路径里重复手写 `av_new_packet()` + `memcpy()`。
- 已完成：`ConvertJpegToPng()` 已改为复用上述基础字节准备逻辑，同时保留现有 `AVCodecContext`、`AVPacket`、`AVFrame`、`SwsContext` 的 RAII 管理方式，为后续替换成通用图像 decoder 铺路。
- 已完成：本阶段修改后已运行 `cmake --build build`，构建通过。

### 6.4 验收标准

- 已完成：PNG 输入仍直接导出 `.png`，内容签名正确。样本 `/tmp/opencode/tagreader_stage6_4/id3v23_apic_png.mp3` 运行 `TagReaderTest` 后输出非空 `coverPath`，导出文件 `/tmp/id3v23_apic_png_-4658303066002726349_0.png` 的后缀为 `.png`，且 PNG 签名检查为 `True`。
- 已完成：JPEG 输入仍转为 `.png`，内容签名正确。样本 `/tmp/opencode/tagreader_stage6_4/id3v23_apic_jpeg.mp3` 输出非空 `coverPath`，导出文件 `/tmp/id3v23_apic_jpeg_-4658303065972300705_0.png` 的后缀为 `.png`，且 PNG 签名检查为 `True`。
- 已完成：不支持或损坏图像返回空 `coverPath`，且不会崩溃。样本 `/tmp/opencode/tagreader_stage6_4/id3v23_apic_bad.mp3` 运行完成并输出 `coverPath:` 为空，没有异常退出。
- 已完成：阶段 6.4 验收基于 ID3v2.3 `APIC` 样本完成，说明在当前重构前的基线路径下，PNG 直写和 JPEG->PNG 转码行为均未回归。
- 已完成：构建通过。阶段 6.4 验收前 `TagReaderTest` 可正常运行，且最近一次 `cmake --build build` 已通过。

## 阶段 7：为封面新增 BMP 支持

### 7.1 修改代码

- 已完成：在当前封面转码路径中新增 BMP 支持。`ImageFormat` 现已增加 `Bmp` 枚举值，`DetectImageFormat()` 现可通过 `BM` 文件头识别 BMP。
- 已完成：保留了 `DetectImageFormat()` 作为快速签名识别与诊断入口，但 BMP 支持已不再依赖 JPEG 专用逻辑，而是接入新的通用图像解码路径。
- 已完成：新增通用转码函数 `ConvertImageToPng(data, size, AVCodecID codecId)`，把“按指定 codec 解码图像 -> `AVFrame` -> RGB 转换 -> PNG 编码”的流程收口到单一路径。
- 已完成：原有 `ConvertJpegToPng()` 已下沉为对 `ConvertImageToPng(..., AV_CODEC_ID_MJPEG)` 的包装，便于后续继续扩展更多图像格式。
- 已完成：`WriteCoverAsPng()` 现在对 BMP 不再返回空路径，而是改为调用 `ConvertImageToPng(..., AV_CODEC_ID_BMP)`，最终仍统一导出为 `.png`。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 7.2 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage7_2` 生成阶段 7.2 样本目录。
- 已完成：使用 `ffmpeg` 生成最小 BMP 图片字节 `cover.bmp`，为 `8x8` 单帧位图，可直接用于各类封面块嵌入。
- 已完成：生成 ID3v2.3 `APIC` BMP 封面样本 `id3v23_apic_bmp.mp3`：
  - `APIC` MIME 为 `image/bmp`
  - `pictureType == 3`（front cover）
- 已完成：生成 FLAC `PICTURE` BMP 封面样本 `flac_picture_bmp.flac`：
  - `PICTURE` block 的 MIME 为 `image/bmp`
  - `pictureType == 3`（front cover）
- 已完成：生成 MP4 `covr` BMP payload 样本 `m4a_covr_bmp.m4a`：
  - 基于带真实音频流的 `base.m4a` 派生
  - 在 `moov/udta/meta/ilst` 下插入 `covr -> data`，其中 payload 为 BMP 字节
  - `dataType` 当前写为 `0`，用于后续验证“至少不会因 dataType 限制直接跳过可解码 BMP”的目标

### 7.3 验收标准

- 已完成：BMP 封面样本在当前已接入的通用解码路径下可以导出非空 `coverPath`。样本 `/tmp/opencode/tagreader_stage7_2/id3v23_apic_bmp.mp3` 与 `/tmp/opencode/tagreader_stage7_2/flac_picture_bmp.flac` 运行 `TagReaderTest` 后均输出非空 `coverPath`。
- 已完成：导出路径后缀为 `.png`。上述两个样本分别导出：
  - `/tmp/id3v23_apic_bmp_-4658302066450072827_0.png`
  - `/tmp/flac_picture_bmp_-4658302066421161862_0.png`
- 已完成：导出文件 PNG 签名正确。两条导出文件的前 8 字节检查结果均为 `png_signature=True`。
- 已确认：MP4 `covr` BMP 样本 `/tmp/opencode/tagreader_stage7_2/m4a_covr_bmp.m4a` 当前仍输出空 `coverPath`，原因是 `ReadMP4DataAtom()` 仍保留阶段 9 待修复的 `covr` dataType/签名限制；这不影响阶段 7 对“BMP 经通用图像解码路径可转 PNG”的主体验收结论。
- 已完成：构建通过。阶段 7.3 验收前已重新运行 `cmake --build build`，输出为 `ninja: no work to do.`。

## 阶段 8：为封面新增 WEBP/GIF/TIFF 等格式支持

### 8.1 现状确认

- 已确认本机 `ffmpeg -decoders` 可用，当前环境的 FFmpeg 构建已包含以下相关图像 decoder：
  - `bmp`
  - `gif`
  - `tiff`
  - `webp`
- 已确认当前环境还包含多种额外图像 decoder（如 `png`、`mjpeg`、`dds`、`exr`、`apng` 等），说明后续阶段可以优先按“给定 `AVCodecID` 直接找 decoder”的路径扩展，而不必先引入新依赖。
- 已确认后续实现不能依赖“本机一定支持所有目标格式”这一前提；即使当前环境支持 `webp/gif/tiff`，代码本身仍必须在 `avcodec_find_decoder(...) == nullptr` 或解码失败时安全返回空 `coverPath`，而不是崩溃或写出损坏文件。
- 已确认阶段 8 的样本与验收应同时覆盖两类结果：
  - 当前环境支持的格式应成功导出 PNG。
  - 若未来换环境缺少某个 decoder，代码也应安全失败并保持 `coverPath` 为空。

### 8.2 修改代码

- 已完成：通用图像解码路径不再只面向 MJPEG/BMP；`DetectImageFormat()` 现已新增 `Webp`、`Gif`、`Tiff` 签名识别，`WriteCoverAsPng()` 也已能把这些格式分发到 `ConvertImageToPng()`。
- 已完成：当前 `WriteCoverAsPng()` 对可识别的图像字节会按格式选择对应 FFmpeg decoder：
  - JPEG -> `AV_CODEC_ID_MJPEG`
  - BMP -> `AV_CODEC_ID_BMP`
  - WEBP -> `AV_CODEC_ID_WEBP`
  - GIF -> `AV_CODEC_ID_GIF`
  - TIFF -> `AV_CODEC_ID_TIFF`
- 已完成：对当前签名识别不到、但仍可能是可解码图片的 payload，`WriteCoverAsPng()` 现已增加有限的 FFmpeg decoder 试探分支；会按小范围候选 codec（当前为 `WEBP/GIF/TIFF/BMP/MJPEG`）依次尝试解码，任一成功即转 PNG，全部失败则安全返回空 `coverPath`。
- 已完成：对 GIF / WEBP 这类可能带动画的输入，当前实现沿用 `avcodec_send_packet()` + `avcodec_receive_frame()` 的首帧输出路径，因此至少会导出第一帧作为封面。
- 已确认：当前 PNG 编码路径仍统一先把解码帧转成 `AV_PIX_FMT_RGB24`，因此阶段 8.2 的目标是“确保能输出正确 PNG 图像”；alpha 保留能力暂未扩展，后续如确有需要再单独增强。
- 已完成：在通用解码路径中新增了基础尺寸防护：解码后会检查 `decodedFrame->width`、`decodedFrame->height` 必须大于 0，并通过 `av_image_check_size(...)` 做基础合法性校验，避免异常尺寸继续进入分配与缩放流程。
- 已确认：阶段 8.2 还未引入更激进的大图防护（例如像素总数上限、自定义内存阈值），但现阶段已补上“宽高必须有效且通过 FFmpeg 尺寸检查”的底线防护。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 8.3 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage8_4` 生成阶段 8 样本目录。
- 已完成：使用 `ffmpeg` 生成最小 WEBP 图片样本 `cover.webp`，尺寸为 `16x16`。
- 已完成：使用 `ffmpeg` 生成最小 GIF 图片样本 `cover.gif`，尺寸为 `16x16`。
- 已完成：使用 `ffmpeg` 生成最小 TIFF 图片样本 `cover.tiff`，尺寸为 `16x16`。
- 已完成：使用 `ffmpeg` 生成最小 MP3 音频底座 `base.mp3`，供 ID3v2.3 `APIC` 样本复用。
- 已完成：额外生成最小 FLAC 音频底座 `base.flac`，作为后续若需补充 `PICTURE` 路径样本的备用底座；本阶段主体验收已由 `APIC` 样本覆盖。
- 已完成：将 `cover.webp` 嵌入 ID3v2.3 `APIC`，生成样本 `id3v23_apic_webp.mp3`。
- 已完成：将 `cover.gif` 嵌入 ID3v2.3 `APIC`，生成样本 `id3v23_apic_gif.mp3`。
- 已完成：将 `cover.tiff` 嵌入 ID3v2.3 `APIC`，生成样本 `id3v23_apic_tiff.mp3`。
- 已确认：本阶段样本均采用 `pictureType == 3` 的 front cover 语义，以避免与阶段 10 的封面类型规则混淆。

### 8.4 验收标准

- 已完成：本机 FFmpeg 当前支持的 `WEBP/GIF/TIFF` 样本均能导出非空 `coverPath`：
  - `/tmp/opencode/tagreader_stage8_4/id3v23_apic_webp.mp3` -> `/tmp/id3v23_apic_webp_-4658300049030235930_0.png`
  - `/tmp/opencode/tagreader_stage8_4/id3v23_apic_gif.mp3` -> `/tmp/id3v23_apic_gif_-4658300049005005797_0.png`
  - `/tmp/opencode/tagreader_stage8_4/id3v23_apic_tiff.mp3` -> `/tmp/id3v23_apic_tiff_-4658300048978733984_0.png`
- 已完成：上述 3 个导出文件都已通过 `file` 检查，确认文件内容为 `PNG image data, 16 x 16, 8-bit/color RGB, non-interlaced`。
- 已确认：本阶段没有额外构造“当前环境缺少 decoder”的负样本，因为 8.1 已确认本机支持 `webp/gif/tiff`；但当前代码路径仍保持 `avcodec_find_decoder(...) == nullptr` 或解码失败时安全返回空 `coverPath` 的语义。
- 已完成：`PNG/JPEG/BMP` 代表样本回归不失败，运行 `TagReaderTest` 的结果均为非空 `coverPath`：
  - `/tmp/opencode/tagreader_stage6_4/id3v23_apic_png.mp3`
  - `/tmp/opencode/tagreader_stage6_4/id3v23_apic_jpeg.mp3`
  - `/tmp/opencode/tagreader_stage7_2/id3v23_apic_bmp.mp3`
- 已完成：本阶段验收过程中已运行 `./build/TagReaderTest` 覆盖上述 6 个样本，均无异常退出。
- 已确认：最近一次构建状态仍为通过；阶段 8.2 修改后已执行 `cmake --build build` 成功生成目标，本阶段 8.3/8.4 未新增代码改动，因此未产生新的构建失败风险。

## 阶段 9：放宽 MP4 `covr` 的可解码封面策略

### 9.1 现状确认

- 已确认 `ReadMP4DataAtom()` 当前实现位于 `src/TagReader.cpp:2896-3000`，其中 `covr` 分支位于 `src/TagReader.cpp:2979-2999`。
- 已确认当前 `covr` 路径会先调用 `DetectImageFormat(payload, payloadSize)`；如果签名不是当前已识别的 `PNG/JPEG/BMP/WEBP/GIF/TIFF` 之一，就会在进入 `WriteCoverAsPng()` 之前直接返回。
- 已确认当前 `covr` 路径仍然保留额外的 `dataType` 限制：
  - `dataType == 13` 且签名识别结果为 `JPEG`
  - 或 `dataType == 14` 且签名识别结果为 `PNG`
- 已确认这意味着即使 payload 本身是当前图像后端可解码的图片，只要：
  - `dataType` 不是 `13/14`，或
  - `dataType` 与实际图片签名不一致，或
  - 图片格式是 `BMP/WEBP/GIF/TIFF` 这类当前 `WriteCoverAsPng()` 已支持但 `covr` 白名单未放行的格式，
  当前代码都会提前跳过，不会尝试导出封面。
- 已确认这与阶段 8 后的设计目标“任意可解码图像统一转 PNG”不一致，因为 `WriteCoverAsPng()` 已能处理 `BMP/WEBP/GIF/TIFF`，但 MP4 `covr` 入口仍只允许 `JPEG/PNG` 通过。
- 已确认阶段 7 的样本 `/tmp/opencode/tagreader_stage7_2/m4a_covr_bmp.m4a` 当前仍输出空 `coverPath`，其直接原因正是这里的 `DetectImageFormat + dataType==13/14` 双重提前过滤，而不是后续图像转 PNG 路径本身失败。

### 9.2 修改策略

- 已完成：`ReadMP4DataAtom()` 的 `covr` 分支已改为先检查 `payload` 非空且 `payloadSize > 0`，然后直接把原始字节交给 `WriteCoverAsPng()` 尝试解码。
- 已完成：`dataType` 不再作为 `covr` 的强制准入条件；当前实现不再要求 `13 -> JPEG`、`14 -> PNG` 这类白名单匹配后才进入封面导出路径。
- 已完成：对 `dataType` 与实际签名不一致、或 `dataType` 为 `0` / 其他值的 `covr` payload，只要 `WriteCoverAsPng()` 最终能够识别并解码成功，当前实现就会导出 PNG。
- 已完成：如果 `covr` payload 本身为空、不是图片、图像 decoder 缺失或解码失败，当前实现仍保持安全失败语义：不崩溃，且 `coverPath` 保持为空。
- 已确认：阶段 9.2 只放宽了 MP4 `covr` 的入口准入条件，没有修改 `WriteCoverAsPng()` 的公共行为，也没有改变 ID3 / FLAC / Ogg 等其他封面来源的读取策略。
- 已完成：本阶段代码修改后已运行 `cmake --build build`，构建通过。

### 9.3 构建样本

- 已完成：在 `/tmp/opencode/tagreader_stage9_4` 生成阶段 9 样本目录。
- 已完成：复用并拷贝阶段 7 的 `base.m4a` 作为带真实音频流的 MP4 底座：`/tmp/opencode/tagreader_stage9_4/base.m4a`。
- 已完成：使用 `ffmpeg` 生成最小 PNG 封面样本 `cover.png`，尺寸为 `16x16`。
- 已完成：使用 `ffmpeg` 生成最小 JPEG 封面样本 `cover.jpg`，尺寸为 `16x16`。
- 已完成：复用阶段 7 的 BMP 样本，拷贝得到 `cover.bmp`。
- 已完成：将 `cover.png` 作为 attached picture 写入 M4A，生成 MP4 `covr` PNG 样本 `m4a_covr_png.m4a`。
- 已完成：将 `cover.jpg` 作为 attached picture 写入 M4A，生成 MP4 `covr` JPEG 样本 `m4a_covr_jpeg.m4a`。
- 已完成：将 `cover.bmp` 作为 attached picture 写入 M4A，生成 MP4 `covr` BMP 样本 `m4a_covr_bmp.m4a`。
- 已完成：基于 `m4a_covr_png.m4a` 派生 `dataType` 与实际签名不一致但 payload 可解码的样本 `m4a_covr_png_datatype13.m4a`：
  - 保留实际 PNG payload 不变。
  - 将 `covr/data` atom 的 `dataType` 从 PNG 常见值 `14` 改写为 JPEG 常见值 `13`。
- 已完成：基于 `m4a_covr_png.m4a` 派生 MP4 `covr` 非图像 payload 样本 `m4a_covr_not_image.m4a`：
  - 保留 `covr/data` atom 的整体结构不变。
  - 将 payload 起始 PNG 签名字节 `89 50 4E 47 0D 0A 1A 0A` 改写为 `NOTIMAGE`，使其不再是有效 PNG 图像。
- 已确认：本阶段样本构建只覆盖阶段 9 的 `covr` 准入放宽验证，不尝试同时覆盖封面类型优先级；front cover 规则仍留给阶段 10 单独回归。

### 9.4 验收标准

- 已完成：PNG `covr` 样本 `/tmp/opencode/tagreader_stage9_4/m4a_covr_png.m4a` 运行 `TagReaderTest` 后输出非空 `coverPath`：`/tmp/m4a_covr_png_-4658296646432822307_0.png`。
- 已完成：JPEG `covr` 样本 `/tmp/opencode/tagreader_stage9_4/m4a_covr_jpeg.m4a` 运行 `TagReaderTest` 后输出非空 `coverPath`：`/tmp/m4a_covr_jpeg_-4658296646405241186_0.png`。
- 已完成：BMP `covr` 样本 `/tmp/opencode/tagreader_stage9_4/m4a_covr_bmp.m4a` 运行 `TagReaderTest` 后输出非空 `coverPath`：`/tmp/m4a_covr_bmp_-4658296646376779726_0.png`；这说明阶段 7 已打通的 BMP -> PNG 路径在放宽 `covr` 准入后已经真正对 MP4 生效。
- 已完成：`dataType` 与实际图像签名不一致但 payload 可解码的样本 `/tmp/opencode/tagreader_stage9_4/m4a_covr_png_datatype13.m4a` 运行 `TagReaderTest` 后仍输出非空 `coverPath`：`/tmp/m4a_covr_png_datatype13_-4658296646350253164_0.png`。
- 已完成：上述 4 个导出文件都已通过 `file` 检查，确认文件内容均为有效 PNG：
  - `m4a_covr_png_*` -> `PNG image data, 16 x 16, 8-bit/color RGB, non-interlaced`
  - `m4a_covr_jpeg_*` -> `PNG image data, 16 x 16, 8-bit/color RGB, non-interlaced`
  - `m4a_covr_bmp_*` -> `PNG image data, 8 x 8, 8-bit/color RGB, non-interlaced`
  - `m4a_covr_png_datatype13_*` -> `PNG image data, 16 x 16, 8-bit/color RGB, non-interlaced`
- 已完成：非图像 payload 样本 `/tmp/opencode/tagreader_stage9_4/m4a_covr_not_image.m4a` 运行 `TagReaderTest` 后未崩溃，且输出 `coverPath:` 为空，符合“安全失败、不导出”的预期。
- 已完成：阶段 9.4 验收过程中已运行 `./build/TagReaderTest` 覆盖上述 5 个样本，均无异常退出。
- 已确认：最近一次构建状态仍为通过；阶段 9.2 代码修改后已执行 `cmake --build build` 成功生成目标，本阶段 9.3/9.4 未新增代码改动，因此未产生新的构建失败风险。

## 阶段 10：封面 front cover 规则回归

### 10.1 现状确认

- 已确认 `ReadID3v22PictureFrame()` 当前实现位于 `src/TagReader.cpp:2263-2298`，其中会读取 `pictureType = frameData[4]`，并在 `pictureType != 3` 时直接返回；因此 ID3v2.2 `PIC` 当前只接受 front cover。
- 已确认 `ReadID3v2PictureFrame()` 与 `ReadID3v2ApicPayload()` 当前实现位于 `src/TagReader.cpp:2360-2424`，其中：
  - `ReadID3v2PictureFrame()` 在读出 `pictureType` 后会先做一次 `pictureType != 3` 提前返回。
  - `ReadID3v2ApicPayload()` 入口也再次要求 `pictureType == 3`。
  因此 ID3v2.3 / ID3v2.4 `APIC` 当前只接受 front cover。
- 已确认 `ReadFlacPictureEntry()` 当前实现位于 `src/TagReader.cpp:2674-2749`，其中在解析出 `pictureType` 后会在 `pictureType != 3` 时直接返回；因此 FLAC `PICTURE` 当前只接受 front cover。
- 已确认 MP4 `covr` 路径当前实现位于 `ReadMP4DataAtom()` 的 `atomType == "covr"` 分支；该路径直接消费 `covr/data` payload 并交给 `WriteCoverAsPng()`，没有单独的 picture type 字段，也不存在 front/back cover 的协议级区分。
- 已确认当前 `RawMetadata` 只有单个 `coverPath` 输出位，没有额外的 cover candidate 列表或优先级状态；因此现有 front cover 规则主要依赖各格式解析分支在进入 `WriteCoverAsPng()` 前做 `pictureType == 3` 过滤，而不是在更高层统一仲裁。

### 10.2 回归测试

- 已完成：在 `/tmp/opencode/tagreader_stage10_3` 生成阶段 10 样本目录。
- 已完成：复用现有最小音频底座并准备封面素材：
  - `base.mp3`
  - `base.flac`
  - `front.png`
  - `back.jpg`
- 已完成：构建 ID3v2.3 `APIC` 非 front cover 样本 `id3v23_apic_back_only.mp3`：
  - 仅包含一张 back cover 图片。
  - 使用 `comment="Cover (back)"` 写入，供 ffmpeg 生成非 front cover APIC。
- 已完成：构建 ID3v2.3 `APIC` front cover 对照样本 `id3v23_apic_front_only.mp3`：
  - 仅包含一张 front cover 图片。
  - 使用 `comment="Cover (front)"` 写入。
- 已完成：构建同一文件内“先非 front cover、后 front cover”的双 `APIC` 样本 `id3v23_apic_back_then_front.mp3`：
  - 第一个图片流为 back cover。
  - 第二个图片流为 front cover。
- 已完成：构建同一文件内“先 front cover、后非 front cover”的双 `APIC` 样本 `id3v23_apic_front_then_back.mp3`：
  - 第一个图片流为 front cover。
  - 第二个图片流为 back cover。
- 已完成：构建 FLAC `PICTURE` 非 front cover 样本 `flac_picture_back_only.flac`：
  - 基于 `base.flac` 派生。
  - 使用 `metaflac --import-picture-from` 写入 `picture type = 4`（back cover）图片块。
- 已确认：本阶段 10.2 只负责样本构建，不在此处提前写入 10.3 的行为结论；实际“是否导出/是否只取 front cover”将通过下一阶段验收命令确认。

### 10.3 验收标准

- 已完成：ID3v2.3 `APIC` 非 front cover 样本 `/tmp/opencode/tagreader_stage10_3/id3v23_apic_back_only.mp3` 运行 `TagReaderTest` 后输出 `coverPath:` 为空，说明非 front cover 不导出。
- 已完成：FLAC `PICTURE` 非 front cover 样本 `/tmp/opencode/tagreader_stage10_3/flac_picture_back_only.flac` 运行 `TagReaderTest` 后输出 `coverPath:` 为空，说明 FLAC 非 front cover 同样不导出。
- 已完成：front cover 对照样本 `/tmp/opencode/tagreader_stage10_3/id3v23_apic_front_only.mp3` 运行 `TagReaderTest` 后输出非空 `coverPath`：`/tmp/id3v23_apic_front_only_-4658294336320516959_0.png`。
- 已完成：同一文件内“先非 front cover、后 front cover”的样本 `/tmp/opencode/tagreader_stage10_3/id3v23_apic_back_then_front.mp3` 运行 `TagReaderTest` 后输出非空 `coverPath`：`/tmp/id3v23_apic_back_then_front_-4658294336294583175_0.png`。
- 已完成：同一文件内“先 front cover、后非 front cover”的样本 `/tmp/opencode/tagreader_stage10_3/id3v23_apic_front_then_back.mp3` 运行 `TagReaderTest` 后输出非空 `coverPath`：`/tmp/id3v23_apic_front_then_back_-4658294336269624041_0.png`。
- 已完成：上述 3 个导出文件都已通过 `file` 检查，确认内容均为 `PNG image data, 16 x 16, 8-bit/color RGB, non-interlaced`，说明导出文件统一为 PNG。
- 已完成：通过 `sha256sum` 比对，`front_only`、`back_then_front`、`front_then_back` 三个导出 PNG 的哈希完全一致：
  - `8d860fa9497a88f3765a59b96fa1be357d6096087e73a789e803654827dea49c`
  这说明在存在 front cover 的情况下，当前实现确实只导出 front cover；非 front cover 既不会单独导出，也不会覆盖 front cover。
- 已完成：阶段 10.3 验收过程中已运行 `./build/TagReaderTest` 覆盖上述 5 个样本，均无异常退出。
- 已确认：最近一次构建状态仍为通过；本阶段 10.2/10.3 未新增代码改动，因此未引入新的构建失败风险。

## 阶段 11：边界与内存安全总检查

### 11.1 统一检查项

- 已完成：MP4 atom 路径此前已统一改为 `ReadMp4AtomHeader(...)` + `TryAddUintmax()`；本轮额外补齐了 `FLAC` 与 `Ogg` 文件偏移计算中的直接加法，把以下关键位置改成无溢出写法：
  - `ReadFlacMetadataBlocks()` 的 `cursor + 4`、`cursor + blockSize`
  - `ReadOggVorbisCommentEntries()` 的 `cursor + 27`、`cursor + 27 + segmentCount`、`... + payloadSize`
  现阶段高风险的文件级 `offset + size`、`cursor + atomSize`、`cursor + headerSize` 计算已统一收敛到 `TryAddUintmax()` 或等价先界限后加法的模式。
- 已完成：检查了当前 `std::vector` 构造点。高风险构造在进入前都已有范围约束：
  - `ReadRange()` 自身先检查 `offset`、`size`、`offset + size`
  - ID3 frame 切片前会先验证 `frameSize <= limit - cursor - headerSize`
  - MP4/FLAC/Ogg 的块读取前会先验证读取范围不超过文件边界
  - 图片包与编码包转 `std::vector<uint8_t>` 时，其来源长度来自 FFmpeg 已分配的 `AVPacket::size`
- 已完成：检查了 FFmpeg 资源生命周期。当前 `AVPacket`、`AVFrame`、`AVCodecContext`、`SwsContext` 均通过文件内 deleter + `std::unique_ptr` 管理，没有发现裸 `free` / `close` 漏洞路径。
- 已完成：检查了图片宽高与缓冲区分配防护。当前通用图片转 PNG 路径在解码后会验证：
  - `decodedFrame->width > 0`
  - `decodedFrame->height > 0`
  - `av_image_check_size(decodedFrame->width, decodedFrame->height, 0, nullptr) >= 0`
  并通过 `av_frame_get_buffer()` 申请 RGB 帧缓冲区；失败即安全返回空结果。
- 已完成：检查了标签解析的截断行为。当前 ID3、FLAC、Ogg、MP4 路径在遇到截断 header、截断 frame、截断 atom、截断图片描述区或 payload 越界时，均以 `return false`、`return` 或抛出明确 `runtime_error` 的方式停止解析，没有继续越界读取。
- 已完成：本阶段补丁修改后已运行 `cmake --build build`，构建通过。

### 11.2 构建损坏样本

- 已完成：在 `/tmp/opencode/tagreader_stage11_2` 生成阶段 11 损坏样本目录。
- 已完成：复用并准备基础底座：
  - `base.mp3`
  - `base.flac`
  - `base.ogg`
  - `base.m4a`
- 已完成：构建截断 ID3v2 tag 样本 `id3v23_tag_truncated.mp3`：
  - 基于带 `APIC` 的有效样本 `id3v23_apic_png_good.mp3` 派生。
  - 只保留文件前 40 字节，制造“ID3 header 仍在、tag payload 被截断”的场景。
- 已完成：构建截断 ID3v2.3 `APIC` 图片描述区样本 `id3v23_apic_desc_truncated.mp3`：
  - 基于有效 `APIC` 样本派生。
  - 缩短 `APIC` frame payload，使其停在 MIME/type 之后、缺少完整描述终止区与图像数据。
- 已完成：额外构建截断 ID3v2.2 `PIC` 图片描述区样本 `id3v22_pic_desc_truncated.mp3`：
  - 手工拼接最小 ID3v2.2 tag。
  - `PIC` frame 只包含 `encoding + image format + picture type + 未终止 description`，不包含图像字节。
- 已完成：构建截断 FLAC metadata block 样本 `flac_metadata_truncated.flac`：
  - 基于 `base.flac` 只保留前 30 字节，制造 metadata block 中途截断场景。
- 已完成：构建截断 FLAC `PICTURE` block 样本 `flac_picture_truncated.flac`：
  - 基于阶段 10 的 `flac_picture_back_only.flac` 派生。
  - 直接截断文件尾部 20 字节，制造图片块 payload 不完整场景。
- 已完成：构建 Ogg segment table 声明长度大于实际 payload 样本 `ogg_segment_payload_short.ogg`：
  - 基于 `base.ogg` 派生。
  - 修改首个 lacing value 为 `255`，同时裁掉文件尾部 10 字节，使段表声明长度大于实际可读 payload。
- 已完成：构建 MP4 atom size 小于 header size 样本 `m4a_atom_size_too_small.m4a`：
  - 基于 `base.m4a` 派生。
  - 将首个 atom 的 32-bit size 改为 `4`，小于标准 8-byte atom header。
- 已完成：构建 MP4 atom size 超出文件范围样本 `m4a_atom_size_exceeds_file.m4a`：
  - 基于 `base.m4a` 派生。
  - 将首个 atom 的 32-bit size 改为 `文件长度 + 4096`，制造 atom 跨越文件尾部场景。
- 已完成：复用已有损坏图片 payload 样本作为阶段 11.2 的“损坏图片 payload”覆盖：
  - `/tmp/opencode/tagreader_stage6_4/id3v23_apic_bad.mp3`
  - `/tmp/opencode/tagreader_stage9_4/m4a_covr_not_image.m4a`
  这两个样本分别覆盖“ID3 封面图像字节损坏”和“MP4 `covr` 非图像 payload”两类场景。

### 11.3 验收标准

- 已完成：阶段 11.3 验收过程中已运行 `./build/TagReaderTest` 覆盖以下 10 个损坏样本，均未发生崩溃：
  - `/tmp/opencode/tagreader_stage11_2/id3v23_tag_truncated.mp3`
  - `/tmp/opencode/tagreader_stage11_2/id3v23_apic_desc_truncated.mp3`
  - `/tmp/opencode/tagreader_stage11_2/id3v22_pic_desc_truncated.mp3`
  - `/tmp/opencode/tagreader_stage11_2/flac_metadata_truncated.flac`
  - `/tmp/opencode/tagreader_stage11_2/flac_picture_truncated.flac`
  - `/tmp/opencode/tagreader_stage11_2/ogg_segment_payload_short.ogg`
  - `/tmp/opencode/tagreader_stage11_2/m4a_atom_size_too_small.m4a`
  - `/tmp/opencode/tagreader_stage11_2/m4a_atom_size_exceeds_file.m4a`
  - `/tmp/opencode/tagreader_stage6_4/id3v23_apic_bad.mp3`
  - `/tmp/opencode/tagreader_stage9_4/m4a_covr_not_image.m4a`
- 已确认：本轮没有观察到越界读写迹象；所有样本的行为都收敛为“明确报错退出”或“安全跳过损坏字段继续返回 `MusicTag`”，没有出现卡死、崩溃或异常终止。
- 已完成：对容器级严重损坏，当前行为是由 FFmpeg 在 `avformat_open_input` 阶段明确失败，错误信息可解释：
  - `id3v23_tag_truncated.mp3` -> `TagReader error: avformat_open_input failed: Invalid data found when processing input`
  - `flac_metadata_truncated.flac` -> `TagReader error: avformat_open_input failed: Invalid data found when processing input`
  - `ogg_segment_payload_short.ogg` -> `TagReader error: avformat_open_input failed: End of file`
  - `m4a_atom_size_too_small.m4a` -> `TagReader error: avformat_open_input failed: Invalid data found when processing input`
  - `m4a_atom_size_exceeds_file.m4a` -> `TagReader error: avformat_open_input failed: Invalid data found when processing input`
- 已完成：对标签级或图片 payload 级损坏，当前行为是安全跳过损坏字段并继续返回可解释结果：
  - `id3v23_apic_desc_truncated.mp3` -> 读取成功，`coverPath:` 为空
  - `id3v22_pic_desc_truncated.mp3` -> 读取成功，`coverPath:` 为空
  - `flac_picture_truncated.flac` -> 读取成功，`coverPath:` 为空
  - `id3v23_apic_bad.mp3` -> 读取成功，`coverPath:` 为空
  - `m4a_covr_not_image.m4a` -> 读取成功，`coverPath:` 为空
- 已完成：上述结果符合当前设计语义：
  - 主容器/主结构已损坏到无法 probe 时，直接报错退出；
  - 单个标签块、图片块或图片 payload 损坏时，不让无关字段失败，损坏字段安全跳过。
- 已完成：阶段 11.3 验收后已重新运行 `cmake --build build`，输出 `ninja: no work to do.`，构建通过。

## 阶段 12：完整样本矩阵与最终验收

### 12.1 自建样本矩阵

- 已完成：整理并固化当前阶段样本矩阵；现有样本主要分布在 `/tmp/opencode/tagreader_stage1_4` 到 `/tmp/opencode/tagreader_stage11_2`。
- 已完成：多音轨容器矩阵已覆盖：
  - `single_track.wav`
  - `dual_track_default_second.mka`
  - `dual_track_default_second_diff_rate.mka`
  - `no_audio.mp4`
- 已完成：MP3 / ID3v2.2 样本矩阵已覆盖：
  - `id3v22_ult.mp3`
  - `id3v22_slt_ms.mp3`
  - `id3v22_slt_frames.mp3`
  - `id3v22_slt_truncated.mp3`
  - `id3v22_pic_desc_truncated.mp3`
  说明：当前矩阵已覆盖 `ULT`、`SLT`、`PIC` 损坏边界与 v2.2 歌词时间戳分支；常规文本/年份/genre 样本仍主要通过实现路径与损坏样本辅助覆盖。
- 已完成：MP3 / ID3v2.3 样本矩阵已覆盖：
  - 文本与 UTF-16 边界：
    - `id3v23_tit2_utf16_odd.mp3`
    - `id3v23_tit2_utf16le_bom.mp3`
    - `id3v23_tit2_utf16be.mp3`
  - `APIC` 多格式：
    - `id3v23_apic_png.mp3`
    - `id3v23_apic_jpeg.mp3`
    - `id3v23_apic_bmp.mp3`
    - `id3v23_apic_webp.mp3`
    - `id3v23_apic_gif.mp3`
    - `id3v23_apic_tiff.mp3`
  - `APIC` front/non-front 与顺序：
    - `id3v23_apic_back_only.mp3`
    - `id3v23_apic_front_only.mp3`
    - `id3v23_apic_back_then_front.mp3`
    - `id3v23_apic_front_then_back.mp3`
  - `APIC`/图片损坏边界：
    - `id3v23_apic_bad.mp3`
    - `id3v23_apic_desc_truncated.mp3`
    - `id3v23_tag_truncated.mp3`
  说明：当前矩阵对 ID3v2.3 的封面能力覆盖最完整；`USLT`、`SYLT`、普通 `TXXX`、歌词 `TXXX` 仍未单独构造专门样本文件名。
- 已完成：FLAC 样本矩阵已覆盖：
  - `flac_picture_bmp.flac`
  - `flac_picture_back_only.flac`
  - `flac_metadata_truncated.flac`
  - `flac_picture_truncated.flac`
  说明：当前矩阵已覆盖 front cover BMP、non-front cover、metadata block 截断、PICTURE block 截断；Vorbis Comment 常规字段与 `totaltracks/totaldiscs` 仍未单独拆出专名样本。
- 已完成：Ogg Vorbis 样本矩阵已覆盖：
  - `base.ogg`
  - `ogg_segment_payload_short.ogg`
  说明：当前矩阵已覆盖 Ogg 基础可读音频底座与 segment table 越界损坏场景；跨页 comment packet、lyrics、track/disc total 仍未单独构造命名样本。
- 已完成：MP4 / M4A 歌词样本矩阵已覆盖：
  - `m4a_lyrics_plain_real_audio.m4a`
  - `m4a_lyrics_item_largesize_real_audio.m4a`
  - `m4a_lyrics_data_largesize_real_audio.m4a`
  - `m4a_lyrics_item_largesize_truncated_real_audio.m4a`
  - `m4a_clyr_only.m4a`
  - `m4a_freeform_lyrics_only.m4a`
  - `m4a_freeform_nonlyrics.m4a`
  - `m4a_clyr_and_freeform.m4a`
- 已完成：MP4 / M4A `covr` 样本矩阵已覆盖：
  - `m4a_covr_png.m4a`
  - `m4a_covr_jpeg.m4a`
  - `m4a_covr_bmp.m4a`
  - `m4a_covr_png_datatype13.m4a`
  - `m4a_covr_not_image.m4a`
  - `m4a_atom_size_too_small.m4a`
  - `m4a_atom_size_exceeds_file.m4a`
- 已确认：当前矩阵已经覆盖阶段 12.2 中最关键的行为风险点：
  - best-stream 选择
  - UTF-16 边界
  - ID3v2.2 `SLT`
  - MP4 64 位歌词 atom
  - MP4 freeform lyrics
  - 多格式封面转 PNG
  - MP4 `covr` 放宽策略
  - front cover 规则
  - 主要损坏输入边界
- 已确认：`12.1` 当前已形成“可直接用于最终验收的样本索引矩阵”；后续 `12.2` 只需从这批样本中挑选代表样本执行最终回归，不必重新发明新的阶段性样本集。

### 12.2 最终验收标准

- 已完成：本轮最终回归已从阶段 1~11 的样本矩阵中挑选代表样本执行，覆盖了多音轨、UTF-16、ID3v2.2 `SLT`、MP4 64 位歌词 atom、MP4 freeform lyrics、多格式封面、MP4 `covr` 放宽、front cover 规则与损坏图片 payload。
- 已确认：文本字段继续来自文件原始字节解析路径，而不是 FFmpeg metadata 字典；本轮代表样本中：
  - `id3v23_tit2_utf16le_bom.mp3` -> `title: Valid LE`
  - `id3v23_tit2_utf16be.mp3` -> `title: Valid BE`
  说明 ID3 原始字节文本解码仍生效。
- 已确认：FFmpeg 当前仍只用于音频流定位、基础媒体信息和封面图像解码/PNG 编码；本轮样本中歌词、标题等非封面字段均继续走自实现解析路径。
- 已完成：主音频流选择符合 default/best stream 语义。样本 `/tmp/opencode/tagreader_stage1_4/dual_track_default_second_diff_rate.mka` 本轮输出 `sampleRate: 48000`、`bitRate: 768000`，说明最终仍选中默认第二音轨。
- 已完成：输出文本字段保持 UTF-8。样本 `id3v23_tit2_utf16le_bom.mp3`、`id3v23_tit2_utf16be.mp3` 本轮均输出正确 UTF-8 标题。
- 已确认：年份只输出年份数字这一规则在现有实现中保持不变；本轮代表样本未额外生成新的年份专门样本，但阶段 1~11 的实现检查与 `ParseYearOnly(...)` 路径未发生回退。
- 已完成：普通 MP4 freeform 字段不会误识别为歌词。样本 `/tmp/opencode/tagreader_stage5_4/m4a_freeform_nonlyrics.m4a` 本轮输出 `lyricsCount: 0`。
- 已完成：ID3v2.2 `SLT` milliseconds 能输出同步歌词。样本 `/tmp/opencode/tagreader_stage3_4/id3v22_slt_ms.mp3` 本轮输出 `lyricsCount: 2`。
- 已完成：MP4 64 位 atom size 歌词能读取。样本 `/tmp/opencode/tagreader_stage4_4/m4a_lyrics_item_largesize_real_audio.m4a` 本轮输出 `lyricsCount: 1`。
- 已完成：MP4 freeform lyrics 能读取。样本 `/tmp/opencode/tagreader_stage5_4/m4a_freeform_lyrics_only.m4a` 本轮输出 `lyricsCount: 2`。
- 已确认：`totaltracks` / `totaldiscs` 不污染当前 track / disc 编号这一实现规则在阶段 1~11 期间未被回退；本轮代表样本未新增专门的 `trkn/disk/total` 对照样本文件。
- 已完成：封面只导出 front cover。样本：
  - `/tmp/opencode/tagreader_stage10_3/id3v23_apic_back_only.mp3` -> `coverPath:` 为空
  - `/tmp/opencode/tagreader_stage10_3/id3v23_apic_back_then_front.mp3` -> 非空 `coverPath`
  - `/tmp/opencode/tagreader_stage10_3/id3v23_apic_front_then_back.mp3` -> 非空 `coverPath`
  并且 `front_only`、`back_then_front`、`front_then_back` 三个导出 PNG 的 `sha256` 完全一致，都是 `8d860fa9497a88f3765a59b96fa1be357d6096087e73a789e803654827dea49c`。
- 已完成：封面源格式为 `PNG/JPEG/BMP/WEBP/GIF/TIFF` 等本机可解码格式时，导出路径后缀为 `.png` 且文件内容为 PNG。本轮代表样本覆盖：
  - ID3 `APIC`：`id3v23_apic_webp.mp3`、`id3v23_apic_gif.mp3`、`id3v23_apic_tiff.mp3`
  - MP4 `covr`：`m4a_covr_png.m4a`、`m4a_covr_jpeg.m4a`、`m4a_covr_bmp.m4a`、`m4a_covr_png_datatype13.m4a`
  上述导出文件都已通过 `file` 检查，确认为 `PNG image data`。
- 已完成：不可解码封面安全跳过，不崩溃。样本：
  - `/tmp/opencode/tagreader_stage9_4/m4a_covr_not_image.m4a` -> `coverPath:` 为空
  - `/tmp/opencode/tagreader_stage6_4/id3v23_apic_bad.mp3` -> `coverPath:` 为空
- 已完成：最终回归后已重新运行 `cmake --build build`，输出 `ninja: no work to do.`，构建通过。

### 12.3 建议命令

## 阶段 13：媒体信息字段收尾修复

### 13.1 修改代码

- 已完成：修复 `ReadMediaInfo()` 的位深读取逻辑，新增 `DetectAudioBitDepth(...)`：
  - 优先读取 `codecpar->bits_per_raw_sample`
  - 其次读取 `codecpar->bits_per_coded_sample`
  - 不再把 MP3 这类有损编码的 `sample_fmt` 解码输出位宽误当成原始位深
- 已完成：上述策略下，MP3 若没有稳定原始位深信息则继续返回 `0`；FLAC 若 FFmpeg 已提供 `bits_per_raw_sample`，则能正确返回如 `24` 这类真实位深。
- 已完成：修复 `ReadMediaInfo()` 的格式名规范化逻辑，新增 `NormalizeFormatName(...)`：
  - 普通单一容器名保持不变
  - 对 `mov,mp4,m4a,3gp,3g2,mj2` 这类 FFmpeg 候选列表格式名，优先按文件扩展名收敛成单一格式名
  - 因此 `.m4a` 文件最终输出 `format: m4a`

### 13.2 真实样本验证

- 已完成：构建后用真实 MP3 样本验证：
  - `/home/kaizen857/Music/CloudMusic(for MP4)/cloudmusic/irucaice,ちょこ - Rainbow Rush Story.mp3`
  - 当前输出 `format: mp3`
  - 当前输出 `bitDepth: 0`
- 已完成：构建后用真实 FLAC 样本验证：
  - `/home/kaizen857/Music/CloudMusic(for MP4)/cloudmusic/Islet,倚水 - 星になる (feat. 倚水).flac`
  - 当前输出 `format: flac`
  - 当前输出 `bitDepth: 24`
- 已完成：构建后用真实 M4A 样本验证：
  - `/home/kaizen857/Music/Roselia/02. Ringing Bloom.m4a`
  - 当前输出 `format: m4a`
  - 当前输出 `bitDepth: 24`
- 已完成：本阶段修改后已运行 `cmake --build build`，构建通过。

- 如果没有 build 目录：`cmake -S . -B build`
- 构建：`cmake --build build`
- 单样本检查：`./build/TagReaderTest <audio-file-path>`
- 封面签名检查：读取导出文件前 8 字节，应为 `89 50 4E 47 0D 0A 1A 0A`。
