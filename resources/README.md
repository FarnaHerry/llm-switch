# resources — 应用资源

HuxerUI 资源根（`huxerui_add_app` 以 `app` 命名空间注册，codegen 生成
`app_resources.h` 常量）：`images/` 图标、`strings/` 本地化串。

## 水墨图标规范（新增图标必须遵守）

项目内所有 24×24 功能图标使用同一套“可辨识轮廓 + 水墨材质”语言：

- 语义轮廓必须在 16–20px 视觉范围内清楚可辨，水墨化不能牺牲功能识别；
- 禁用纯黑 `#000000`，普通态用暖淡墨 `#57534A`，选中态用暖浓墨
  `#2E2B25`；运行时 tint 可以换色，但资源中的透明度层次必须保留；
- 主笔画使用圆头、圆角连接，允许轻微不对称；避免整齐、等宽、完全闭合的
  Material 几何感；
- 每个图标必须包含 `data-ink-texture="flywhite"` 的 1–3 个克制墨点，以及
  `data-ink-texture="dry-brush"` 的断续飞白收笔；纹理不能遮挡主体语义；
- 普通态纹理透明度约 0.16–0.32，选中态约 0.34–0.58；选中态依靠墨量，
  不依靠放大或改变图标含义；
- 品牌图标保留官方轮廓，只调整暖墨色、透明度与外围笔触，不重画商标结构；
- 太极、全景背景和卡片墨框属于独立插画资源，不套用 24×24 功能图标模板。

CMake 配置阶段会检查全部 24×24 SVG：缺少飞白/墨点标记或仍使用纯黑时直接
报错。新增图标时应同时补充下方来源与许可表。

## 图标来源与许可

| 文件 | 来源 | 许可 |
|------|------|------|
| `agents.svg` / `agents_selected.svg` | 本仓库自绘机器人图标（顶级「Agent 管理」入口） | 同本仓库 |
| `claudecode.svg` / `claudecode_selected.svg` | 本仓库自绘终端窗口 + 提示符（Claude Code 是 CLI） | 同本仓库 |
| `claude.svg` / `claude_selected.svg` | Claude 官方 logo，[simple-icons](https://simpleicons.org) `claude`（官方轮廓不变，增加暖墨透明度与外围飞白） | CC0 1.0 |
| `codex.svg` / `codex_selected.svg` | OpenAI 官方 logo，simple-icons `openai`（上游主分支已移除，取自 release tag 15.1.0；官方轮廓不变，增加暖墨纹理） | CC0 1.0 |
| `opencode.svg` / `opencode_selected.svg` | opencode 官方 logo，simple-icons develop 分支 `opencode`（保留镂空规则与官方轮廓，增加暖墨纹理） | CC0 1.0 |
| `pi.svg` / `pi_selected.svg` | pi-mono 官方 logo，[pi.dev](https://pi.dev) `logo-auto.svg`（等比缩到 24×24，官方轮廓不变，增加暖墨纹理） | MIT（[pi-mono 仓库](https://github.com/badlogic/pi-mono)） |
| `settings.svg` / `settings_selected.svg` | 姊妹项目 Clash-Flux（Material 风格手绘） | 同本仓库 |
| `router.svg` / `router_selected.svg` | 本仓库自绘双向交换箭头（顶级「本地路由」入口） | 同本仓库 |
| `stats.svg` / `stats_selected.svg` | 本仓库自绘柱状图（顶级「使用统计」入口） | 同本仓库 |
| `mcp.svg` / `mcp_selected.svg` | 本仓库自绘三方块连接（顶级「MCP 服务器」入口） | 同本仓库 |
| `skills.svg` / `skills_selected.svg` | 本仓库自绘魔法棒 + 星星（顶级「Skills」入口） | 同本仓库 |
| `sessions.svg` / `sessions_selected.svg` | 本仓库自绘时钟（顶级「会话」入口） | 同本仓库 |
| `about.svg` / `about_selected.svg` | 本仓库自绘圆圈 i（顶级「关于」入口） | 同本仓库 |
| `tray*.png` | 本仓库自绘太极图标的多倍率栅格版本 | 同本仓库 |
