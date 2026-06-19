2026-06-01: 回归入口采用无框架 `TagReaderRegressionTests`，所有 TR-AUDIT-001..015 先注册为未实现；`--list` 可作为后续任务共享清单，单项未实现必须非零退出，避免误报通过。
2026-06-01: TR-AUDIT-001 产品修复中，`FindNextMp4SiblingAfterSizeZero()` 不再扫描 size-zero payload 内的伪 sibling；允许 size-zero 的 child walker 会在回调当前 atom 后停止当前 sibling 循环，不允许 size-zero 的 item/data 子层仍按 malformed 处理。
2026-06-01: TR-AUDIT-001 回归用例运行时用 ffmpeg 生成短 AAC base.m4a，再注入 `moov/udta/meta/ilst`；证据固定写到 `/tmp/opencode/tagreader_regression/TR-AUDIT-001/`，summary marker 为 `size-zero recovery disabled`。
2026-06-01: TR-AUDIT-002 产品修复将 Ogg logical stream 状态从 vector 线性查找改为 serial-keyed unordered_map，并用 256 条上限在新 serial 溢出时返回 not found；现有单 logical stream Vorbis comment 包装与 sequence/continuation 处理保持原流程。
2026-06-01: TR-AUDIT-002 回归可用 ffmpeg 生成真实 Ogg Vorbis 音频，再在文件前拼接 300 个合法 CRC 的小 Ogg 页触发 logical-stream-limit；证据 marker 同时包含 `logical-stream-limit` 和 `many serials rejected without quadratic scan`。
2026-06-01: TR-AUDIT-003 产品修复将 FLAC 与 Ogg Vorbis comment walker 共用 `tagreader_vorbis::kMaxVorbisComments=4096`，并在进入循环前要求剩余字节至少容纳所有 4 字节 length 前缀，超大或不可能数量按 malformed 返回。
2026-06-01: TR-AUDIT-003 回归中 Ogg 样本不能替换整页 comment packet，否则会丢失同页 setup packet 让 FFmpeg probe 失败；应原位 patch comment count/首条 comment 并重算 Ogg CRC，保持音频容器可读。
2026-06-01: TR-AUDIT-004 产品修复在 MP4 `covr` item 层发现已有 `metadata.coverPath` 时直接早退，配合 data 层既有防御，避免重复封面进入 child traversal 或读取后续 payload。
2026-06-01: TR-AUDIT-004 回归可复用 TR-AUDIT-001 的 M4A 注入 helper，构造两个独立 `covr/data` item：先放内嵌 1x1 PNG，再放 2MiB 无效 payload，并通过 PNG 数量与 mtime 证明重复封面未导出或重写缓存。
2026-06-01: TR-AUDIT-005 产品修复在 FLAC PICTURE metadata block 层发现已有 coverPath 时直接跳到 blockEnd，并在 ReadFlacPictureEntry() 加二级早退，避免重复封面读取大 payload 或进入解码。\n2026-06-01: TR-AUDIT-005 notepad 上一条含字面量反斜杠 n；有效结论不变：FLAC 重复 PICTURE 同时在 metadata block 层和 entry 层跳过。

2026-06-01: TR-AUDIT-005 回归用例可在 ffmpeg 生成的真实 FLAC 后重写 metadata 链：保留原 STREAMINFO/metadata，将原 last 标记清掉，再追加两个 PICTURE block，最后一个 PICTURE 设置 last，音频帧保持在 metadata 后。
2026-06-01: TR-AUDIT-006 产品修复在 ID3v2.2 与 v2.3/v2.4 metadata/lyrics walker 共用 malformed-frame resync：真实 padding 仍停止，非 padding 坏 header/零 size/越界 size/v2.4 非 syncsafe size 会从 cursor+1 查找下一个合法 frame header。
2026-06-01: TR-AUDIT-006 Atlas 复核指出 resync 扫描阶段不能把任意单个 0 字节当 padding；修正后只有原始 cursor 保留首字节 0 停止语义，扫描候选位置仅在剩余区域全 0 时停止。

2026-06-01: TR-AUDIT-006 回归样本用真实 ffmpeg MP3 音频作为尾部载体，再手写前置 ID3v2.3 tag；坏帧样本要在 junk 中放内部 0 字节验证 resync 不误停，padding 样本则用原始 cursor 的首个 0 验证真正 padding 立即停止。

2026-06-01: TR-AUDIT-007 产品修复只收紧 ID3 轨道/光盘数字解析：`ParseUInt16()` 在 `std::stoul()` 后要求完整消费 trimmed 字符串，`ParseSlashNumber()` 对 slash 两侧分别 trim 并在任一侧缺失或含垃圾时整组拒绝，避免 `12abc/7`、`003x/01` 这类前缀污染 current/total。

2026-06-01: TR-AUDIT-007 回归复用真实 ffmpeg MP3 尾部并手写前置 ID3 tag；v2.2 需本地 3 字节 frame id/24-bit size helper，v2.3 复用既有 helper，stdout/summary 同时保留 v22-strict、v23-strict 与 strict-number-parse marker。
2026-06-01: TR-AUDIT-008 产品修复将 LRC 数字解析改为乘加前先做 uint32 溢出前检查，超长 minute/second/fraction 直接拒绝；合法 `[01:02.34]`、`[123:45.678]` 与非 timestamp 方括号 plain lyrics 保持原行为。
2026-06-01: TR-AUDIT-008 回归用真实 ffmpeg MP3 尾部加前置 ID3v2.3 USLT，LRC 文本同时放合法 `[01:02.340]ok` 与超长 minute/second/fraction；断言 synced lyrics 只剩一个合法时间戳行并把样本/summary 固定写到 `/tmp/opencode/tagreader_regression/TR-AUDIT-008/`。
2026-06-01: TR-AUDIT-009 产品修复要求 Ogg Vorbis identification packet 至少 30 字节且校验 version/channels/sampleRate/framing flag，comment packet 至少包含 `0x03vorbis` 和 4 字节 vendor length 后才进入共享 Vorbis comment walker。
2026-06-01: TR-AUDIT-009 回归通过在真实 ffmpeg Ogg Vorbis 前拼接 CRC 正确的恶意 logical stream：identification prefix-only 直接拒绝，合法 identification 后 comment prefix-only 也必须在进入 Vorbis comment walker 前拒绝。
2026-06-01: TR-AUDIT-010 产品修复将 `ReadUtf16TextWithBom()` 的无 BOM 兜底从默认小端改为直接返回空串；因此 ID3 编码字节 `0x01` 只有在 `FF FE` 或 `FE FF` 明确存在时才解码，`0x02` 显式 UTF-16BE、`0x00` Latin-1 与 `0x03` UTF-8 行为保持不变。
2026-06-01: TR-AUDIT-010 回归复用真实 ffmpeg MP3 尾部和手写 ID3v2.3 TIT2；encoding=1 的 UTF-16LE/BE BOM 样本应解码为标题，无 BOM 偶数字节 payload 必须得到空标题并输出 bomless-rejected marker。

2026-06-01: TR-AUDIT-011 产品修复在 `ConvertImageToPng()` 中保存 `sws_scale()` 返回行数，只有 `scaledRows == decodedFrame->height` 时才继续 `EncodeFrameAsPng()`；负数或短行数统一返回空 PNG，尺寸/像素/input/output 限制和 codec 选择均未改动。
2026-06-01: TR-AUDIT-011 回归复用 MP4 `covr/data` helper 和内嵌 1x1 PNG，正常样本必须导出一个 PNG；坏样本使用带 PNG 签名但截断的 payload，要求 coverPath 为空、PNG 数不增加且缓存文件计数保持不变。
2026-06-01: TR-AUDIT-012 产品修复中，ID3v2.4 extended header 的 syncsafe size 按规范视为包含 4 字节 size 字段的总 extended header 长度；frame cursor 应直接前进到 `extSize`，同时保持 `extSize >= 6`、syncsafe 校验、flag bytes 边界校验和 footer 排除逻辑。
2026-06-01: TR-AUDIT-012 回归复用真实 ffmpeg MP3 尾部并手写 ID3v2.4 tag；v2.4 frame size 也必须用 syncsafe32，标准 extended header 可用 extSize=6、flagBytes=1、flags=0 后紧跟 TIT2。
2026-06-01: TR-AUDIT-012 malformed 样本要同时覆盖 extSize<6 和 extSize 超出 tag frameLimit；断言标题为空且不能通过 resync 解析错位的 `shifted-garbage` TIT2。
2026-06-01: TR-AUDIT-013 产品修复只替换封面缓存 content address：`HashEmbeddedImageBytes()` 改用 libavutil SHA-256 输出 64 位小写 hex，原有 `first2/rest.png` 分片和已有缓存字节级污染拒绝策略保持不变。
2026-06-01: TR-AUDIT-013 回归样本固定写到 /tmp/opencode/tagreader_regression/TR-AUDIT-013/cover_sample.mp3；先预污染 SHA-256 目标缓存路径验证 cover cache+path 诊断，再删除污染文件验证同图重复读取路径和 mtime 均复用。
2026-06-01: TR-AUDIT-014 产品修复删除 `ReadImageBytes()` 的 packet->vector->packet 双拷贝，`ConvertImageToPng()` 直接用输入 byte span 构造唯一解码 AVPacket；未知 magic fallback 明确限制为 PNG/JPEG 两次尝试。
2026-06-01: TR-AUDIT-014 回归用独立 cover 目录分别验证 1x1 PNG、ffmpeg 动态 JPEG 和 2MiB unknown-magic payload；malformed 分支要求 coverPath 为空且目录内 PNG 数为 0。
2026-06-01: TR-AUDIT-015 产品修复中，`MusicTag` 文本字段改为对象内 `std::string`，getter 返回 `const std::string&`；构建管线仍通过现有 setter 注入已规范化 UTF-8 文本，不引入共享字符串池。
2026-06-01: TR-AUDIT-015 回归用 8 个动态 ID3v2.3 MP3 样本和 16 个 std::async worker 验证 title/artist/album/composer 不串值，证据固定写到 /tmp/opencode/tagreader_regression/TR-AUDIT-015/。
