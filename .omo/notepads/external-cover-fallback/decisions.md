## 2026-06-21 Task: external-cover-fallback
- 共享 helper 放在 `src/cover/SidecarCover.*`，由 `ReadTag()` 统一调用；CUE 删除私有 sidecar 扫描逻辑，直接复用 `ReadTag()` 的 `coverPath` 结果。
- `ExportSidecarCover()` 只接受已解析、已校验的非空 `coverExportDir`；默认导出目录的选择与硬化仍由 `ReadTag()` 调用方负责。
- Sidecar 失败边界测试覆盖 malformed、oversized、symlink、no-candidate、invalid export dir，保证局部跳过与 `cover export`/`cover cache` 错误传播边界不漂移。
