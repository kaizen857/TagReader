# Task 1 baseline gaps

## 真实样本覆盖缺口

- 当前仓库内没有真实 `.mp3`、`.flac`、`.ogg`、`.m4a`、`.mp4` 音频样本，因此无法建立真实音频文件的跨格式 stdout baseline。
- 生成 corpus 不是完整真实音频覆盖：多数 seed 只包含元数据结构或畸形边界，不能替代带实际音频 stream、真实 encoder/container 写法、真实封面和真实歌词的样本。
- MP3/ID3：生成 seed 全部在 `avformat_open_input` 阶段失败，没有 `TagReaderTest` stdout。
- Ogg：生成 seed 全部在 `avformat_open_input` 阶段失败，没有 `TagReaderTest` stdout。
- MP4/M4A：生成 seed 多数进入容器但没有音频 stream，当前入口返回 `no audio stream found in input file`，没有 `TagReaderTest` stdout。
- lyrics：虽然 corpus 包含 ID3/Ogg/MP4 lyrics seed，但没有成功通过 `TagReaderTest` 入口的 lyrics stdout。
- cover：仅 FLAC PICTURE generated seed 成功形成 stdout 和导出 PNG；ID3 APIC 与 MP4 covr generated seed 未形成 runnable stdout。

## 已保留的 fallback 价值

- FLAC generated seed 有 8 个成功 stdout，可用于后续比较当前 FLAC/Vorbis comment 与 FLAC PICTURE 边界行为。
- `tagreadertest-runs.json` 保留 65 个失败 seed 的 stderr，可作为入口失败策略基线，但不应被标记为格式功能通过。

## 后续需要补齐

- 增加仓库内可提交或可生成的真实 MP3、FLAC、Ogg Vorbis、M4A 样本。
- 对至少一个真实封面样本和一个真实歌词样本建立 `TagReaderTest` stdout。
- 如果继续使用生成样本，应补充能通过 FFmpeg probe 且包含音频 stream 的 deterministic 样本生成方式。
