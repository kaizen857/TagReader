# TagReader 顶层模块编写计划

## 规划依据

- 当前代码已完整覆盖的基础标签族是 ID3v1/v2.2/v2.3/v2.4、FLAC/Ogg Vorbis Comment、MP4 `ilst`、APEv2。
- 后续扩展应优先复用现有核心 parser，把新增工作集中在“定位容器内标签区域”和“补齐少量缺失字段来源”。
- `docs/DESIGN.md` 中的最终目标不是当前能力，模块推进时应持续区分“已支持格式”和“目标格式”。

## 阶段 0：稳固现有基础能力

先保持并收敛现有 `id3`、`vorbis`、`flac`、`ogg-vorbis`、`mp4`、`ape` 六个格式模块，确保它们作为后续复用层足够稳定。这个阶段不追求新增后缀覆盖，重点是让现有解析、字段规范化、歌词、封面缓存和资源上限成为可靠公共底座。

## 阶段 1：补齐现有 parser 可直接覆盖的格式族

优先处理不需要全新标签语法的目标格式：`aac`、`m4a`、`alac`、`mpc`、`mp+`、`mpp`、`wv`、`tta`、`tak`、`shn`。这些格式的目标主要是让文件中已有的 ID3v2、ID3v1、APEv2 或 MP4 `ilst` 能被正确发现和分发。

这一阶段应优先完成，因为它能最大化复用当前代码，并快速扩大最终目标覆盖范围。

## 阶段 2：完善 Ogg/Opus 与 Vorbis Comment 系列

在当前 Ogg Vorbis Comment 基础上继续处理 `ogg` 的封面目标，并把 `opus` 纳入同一组推进。这个阶段仍以 Vorbis Comment/OpusTags 这类相近模型为中心，目标是补齐 Ogg/Opus 家族的元数据、歌词和封面覆盖。

这一阶段适合放在复用型后缀之后，因为它需要扩展现有 Ogg 路径，但还不需要像 ASF 或 Matroska 那样建立完全不同的元数据模型。

## 阶段 3：实现可复用 ID3 的容器提取器

随后处理 `wav`、`aiff`、`aif`、`dsf`、`dff`，以及依赖实际封装判断的 `dxd`。这些目标的共同点是需要先从容器结构中定位原生字段或内嵌 ID3v2，再尽量复用现有 ID3 和规范化链路。

这一阶段应先于 ASF/Matroska，因为它们虽然需要新增容器处理，但核心标签结果仍能大量复用现有 ID3 能力。

## 阶段 4：补齐需要新元数据模型的容器

再处理 `wma`、`mka`、`webm`。这类格式需要 ASF 或 Matroska/EBML 级别的元数据模型，和现有 ID3、Vorbis、MP4、APE 分支差异更大，适合在复用型扩展和 ID3 容器提取器稳定后推进。

这一阶段的目标是补齐独立容器元数据能力，而不是把它们强行折叠进已有格式 parser。

## 阶段 5：处理裸流和边界目标

最后统一处理 `dts`、`ac3`、`truehd` 这类裸音频流边界。根据设计文档，它们不规划独立标签 parser；如果需要标签，应通过 Matroska、MP4 等外层容器读取。

这一阶段主要用于明确检测、分发和文档边界，避免为没有标准标签来源的裸流新增孤立 parser。

## 总体优先级

1. 现有基础模块稳定化：ID3、Vorbis/FLAC/Ogg、MP4、APE。
2. 复用现有 parser 的后缀扩展：AAC/M4A/ALAC、MPC/MP+/MPP/WV/TTA/TAK/SHN。
3. Ogg/Opus 家族补齐：Ogg 封面、OpusTags/Comment 路径。
4. RIFF/IFF/DSF/DSDIFF 类容器提取：WAV、AIFF/AIF、DSF/DFF、DXD。
5. 新元数据模型容器：WMA/ASF、MKA/WebM/Matroska。
6. 裸流边界整理：DTS、AC3、TrueHD。
