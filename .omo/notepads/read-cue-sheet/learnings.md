TODO 0 isolation complete: the worktree already contains unrelated planning/state files and a modified `.omo/boulder.json`, so later CUE work must stay inside the allowed families only.
Captured the current `git status --short` in `.omo/evidence/task-0-read-cue-sheet.txt` and locked the scope decision there for reuse by later todos.
TODO 2 result: added a dedicated `TagReaderCueCatch2Tests` target plus shared temp-artifact helpers that write under `std::filesystem::temp_directory_path()` only.
The new CUE sample bundle helper creates `audio.mp3`, `cover.jpg`, and `album.cue` in a temp workspace, so no generated binaries need to live under `test/`.
TODO 1 skeleton complete: `TagReader` now exposes `ReadCueSheet` batch overloads, while `Read()` remains scalar and forwards unchanged.
TR-AUDIT-056 now asserts `ReadCueSheet(path)` and `ReadCueSheet(path, coverExportDir)` return `std::vector<MusicTag>` and continues rejecting batch semantics on `Read()` plus `ReadAlbum`.
The old `HasReadCueMember` negative check still targets `ReadCue`, so the new API is covered without weakening the legacy boundary assertion.
TODO 3 is implemented via `tagreader_cue::LoadCueTextUtf8()`, which rejects non-regular inputs and files above 4 MiB before decode.
The CUE text loader reuses `tagreader_text::DecodeRawText()` and the current text codec chain, so UTF-8, UTF-8 BOM, UTF-16LE/BE BOM, and Latin-1 fallback all round-trip to UTF-8.
Focused CUE encoding tests now cover plain UTF-8, UTF-8 BOM, UTF-16LE/BE BOM, Latin-1 fallback, oversize rejection, undecodable UTF-16 rejection, and directory rejection.
# task-4 learnings
- CUE 解析建议独立成 `src/formats/cue/CueParser.*`，把 `global/file/track/index/REM` 状态限定在模块内部，测试通过相对路径 `../../src/formats/cue/CueParser.hpp` 访问即可，不需要暴露到 `include/`。
- 解析器需要按行计数做硬限流；本次边界覆盖应优先盯住 `FILE` / `TRACK` / `INDEX` 的层级转换、大小写不敏感命令、以及 `REM GENRE/DATE/YEAR/DISCNUMBER` 的保留字段。
- `INDEX` 数字解析要拒绝 frame=75、秒=60 以及超大整数溢出；字段长度也要单独限制，避免超长 `TITLE` / `PERFORMER` / `FILE` 名称进入后续阶段。
- TODO 5 先落一个私有 `CuePathResolver`，把 `FILE` 解析限制在 `.cue` 同目录下的普通文件；当前策略显式拒绝 empty/absolute/`..`/symlink/directory/non-regular/self-reference`，并为后续音频映射保留可测的 `CuePathResolutionStatus`。
- `cue_catch2_tests.cpp` 的路径测试已拆到 `cue_path_catch2_tests.cpp`，避免单文件超过 250 行；路径安全只依赖 `CuePathResolver`，解析语法测试继续独立。
- TODO 5 已提交为原子提交，后续 task 6 继续只在音频映射层展开。
- TODO 6 先在 `CueReader.cpp` 里做最小映射：`ReadTag()` 负责音频字段，CUE 只覆盖 `title/artist/composer/album/albumArtist/genre/year/discNumber/trackNumber`；普通 `Read()` 路径保持不变。
- 映射测试单独放到 `cue_mapping_catch2_tests.cpp`，覆盖 track/album 覆盖、audio field 保留、以及普通 `Read()` 非回归三条用例。
- TODO 7 的时间层要按 75fps 从 `INDEX 01` 推导 offset/duration；同文件内 duration 只依赖下一轨的 offset，最后一轨才回退到音频总时长，且 backward/invalid frame/overflow 直接让 `ReadCueSheet()` 失败。
- 时间测试单独放到 `cue_timing_catch2_tests.cpp`，覆盖 frame→microseconds、multi-track duration、multi-file 独立、以及非法/backward/overflow 时间拒绝。
- `CueReader.cpp` 的时间逻辑已拆到 `CueTiming.*`，保持 reader 负责编排、helper 负责时序计算；这样便于后续 task 8 在同一输出上叠加封面 fallback。
