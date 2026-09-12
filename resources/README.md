# resources — 应用资源

HuxerUI 资源根（`huxerui_add_app` 以 `app` 命名空间注册，codegen 生成
`app_resources.h` 常量）：`images/` 图标、`strings/` 本地化串。

## 水墨图标规范（新增图标必须遵守）

项目内所有 24×24 功能图标使用同一套“无色 alpha-mask + 水墨材质”语言：

- 语义轮廓必须在 16–20px 视觉范围内清楚可辨，水墨化不能牺牲功能识别；
- 资源统一只使用白色 `#FFFFFF` 与透明度描述墨量，不写死浅色／深色；运行时
  tint 根据主题映射成宣纸或玄墨颜色；
- 主笔画使用圆头、圆角连接，允许轻微不对称；避免整齐、等宽、完全闭合的
  Material 几何感；
- 每个语义图标只保留一个 SVG，禁止新增 `_selected.svg`、`_dark.svg`、
  `_light.svg` 等重复轮廓；选中态由承载底块表达，不更换图标文件；
- 品牌图标保留官方轮廓，只调整暖墨色、透明度与外围笔触，不重画商标结构；
- 太极、全景背景和卡片墨框属于独立插画资源，不套用 24×24 功能图标模板。

CMake 配置阶段会检查全部 24×24 SVG：包含硬编码颜色（非 `#FFFFFF`）或
存在 `_selected` 重复资源时直接报错。新增图标时应同时补充下方来源与许可表。

## 图标来源与许可

本次图标系统以仓库内的 `docs/icon-reference.png` 作为水墨图形参考，转换后的
24×24 SVG 只保留白色 alpha-mask；参考图本身不属于运行时资源，也不会随应用打包。

| 文件 | 来源 | 许可 |
|------|------|------|
| `claudecode.svg` | 本仓库自绘终端窗口 + 提示符（Claude Code 是 CLI） | 同本仓库 |
| `claude.svg` | Claude 官方 logo，[simple-icons](https://simpleicons.org) `claude`（官方轮廓不变，增加 alpha 水墨纹理） | CC0 1.0 |
| `codex.svg` | OpenAI 官方 logo，simple-icons `openai`（取自 release tag 15.1.0；官方轮廓不变，增加 alpha 水墨纹理） | CC0 1.0 |
| `opencode.svg` | opencode 官方 logo，simple-icons develop 分支 `opencode`（保留镂空规则与官方轮廓） | CC0 1.0 |
| `pi.svg` | pi-mono 官方 logo，[pi.dev](https://pi.dev) `logo-auto.svg`（等比缩到 24×24，官方轮廓不变） | MIT（[pi-mono 仓库](https://github.com/badlogic/pi-mono)） |
| `home/agents/providers/models/router/skills/mcp/sessions/stats/settings.svg` | `docs/icon-reference.png` 中的核心导航水墨图标（对应首页、Agent、供应商、模型、本地路由、Skills、MCP、会话、用量、设置） | 本仓库转换稿 |
| `trash/download/upload/search/refresh/add/edit/import/export/backup/restore.svg` | `docs/icon-reference.png` 中的常用操作水墨图标 | 本仓库转换稿 |
| `success/error/warning/info/loading/more/disabled/processing.svg` | `docs/icon-reference.png` 中的状态提示水墨图标 | 本仓库转换稿 |
| `user/api_key/link/options/logout.svg` | `docs/icon-reference.png` 中的辅助水墨图标 | 本仓库转换稿 |
| `about.svg` | 本仓库自绘圆圈 i（顶级「关于」入口） | 同本仓库 |
| `tray*.png` | 本仓库自绘太极图标的多倍率栅格版本 | 同本仓库 |
| `platform/windows/app.ico` | 由 `tray*.png` 生成的 Windows 应用、快捷方式和安装器图标 | 同本仓库 |
| `back/forward.svg` | 本仓库自绘（墨韵图标库·基础操作） | 同本仓库 |
| `file/folder/image/video/audio.svg` | 本仓库自绘（墨韵图标库·内容相关） | 同本仓库 |
| `group/message/bell/star/heart.svg` | 本仓库自绘（墨韵图标库·用户相关） | 同本仓库 |
| `help/lock/unlock.svg` | 本仓库自绘（墨韵图标库·状态提示） | 同本仓库 |
| `calendar/clock/location/filter/sort/menu.svg` | 本仓库自绘（墨韵图标库·其他常用） | 同本仓库 |
