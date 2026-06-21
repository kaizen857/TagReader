---
slug: external-cover-fallback
status: high-accuracy-reviewed
intent: clear
pending-action: write .omo/plans/external-cover-fallback.md
approach: 抽出 CUE sidecar cover fallback 为共享 cover helper，在 tagreader_core::ReadTag() 解析内嵌封面后、BuildMusicTag() 前统一补外部封面；CUE 删除重复 fallback 调用并继承统一行为。
---

# Draft: external-cover-fallback

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->
| C1 | 共享外部封面查找/导出 helper，行为与 CUE 当前逻辑一致 | active | src/formats/cue/CueReader.cpp:28, src/formats/cue/CueReader.cpp:129 |
| C2 | 普通格式 ReadTag 统一在无内嵌 coverPath 时补 sidecar cover | active | src/core/TagPipeline.cpp:609, src/core/TagPipeline.cpp:633 |
| C3 | CUE 迁移到共享 helper，避免双重 fallback 和行为漂移 | active | src/formats/cue/CueReader.cpp:310, src/formats/cue/CueReader.cpp:331 |
| C4 | 回归测试覆盖“无内嵌才外部、有内嵌优先、显式/默认导出、非 CUE 单文件” | active | test/regression/cue_catch2_tests.cpp:268, test/regression/catch2_sample_support.cpp:61 |
| R1 | 高精度审查修正：导出目录契约、CTest 注册、CUE 异常边界、非 MP3 代表格式验证 | active | .omo/plans/external-cover-fallback.md:39 |

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->
| 外部封面候选规则 | 完全复用 CUE 当前规则：同目录、非递归、普通文件、拒绝 symlink、名称 cover/front/folder/album/artwork、扩展 .png/.jpg/.jpeg/.bmp/.webp/.gif/.tiff、最多 256 候选、单文件 64 MiB | 用户明确要求“与 cue 分支中的处理方式一致” | 可逆 |
| 实现位置 | 放在 core pipeline 的 metadata 后处理，而不是每个格式 parser 内部 | 所有格式已经汇聚到 ReadTag()；逐 parser 修改会重复并易漂移 | 可逆 |
| CUE 行为 | CUE 不再单独二次查找；依赖 ReadTag() 返回的 coverPath | CUE 的 audioTag 来自 ReadTag()，统一后自然有 sidecar cover | 可逆 |
| 测试策略 | TDD：先加失败测试，再抽 helper/接 pipeline | 用户要求规划代码修改，行为边界清晰且已有 Catch2 回归模式 | 可逆 |
| 共享 helper 导出目录契约 | `ExportSidecarCover()` 要求传入非空、已校验的 `coverExportDir`；默认目录只能由 `ReadTag()` 或 CUE 入口预先解析 | 高精度审查指出“helper 不选默认目录”与“空目录返回空”的计划表述冲突 | 可逆 |
| “所有格式”验证代表样本 | MP3 作为普通单文件主样本，另加一个非 MP3 容器样本（推荐 FLAC 或 WAV）证明 core pipeline 接入不是 ID3 专属 | 高精度审查指出原计划过度 MP3-centric | 可逆 |

## Findings (cited - path:lines)
- 普通单文件入口为 `TagReader::Read()` → `tagreader_core::ReadTag()`：`src/TagReader.cpp:5`、`src/core/TagPipeline.cpp:609`。
- `ReadTag()` 当前顺序是校验路径/封面目录、探测流、探测格式、读 media info、读 metadata、读 lyrics、`BuildMusicTag()`：`src/core/TagPipeline.cpp:613`、`src/core/TagPipeline.cpp:633`、`src/core/TagPipeline.cpp:636`。
- `BuildMusicTag()` 只把 `metadata.coverPath` 填入输出 tag，不做外部封面补缺：`src/core/TagPipeline.cpp:533`、`src/core/TagPipeline.cpp:577`。
- CUE 当前的外部封面规则定义在 `CueReader.cpp` 私有常量和 `ExportCueSidecarCover()`：`src/formats/cue/CueReader.cpp:28`、`src/formats/cue/CueReader.cpp:36`、`src/formats/cue/CueReader.cpp:129`。
- CUE 外部封面查找只扫描音频同目录，跳过 symlink/非普通文件，按名称优先级排序并用 `WriteCoverAsPng()` 导出：`src/formats/cue/CueReader.cpp:141`、`src/formats/cue/CueReader.cpp:150`、`src/formats/cue/CueReader.cpp:198`。
- CUE 当前在 `audioTag.coverPath().empty()` 时调用 fallback，且 timing 后又有一次重复 fallback：`src/formats/cue/CueReader.cpp:310`、`src/formats/cue/CueReader.cpp:331`。
- 内嵌封面统一通过 `WriteCoverAsPng()` 写 content-addressed PNG cache：`src/cover/CoverCache.cpp:391`。
- 其它格式现有封面来源均为内嵌/容器内图片：ID3 APIC/PIC、FLAC PICTURE、Ogg/Opus METADATA_BLOCK_PICTURE、MP4 covr、APE binary cover、ASF WM/Picture、Matroska attachment；搜索命中均调用 `WriteCoverAsPng()`，没有 `directory_iterator` 外部图像查找。
- 测试辅助已有 `GenerateBaseMp3()`、`GenerateCoverSample()`、`OneByOnePng()`、`OneByOneJpeg()` 可复用：`test/regression/catch2_sample_support.cpp:7`、`test/regression/catch2_sample_support.cpp:61`、`test/regression/catch2_sample_support.cpp:79`。
- CUE 已有同目录外部封面 fallback 与内嵌优先测试，可作为行为基线：`test/regression/cue_catch2_tests.cpp:268`、`test/regression/cue_catch2_tests.cpp:293`。
- 构建中 `TagReaderCore` 源文件显式列在根 `CMakeLists.txt`，新增 `.cpp` 必须加入该列表：`CMakeLists.txt:71`、`CMakeLists.txt:80`、`CMakeLists.txt:98`。
- Catch2 测试目标在 `test/CMakeLists.txt` 显式注册，新增测试文件需加入现有或新增 target：`test/CMakeLists.txt:77`、`test/CMakeLists.txt:94`。
- 高精度审查发现必须修正四点：共享 helper 的导出目录所有权不能含糊；新增 Sidecar 测试必须在 `test/CMakeLists.txt` 中 `add_executable` + `catch_discover_tests()`；CUE 删除 fallback 后仍要保留 `cover export` / `cover cache` 抛出边界；至少增加一个非 MP3 代表格式来证明 core pipeline 行为覆盖所有格式。

## Decisions (with rationale)
- 新增共享模块建议命名为 `src/cover/SidecarCover.hpp` / `src/cover/SidecarCover.cpp`，导出 `std::optional<std::filesystem::path> ExportSidecarCover(const std::filesystem::path &audioPath, const std::filesystem::path &coverExportDir)`；放在 `cover/` 而不是 `formats/cue/`，因为行为将由 core 和 CUE 共享。
- `ExportSidecarCover()` 的契约必须明确为：`coverExportDir` 非空且 caller 已完成默认目录解析与安全校验；helper 不调用 `DefaultCoverExportDir()` / `ValidateCoverExportDir()` / `ValidateDefaultCoverExportDir()`，空目录应视为调用方错误并返回空或断言，但生产路径不得依赖空目录分支。
- `ReadTag()` 在 `RawMetadata metadata = ReadMetadata(...)` 后立即执行：若 `metadata.coverPath.empty()`，调用 `tagreader_cover::ExportSidecarCover(context.filePath, context.coverExportDir)`；成功则写回 `metadata.coverPath`，随后再 `ReadLyrics()` 与 `BuildMusicTag()`。
- CUE 删除私有 `kCueCoverNames`、`kCueCoverExtensions`、`EqualsIgnoreCase()`、`IsCueCoverFile()`、`ResolveCueCoverExportDir()`、`ExportCueSidecarCover()` 及两处 fallback 调用；保留 `audioTag` 复制到 track 的行为即可。
- 外部封面错误策略沿用当前 CUE：无匹配、读失败、解码失败返回空；`WriteCoverAsPng()` 抛出的 `cover export` / `cover cache` 继续向外传播，不吞掉。
- 不改变 public API；`Read(path)`、`Read(path, coverExportDir)` 只是返回的 `coverPath` 在无内嵌封面且同目录有候选图时变为非空。

## Scope IN
- 普通单文件 `TagReader::Read()` / `Read(path, coverExportDir)` 对所有格式增加外部封面 fallback。
- CUE 迁移到共享 sidecar cover helper，确保同一算法、同一候选规则、同一导出目录语义。
- 新增/更新 Catch2 回归测试，覆盖普通格式 sidecar fallback、内嵌优先、显式导出目录、候选优先级、symlink/非图片/oversized/malformed 跳过中的关键失败场景。
- 更新仓库事实文档/agent notes 中关于封面副作用的描述。

## Scope OUT (Must NOT have)
- 不新增 public API，不新增 `ReadAlbum`，不改变 `ReadCueSheet` 返回类型。
- 不递归扫描目录，不跨目录找图，不读取 CUE 文件所在目录作为普通格式封面来源；只用当前音频文件同目录。
- 不让外部封面覆盖内嵌封面。
- 不改各格式 parser 的元数据/歌词解析语义，不引入 FFmpeg `AVDictionary` 作为标题/歌手/专辑来源。
- 不放宽 cover cache 安全策略，不吞掉 `cover export` / `cover cache` 错误。

## Open questions
- 无阻塞问题。用户已明确要求“其他所有格式”“无内嵌时才处理”“与 CUE 一致”；实现位置、测试策略、CUE 去重均可由代码事实决定。

## Approval gate
status: approved-by-request
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
用户本轮明确要求“收集具体信息，根据要求以及具体信息编写完整的代码编写/修改计划”，因此本次直接生成计划文件；仍不执行产品代码修改。
