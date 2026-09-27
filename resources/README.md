# resources — 应用资源

HuxerUI 资源根（`huxerui_add_app` 以 `app` 命名空间注册，codegen 生成
`app_resources.h` 常量）：`images/` 图标、`strings/` 本地化串。

## raw/usage_templates.json — 官方用量查询模板表（数据文件，非图标）

厂商用量查询端点的官方默认表，随资源包发布（运行时经 `UseRawResource`
读取，键 `app::raw::usage_templates_json`），**不在 C++ 代码中硬编码**。
收录原则：只收厂商官方文档公开的端点，没把握的不编；每条目的 `source`
字段记录文档出处，修改/新增时必须同步核实并更新。

用户可在 `dataDir()/usage_templates.json`（设置页展示路径）放一份同格式
文件整体覆盖/扩展内置表。解析与匹配在 `llmswitch.models`
（`parseUsageTemplates` / `suggestUsageQuery`），仓库内这份文件由
`test_store` 每次校验可解析且已知厂商可命中。

## 水墨图标规范（新增图标必须遵守）

项目内所有 24×24 功能图标使用同一套“无色 alpha-mask + 水墨材质”语言：

- 语义轮廓必须在 16–20px 视觉范围内清楚可辨，水墨化不能牺牲功能识别；
- 资源统一只使用白色 `#FFFFFF` 与透明度描述墨量，不写死浅色／深色；运行时
  tint 根据主题映射成冷白或深石板蓝（见 `src/ui/app.cpp` 两套 ThemeSpec）；
- 主笔画使用圆头、圆角连接，允许轻微不对称；避免整齐、等宽、完全闭合的
  Material 几何感；
- 每个语义图标只保留一个 SVG，禁止新增 `_selected.svg`、`_dark.svg`、
  `_light.svg` 等重复轮廓；选中态由承载底块表达，不更换图标文件；
- 品牌图标保留官方轮廓，只调整暖墨色、透明度与外围笔触，不重画商标结构；
- 太极和全景背景属于独立插画资源，不套用 24×24 功能图标模板。

CMake 配置阶段会检查全部 24×24 SVG：包含硬编码颜色（非 `#FFFFFF`）或
存在 `_selected` 重复资源时直接报错。新增图标时应同时补充下方来源与许可表。

## 图标来源与许可

功能图标于 2026-09-19 整体换版：以用户提供的 **llm-switch Icon Set v1.0** 参考图
为准逐枚矢量化，转换后的 24×24 SVG 只保留白色 alpha-mask；参考图与转换稿本身
不属于运行时资源，也不会随应用打包。

被换下的旧图标（34 枚，`docs/icon-reference.png` 时期的转换稿）**原样留档在仓库
根目录 `old/`**，只作对照与回滚，不参与构建、不随应用打包。独立交付的完整图标集
（35 枚，含尚未接入运行时的实心莲花 `home_active.svg`，以及浅/深两套预览图）见
仓库根目录 `icon-set/`。

| 文件 | 来源 | 许可 |
|------|------|------|
| `claudecode.svg` | 本仓库自绘终端窗口 + 提示符（Claude Code 是 CLI） | 同本仓库 |
| `claude.svg` | Claude 官方 logo，[simple-icons](https://simpleicons.org) `claude`（官方轮廓不变，增加 alpha 水墨纹理） | CC0 1.0 |
| `codex.svg` | OpenAI 官方 logo，simple-icons `openai`（取自 release tag 15.1.0；官方轮廓不变，增加 alpha 水墨纹理） | CC0 1.0 |
| `opencode.svg` | opencode 官方 logo，simple-icons develop 分支 `opencode`（保留镂空规则与官方轮廓） | CC0 1.0 |
| `pi.svg` | pi-mono 官方 logo，[pi.dev](https://pi.dev) `logo-auto.svg`（等比缩到 24×24，官方轮廓不变） | MIT（[pi-mono 仓库](https://github.com/badlogic/pi-mono)） |
| `dsh.svg` | 本仓库自绘束带扣（DeepSeek Harness 的 harness 意象） | 同本仓库 |
| `hermes.svg` | 本仓库自绘飞翼（Hermes 的神使飞翼意象） | 同本仓库 |
| `home/home_active/agents/providers/models/router/skills/mcp/sessions/stats/settings.svg` | 用户提供的 **llm-switch Icon Set v1.0** 参考图中的核心导航图标（首页未选中／选中、Agent 管理、供应商、模型、本地路由、Skills、MCP、会话、用量、设置；`home_active.svg` 为实心莲花，当前未接入运行时，选中态仍由承载底块表达） | 本仓库转换稿 |
| `trash/download/upload/search/refresh/edit/import/export/backup/restore.svg` | Icon Set v1.0 中的常用操作图标（删除、下载、上传、搜索、刷新、编辑、导入、导出、备份、恢复） | 本仓库转换稿 |
| `add.svg` | 本仓库自绘的简单通用加号（圆头十字，墨迹 14/24）。Icon Set v1.0 的「卷宗 + 加号」在 16px 下已经认不出是新增，按产品要求换成通用 add 造型 | 同本仓库 |
| `success/error/warning/info/loading/more/disabled/processing.svg` | Icon Set v1.0 中的状态提示图标 | 本仓库转换稿 |
| `user/api_key/link/options/logout.svg` | Icon Set v1.0 中的辅助图标 | 本仓库转换稿 |
| `about.svg` | 本仓库自绘圆圈 i（顶级「关于」入口） | 同本仓库 |
| `lotus_bloom.svg` | 品牌莲花·盛开，直接采用 Icon Set v1.0 的莲花轮廓（与 `images/home.svg` 同一造型，仅 viewBox 表达不同）；用于标题栏、导航中心、托盘与关于页。含苞态（`lotus_bud.svg`）已整体废弃——托盘与导航盘都不再用"闭合"形态表达状态 | 本仓库转换稿 |
| `lotus_tray_bloom*.png` | 由盛放图稿生成的托盘多倍率栅格版本（深色圆底 `#14202E` + 冷白莲花，1x/2x/3x/4x/8x） | 同本仓库 |
| `platform/{linux,windows}` 应用图标 | 由盛放莲花图稿生成的 Linux SVG 与 Windows ICO（品牌底色 `#14202E` / 莲花 `#E8F0F8` 保持不变，仅换莲花造型） | 同本仓库 |
| `back/forward.svg` | 本仓库自绘（墨韵图标库·基础操作） | 同本仓库 |
| `file/folder/image/video/audio.svg` | 本仓库自绘（墨韵图标库·内容相关） | 同本仓库 |
| `group/message/bell/star/heart.svg` | 本仓库自绘（墨韵图标库·用户相关） | 同本仓库 |
| `help/lock/unlock.svg` | 本仓库自绘（墨韵图标库·状态提示） | 同本仓库 |
| `calendar/clock/location/filter/sort/menu.svg` | 本仓库自绘（墨韵图标库·其他常用） | 同本仓库 |

## 换图标后的生效范围（任务栏／托盘为什么不跟着变）

仓库里的 `resources/images/` 只决定**构建目录**里跑的应用（`./run.sh`）。装了包之后，
另外两处来源各自独立，改图标必须重新发布到它们才会生效：

- **任务栏／程序坞图标**：HuxerUI 目前没有窗口图标 API，Linux 下应用无法自报图标，
  只能由桌面条目解析——`StartupWMClass=llm-switch` 匹配
  `/usr/share/applications/llm-switch.desktop`，其 `Icon=llm-switch` 再指向
  `/usr/share/icons/hicolor/scalable/apps/llm-switch.svg`。这一份来自打包安装
  （`platform/linux/package/llm-switch.svg`），构建目录里改它是碰不到的。
- **应用内标题栏莲花与托盘图标**：来自已安装的 `llm-switch.resources` 资源包，
  同样要重装才更新。

因此改完图标后按需二选一：

```bash
# 只让任务栏立刻跟上（最小改动）
sudo install -m 644 platform/linux/package/llm-switch.svg \
     /usr/share/icons/hicolor/scalable/apps/llm-switch.svg

# 走正常发布渠道（同时更新二进制、资源包、桌面条目与图标）
cmake -S . -B build-rpm -G Ninja -DCMAKE_BUILD_TYPE=Release -DHUXERUI_PACKAGE=ON
cmake --build build-rpm --parallel 4
cpack --config build-rpm/CPackConfig.cmake -G RPM -B build-rpm
sudo rpm -Uvh --replacepkgs build-rpm/llm-switch-*.rpm   # 版本号未变时必须 --replacepkgs
```

图标主题有缓存时再补一条 `sudo gtk-update-icon-cache -f -t /usr/share/icons/hicolor`；
部分桌面环境仍需重新登录或重启 dock 才会重读图标。
