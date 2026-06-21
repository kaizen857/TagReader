## 2026-06-21 Task: external-cover-fallback
- `ctest` 需要在 `build/default/test` 和 `build/sanitize/test` 目录层级运行，才能稳定发现 Catch2 细粒度用例；根 `build/*` 层更适合总入口与非 Catch2 测试。
- `OneByOneJpeg()` 不适合作为 sidecar fallback 的稳定解码样本；Sidecar 回归改用 `OneByOnePng()` 以避免把图片编解码差异误判为 sidecar 逻辑失败。
- 当前仓库默认全量 `ctest --test-dir build/default/test --output-on-failure` 存在与本计划无关的 `TR-AUDIT-015..031`、`038..056` `BAD_COMMAND`/权限问题；本计划回归以 Sidecar/Cue 定向和 sanitize 定向为主要验收证据。
