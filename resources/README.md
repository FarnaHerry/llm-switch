# resources — 应用资源

HuxerUI 资源根（`huxerui_add_app` 以 `app` 命名空间注册，codegen 生成
`app_resources.h` 常量）：`images/` 图标、`strings/` 本地化串。

## 图标来源与许可

| 文件 | 来源 | 许可 |
|------|------|------|
| `agents.svg` / `agents_selected.svg` | 本仓库自绘机器人图标（顶级「Agent 管理」入口） | 同本仓库 |
| `claudecode.svg` / `claudecode_selected.svg` | 本仓库自绘终端窗口 + 提示符（Claude Code 是 CLI） | 同本仓库 |
| `claude.svg` / `claude_selected.svg` | Claude 官方 logo，[simple-icons](https://simpleicons.org) `claude`（品牌色改 #000000 配合运行时 tint；普通版 55% 不透明度区分选中态） | CC0 1.0 |
| `codex.svg` / `codex_selected.svg` | OpenAI 官方 logo，simple-icons `openai`（上游主分支已移除，取自 release tag 15.1.0；同改黑色 + 透明度变体） | CC0 1.0 |
| `opencode.svg` / `opencode_selected.svg` | opencode 官方 logo，simple-icons develop 分支 `opencode`（同改黑色 + 透明度变体，fill-rule evenodd 保留镂空） | CC0 1.0 |
| `pi.svg` / `pi_selected.svg` | pi-mono 官方 logo，[pi.dev](https://pi.dev) `logo-auto.svg`（等比缩到 24x24 + 黑色 + 透明度变体） | MIT（[pi-mono 仓库](https://github.com/badlogic/pi-mono)） |
| `settings.svg` / `settings_selected.svg` | 姊妹项目 Clash-Flux（Material 风格手绘） | 同本仓库 |
| `tray*.png` | 本仓库自绘双向箭头（SVG 源栅格化） | 同本仓库 |
