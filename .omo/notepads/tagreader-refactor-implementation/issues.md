
## 2026-05-31 Task 1 baseline gaps

- 生成 corpus 不能等价于真实音频覆盖；MP3/Ogg/MP4/M4A generated seed 未形成成功 stdout，lyrics 也没有成功入口基线。
- 当前 cover stdout 只覆盖 FLAC PICTURE generated seed；ID3 APIC 与 MP4 covr generated seed 因缺少可 probe 音频流或音频 stream 失败。

## 2026-05-31 Task 4 extraction notes

- 未发现新的阻塞问题；baseline 覆盖仍继承 Task 1 限制，缺少成功入口的真实 MP3/Ogg/MP4/M4A/lyrics 样本。

## 2026-05-31 Task 5 extraction notes

- 未发现新的实现阻塞问题；cover 行为验证仍主要依赖 generated FLAC PICTURE seed，ID3 APIC 与 MP4 covr 成功入口样本缺口继承 Task 1 baseline 限制。

## 2026-05-31 Task 6 extraction notes

- Generated ID3/MP3 corpus entries still fail before ID3 parser execution through `TagReaderTest` because FFmpeg probe reports invalid data; no successful ID3 main-entrypoint sample exists in current corpus.
- Task 6 recovery exposed that prior Tasks 1-5 are uncommitted relative to git HEAD; do not use `HEAD:src/TagReader.cpp` as a post-Task-5 baseline.

## 2026-05-31 Task 7 extraction notes

- 未发现新的实现阻塞问题；baseline 仍只有 8 个成功 FLAC 入口，Ogg Vorbis generated seeds 仍在 FFmpeg probe 阶段失败，无法通过主入口覆盖 Ogg scanner 行为。

## 2026-05-31 Task 8 extraction notes

- 未发现新的实现阻塞问题；FLAC 迁移验证仍继承 Task 1 baseline 限制，只有 8 个 generated FLAC 成功入口且没有真实音频样本覆盖。
- `.omo/evidence/task-8-flac-diff.txt` 中的 git diff 相对当前 HEAD 会包含早期未提交 refactor 背景；判断 Task 8 时优先看本任务列出的 FLAC 文件、CMake 行和命令验证结果。

## 2026-05-31 Task 9 extraction notes

- 未发现新的实现阻塞问题；Ogg generated corpus 仍全部在 `avformat_open_input failed: End of file` 阶段失败，无法通过 public `TagReaderTest` 成功覆盖迁出的 Ogg scanner。
- Task 9 的 `git diff`/`git diff --stat` 相对 HEAD 仍会混入 Tasks 1-8 未提交背景；判断本任务时优先看 `src/formats/ogg-vorbis/OggVorbisParser.*`、`src/TagReader.cpp` Ogg dispatch、CMake 源列表、LSP/构建/基线证据。

## 2026-05-31 Task 10 extraction notes

- 未发现新的实现阻塞问题；MP4/M4A generated corpus 仍全部在 FFmpeg probe 或 `no audio stream found` 阶段失败，无法通过 public `TagReaderTest` 成功覆盖迁出的 MP4 parser。
- Task 10 的 `git diff`/`git diff --stat` 相对 HEAD 仍会混入 Tasks 1-9 未提交背景；判断本任务时优先看 `src/formats/mp4/Mp4AtomReader.*`、`src/formats/mp4/Mp4Parser.*`、`src/TagReader.cpp` MP4 dispatch、CMake 源列表和 `.omo/evidence/task-10-mp4-*.txt`。

## 2026-05-31 Task 11 extraction notes

- 未发现新的实现阻塞问题；成功入口 baseline 仍只有 8 个 generated FLAC 样本，ID3/Ogg/MP4/M4A dispatch 的成功 public-entry 覆盖缺口继承 Task 1 限制。
- Task 11 的 `git diff`/`git diff --stat` 相对 HEAD 仍会混入 Tasks 1-10 未提交背景；判断本任务时优先看 `src/core/TagPipeline.*`、`src/TagReader.cpp` facade、`CMakeLists.txt` 新源文件和 `.omo/evidence/task-11-*.txt`。

## 2026-05-31 Task 12 cleanup notes

- 未发现新的实现阻塞问题；本任务没有扩大 runnable baseline，仍继承只有 8 个 generated FLAC 成功入口、缺少真实 MP3/Ogg/MP4/M4A 样本的限制。
- Task 12 的 `git diff`/`git diff --stat` 相对 HEAD 仍会混入 Tasks 1-11 未提交背景；判断本任务时优先看 `CMakeLists.txt` 的 `TagReaderCore` 源列表、删除的零字节 scaffold 文件、public include 扫描和 `.omo/evidence/task-12-*.txt`。

## 2026-05-31 Task 13 final verification notes

- 未发现新的实现阻塞问题；Task 13 只记录最终本地验证证据，没有修改源码或扩大样本覆盖。
- 最终覆盖仍继承 Task 1 baseline 限制：成功 public-entry 可比较输出只有 8 个 generated FLAC 样本，仍不声称真实 MP3/Ogg/MP4/M4A、ID3 APIC、MP4 covr 或歌词成功入口覆盖。

## 2026-05-31 F1 remediation notes

- F1 的 media deliverable 缺口已通过真实 `src/media/*` 模块补齐，而不是改计划豁免；后续 final review 应检查 `TagPipeline.cpp` 不再重新吸收 FFmpeg/media helper。
- 覆盖缺口不变：本次只做等价搬迁和 evidence 修正，成功 public-entry stdout 仍限于 8 个 generated FLAC baseline，不应声称真实 MP3/Ogg/MP4/M4A 覆盖。
