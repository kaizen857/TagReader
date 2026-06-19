# Task 1 baseline summary

本任务在任何源码、公共头文件、CMake 或 docs 修改前建立 characterization baseline。未修改 `src/`、`include/`、`CMakeLists.txt` 或 `docs/`。

## 执行命令与状态

| 顺序 | 命令 | 退出状态 | 结果 |
| ---: | --- | ---: | --- |
| 1 | `cmake -S . -B build` | 0 | configure 通过，build files 写入 `build/`。 |
| 2 | `cmake --build build` | 0 | `TagReaderCore`、`TagReaderTest`、`TagReaderSecuritySmoke` 构建通过。 |
| 3 | repository-local glob for `**/*.mp3`, `**/*.flac`, `**/*.ogg`, `**/*.m4a`, `**/*.mp4` | 0 | 当前仓库无真实音频样本。 |
| 4 | `python3 test/corpus/generate_corpus.py` | 0 | 生成 fallback corpus 到 `/tmp/opencode/tagreader_fuzz_corpus`。 |
| 5 | `./build/TagReaderTest <sample>` over generated corpus | mixed | 73 个生成样本全部尝试；8 个返回 0 并保存 stdout，65 个返回 2。 |

## stdout evidence

成功运行的 stdout 文件位于 `.omo/evidence/baseline/`：

- `.omo/evidence/baseline/flac_flac_picture_desc_truncated.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_desc_truncated.flac`
- `.omo/evidence/baseline/flac_flac_picture_image_truncated.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_image_truncated.flac`
- `.omo/evidence/baseline/flac_flac_picture_mime_truncated.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_mime_truncated.flac`
- `.omo/evidence/baseline/flac_flac_picture_valid.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_picture_valid.flac`
- `.omo/evidence/baseline/flac_flac_valid_chain.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_valid_chain.flac`
- `.omo/evidence/baseline/flac_flac_vorbis_invalid_key_then_title.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_key_then_title.flac`
- `.omo/evidence/baseline/flac_flac_vorbis_invalid_lyrics_then_title.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_lyrics_then_title.flac`
- `.omo/evidence/baseline/flac_flac_vorbis_invalid_value_then_artist.flac.stdout` ← `/tmp/opencode/tagreader_fuzz_corpus/flac/flac_vorbis_invalid_value_then_artist.flac`

完整索引见 `.omo/evidence/baseline/index.md`；全量机器可读运行记录见 `.omo/evidence/baseline/tagreadertest-runs.json`。
