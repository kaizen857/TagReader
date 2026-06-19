# Task 1 baseline index

生成时间：2026-05-31。当前仓库内未发现真实 `.mp3`、`.flac`、`.ogg`、`.m4a` 或 `.mp4` 样本；因此已运行 `python3 test/corpus/generate_corpus.py` 生成 fallback corpus 到 `/tmp/opencode/tagreader_fuzz_corpus`。注意：生成 corpus 只用于刻画当前入口在缺少真实音频样本时的行为，不等价于完整真实音频覆盖。

## 构建基线

| 命令 | 退出状态 | 备注 |
| --- | ---: | --- |
| `cmake -S . -B build` | 0 | CMake configure 通过。 |
| `cmake --build build` | 0 | 构建 `TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke` 通过。 |
| `python3 test/corpus/generate_corpus.py` | 0 | 输出 `/tmp/opencode/tagreader_fuzz_corpus`；类别计数：encoding 6、flac 10、id3 24、image 3、mp4 19、ogg 11。 |

## 覆盖索引

| 维度 | 状态 | sample path | command | notes |
| --- | --- | --- | --- | --- |
| MP3 / ID3 | generated + failed | `/tmp/opencode/tagreader_fuzz_corpus/id3/*.mp3 与 /tmp/opencode/tagreader_fuzz_corpus/encoding/*.mp3` | `./build/TagReaderTest <sample>` | 仓库内无真实 MP3；生成 MP3/ID3 seed 共 29 个，全部返回 2；典型错误：TagReader error: avformat_open_input failed: Invalid data found when processing input |
| FLAC | generated + covered | `/tmp/opencode/tagreader_fuzz_corpus/flac/*.flac` | `./build/TagReaderTest <sample>`；图片相关样本附加 `.omo/evidence/baseline/covers` | 仓库内无真实 FLAC；生成 FLAC seed 10 个，其中 8 个 runnable 且已保存 stdout，2 个失败。 |
| Ogg Vorbis | generated + failed | `/tmp/opencode/tagreader_fuzz_corpus/ogg/*.ogg` | `./build/TagReaderTest <sample>` | 仓库内无真实 Ogg；生成 Ogg seed 11 个，全部返回 2；典型错误：TagReader error: avformat_open_input failed: End of file |
| MP4 / M4A | generated + failed | `/tmp/opencode/tagreader_fuzz_corpus/mp4/*.m4a 与 encoding/mp4_utf16_title_over_limit.m4a` | `./build/TagReaderTest <sample>` | 仓库内无真实 MP4/M4A；生成 MP4/M4A seed 20 个，全部返回 2；典型错误：TagReader error: no audio stream found in input file |
| cover | generated + partial | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac` | `./build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac .omo/evidence/baseline/covers` | FLAC PICTURE seed 返回 0，并导出 content-addressed PNG；ID3 APIC 与 MP4 covr seed 因没有可 probe 音频流返回 2。 |
| lyrics | generated + failed | `ID3/Ogg/MP4 lyrics seeds under /tmp/opencode/tagreader_fuzz_corpus` | `./build/TagReaderTest <sample>` | 生成 lyrics seed 存在，但 MP3/Ogg/MP4 lyrics seed 均未通过当前入口的 FFmpeg probe；没有真实 lyrics 音频 stdout 基线。 |
| malformed input | generated + failed | `truncated/oversized/deep/resource-limit seeds under /tmp/opencode/tagreader_fuzz_corpus` | `./build/TagReaderTest <sample>` | 畸形输入按 corpus 全量尝试执行；失败均记录在 tagreadertest-runs.json，成功的 FLAC 畸形 picture 边界 stdout 已保存。 |

## Runnable 样本 stdout

以下样本均已执行 `TagReaderTest` 且返回 0；stdout 已保存为 `.omo/evidence/baseline/*.stdout`。

| 来源 | sample path | command | stdout | notes |
| --- | --- | --- | --- | --- |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_desc_truncated.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_desc_truncated.flac /home/kaizen857/cppProject(app_and_lib)/TagReader/.omo/evidence/baseline/covers` | `.omo/evidence/baseline/flac_flac_picture_desc_truncated.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_image_truncated.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_image_truncated.flac /home/kaizen857/cppProject(app_and_lib)/TagReader/.omo/evidence/baseline/covers` | `.omo/evidence/baseline/flac_flac_picture_image_truncated.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_mime_truncated.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_mime_truncated.flac /home/kaizen857/cppProject(app_and_lib)/TagReader/.omo/evidence/baseline/covers` | `.omo/evidence/baseline/flac_flac_picture_mime_truncated.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac /home/kaizen857/cppProject(app_and_lib)/TagReader/.omo/evidence/baseline/covers` | `.omo/evidence/baseline/flac_flac_picture_valid.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_valid_chain.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_valid_chain.flac` | `.omo/evidence/baseline/flac_flac_valid_chain.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_key_then_title.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_key_then_title.flac` | `.omo/evidence/baseline/flac_flac_vorbis_invalid_key_then_title.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_lyrics_then_title.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_lyrics_then_title.flac` | `.omo/evidence/baseline/flac_flac_vorbis_invalid_lyrics_then_title.flac.stdout` | 返回 0 |
| generated | `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_value_then_artist.flac` | `/home/kaizen857/cppProject(app_and_lib)/TagReader/build/TagReaderTest /tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_value_then_artist.flac` | `.omo/evidence/baseline/flac_flac_vorbis_invalid_value_then_artist.flac.stdout` | 返回 0 |

## 全量执行记录

`TagReaderTest` 对 `/tmp/opencode/tagreader_fuzz_corpus` 下 73 个生成文件全部尝试执行：成功 8 个，失败 65 个。完整返回码、stderr 和 stdout 文件映射保存在 `.omo/evidence/baseline/tagreadertest-runs.json`。
