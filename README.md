# llm-switch

桌宠优先的本地 AI 桌面伙伴与多 Agent 控制中心。桌宠是日常入口；主窗口用于管理
Claude Code / Claude Desktop / Codex / opencode / pi / Oh My Pi / DeepSeek
Harness（dsh）/ Hermes Agent / ZCode / Kimi Code 十款工具的供应商配置，并提供
本地路由、用量、MCP、Skills 与会话管理。产品方向与实施顺序见 [桌宠路线图](docs/desktop-pet-roadmap.md)。

项目是 [cc-switch](https://github.com/farion1231/cc-switch) 的 C++ 重写，使用
C++23 modules + HuxerUI，无 Electron。

## 功能

- **AI 桌宠优先入口**：桌宠汇总多 Agent 活动并打开快捷信息卡；Shimeji 提供
  Q 版帧动画，Live2D 提供模型、动作、表情、参数与 HitArea 高阶交互，莲花球作为
  低资源与加载失败时的内建回退。模型目录可在设置页导入、切换和删除；原生 Agent
  活动覆盖与 Live2D 分发验证按[路线图](docs/desktop-pet-roadmap.md)推进。
- **供应商管理**：十个工具组各自维护供应商列表（新增 / 编辑 / 复制 /
  删除），内置 DeepSeek / Kimi / GLM / OpenRouter 等预设模板；API 协议三档
  （OpenAI Chat Completions / OpenAI Responses / Anthropic Messages），
  Kimi Code 另有自己的 Kimi 协议档；推理档位支持规范七档，Kimi 另含最高
  `ultra`，还可追加自定义 `support_efforts` 值。
- **聚合供应商（所有 Agent）**：从当前 Agent 的现有供应商里勾选多个供应商，
  默认带入每个供应商的全部已知模型（主模型优先，再按模型清单顺序），每个
  模型是一条成员；对 Agent 暴露的模型 ID 先按供应商内部去重、再整组合并，
  并拼成「供应商名/模型」。默认直接引用直连模式已配置的模型（主模型、
  三档映射和已保存的模型清单），无需额外请求；「获取全量模型」才按各供应商
  地址和密钥拉取完整 /models 并写回。模型可手动增删，也可拖拽或用上下按钮
  调整兜底顺序。凭据只保留在原供应商条目里。选中后 Agent 走本地路由，按请求 model 精确匹配成员（转发
  前剥掉供应商前缀），429/5xx 或连接失败时按列表顺序继续尝试；聚合内兜底不受
  组级故障转移开关影响。每个供应商可显式选择成员协议；Anthropic Messages、
  OpenAI Chat 与 OpenAI Responses 三档可互相自动转换（Responses 与 Chat 通过
  Anthropic 中间表示），成员地址需与所选协议匹配（Chat/Responses 使用
  `/v1` 等地址，不能沿用 `/anthropic`；Anthropic 地址不要带 `/v1`）。
  dsh / ZCode 的专用供应商页在「使用 / 写入」时把聚合项落成本地路由条目；
  Kimi Code 要求至少一个成员设置模型。
- **直连 / 路由 / 聚合三段分类**：支持单供应商切换的 Agent（dsh / ZCode 的
  增量多供应商页除外）顶部有「直连 / 路由 / 聚合」页签（对齐 cc-switch）。页签只切换正在看的列表，不改变生效模式；
  直连写供应商真实地址，路由写本地 `127.0.0.1:<port>/<tool>` 并由路由注入
  鉴权，聚合按请求 model 分发成员。`direct` 回退供应商独立保存，切换路由
  目标或聚合项不会丢失「回到直连」的目标；页签行右侧的加号按当前页签打开
  普通或聚合表单。
- **按需显示**：设置页「管理的 Agent」可关闭不常用的工具，供应商、路由、
  会话、MCP / Skills 页面随之隐藏；只影响展示，不改已有 live 配置。
- **一键切换**：把选中供应商写进工具的 live 配置文件——
  Claude Code 深合并 `~/.claude/settings.json` 的 env 块
  （`ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN` / `ANTHROPIC_MODEL`，其余字段
  原样保留）；Codex 写 `~/.codex/auth.json` 的 `OPENAI_API_KEY`，并可整段替换
  `~/.codex/config.toml`；opencode 顶层 provider map additive upsert；
  pi 写 models.json + settings.json；dsh 行级改写
  `~/.dsh/settings.yaml` 的 llm-pi-ai 路由并把密钥写进
  `~/.dsh/.credentials.yaml`（apiKeyEnv 引用，热监听即时生效）；
  hermes 行级改写 `~/.hermes/config.yaml` 的 custom_providers 列表并
  指向顶层 model 节；Kimi Code 增量改写 `~/.kimi-code/config.toml` 的
  `[providers.*]` / `[models.*]`，切换只改 `default_model`（CLI 与 Desktop
  共用同一份配置，热监听即时生效）；
  Claude Desktop 3p profile（仅 macOS / Windows）。
- **本地路由**：内置反向代理监听 `http://127.0.0.1:<port>/<tool>/`（默认
  15731），转发到该工具当前供应商并自动替换鉴权头；支持 Anthropic Messages、
  OpenAI Chat 与 OpenAI Responses 三档协议之间的请求/响应/SSE 转换；上游
  429/5xx 可选故障转移；支持逐 Agent 选择是否接受代理，运行中切换即时生效，
  并支持开机自启。
- **使用统计**：按请求记录状态码 / 耗时 / token 用量，JSONL 持久化
  （`~/.local/share/llm-switch/router/requests.jsonl`），统计页看汇总与
  按供应商分布。
- **用量查询**：为供应商配置用量端点（DeepSeek 等内置模板一键填充），
  卡片上直接显示余额，支持自动轮询与手动刷新。
- **MCP 服务器**：统一清单管理，启停即同步到 Claude Code / Codex /
  opencode 的实际配置。
- **Skills**：中央库维护技能，符号链接同步到 Claude Code / Codex 的
  skills 目录。
- **会话管理**：浏览 / 删除 / 导出 Claude Code 与 Codex 的历史会话。
- **安全兜底**：改写任何 live 文件前自动备份（每工具每文件保留最近 10 份），
  所有写入原子化（tmp + rename）；配置文件损坏自动隔离不崩溃。
- **收编与探测**：首次启动自动把当前生效配置收编为「当前配置」供应商；
  列表实时标记 live 文件命中的「使用中」项。
- **系统托盘**：托盘菜单按工具分组列出供应商，点击直接切换。
- **导入 / 导出**：整个配置库导出为 JSON（系统文件对话框或固定目录），
  导入按 id 合并且导入前自动备份。
- 黑白双色主题：深色近黑 / 浅色纯白，背景、卡片与描边只用中性黑白灰；
  主题蓝只出现在主要操作按钮、选中态和焦点等交互控件上。支持深色 / 浅色 /
  跟随系统，设置页用一枚太极图形选择器切换。

## 构建前提

- CMake ≥ 3.30（本机用 4.4，`import std` 的 experimental UUID 见
  `cmake/CxxImportStdGate.cmake`）、GCC ≥ 16（libstdc++）、Ninja
- HuxerUI 0.3.0：默认使用最新源码（clone 到 `third_party/huxerui/`）或已安装
  SDK（`HUXERUI_HOME` 指向前缀）；都没有时用仓库内 Linux 0.2.0 离线包兜底。
  Windows 自定义安装向导（`huxerui package windows`，MSI + Burn 捆绑包 +
  HuxerUI 托管安装器 UI，`platform/windows/package/`）会随 Windows 发布产出
  `llm-switch-windows-x86_64-setup.exe`；该流程需要含
  `huxerui_add_windows_installer` 的源码/新版 SDK（0.2.0 之后），
  0.2.0 发布包没有该能力
- HuxerUI 源码通道编译需要 gtk4 / libepoxy / libsoup3 开发包
  （Fedora：`sudo dnf install gtk4-devel libepoxy-devel libsoup3-devel`）；
  SDK 通道只需要运行时库

## 构建 / 运行 / 测试

```bash
# 本地开发默认 Debug；CI / 打包用 Release
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./run.sh                 # 或 huxerui run linux（CLI 流程）
```

项目也提供了 Ninja 预设：`cmake --preset ninja-debug`、
`cmake --build --preset ninja-debug`、`ctest --preset ninja-debug`；
发布用 `ninja-release`。

`./run.sh` 会优先启动已配置为 `LLMSWITCH_LIVE2D=ON` 的
`build-live2d/llm-switch`，否则启动 `build/llm-switch`；可用
`LLMSWITCH_BIN=/path/to/llm-switch` 显式指定程序。

CI（GitHub Actions，`.github/workflows/build.yml`）提供 Linux / Windows /
macOS 三平台构建产物：日常构建在 Actions 页面下载压缩包，Windows 另含
HuxerUI 安装器（保留 14 天），
push `v*` tag 会自动建 release 并挂上三个平台包；三个平台均通过构建、测试和
打包后才会发布。

## 配置存储位置

- llm-switch 自身数据目录：`~/.local/share/llm-switch/`（遵循 XDG；
  `$XDG_DATA_HOME/llm-switch/`）——`config.json`（配置库）、`backups/`、
  `mcp.json`（MCP 统一清单）、`skills-store/`（Skills 中央库）、
  `router/requests.jsonl`（路由请求统计）、`shimeji/` 与 `live2d/`（用户导入模型）。
- 操作的 live 文件（工具实际读取的配置）：
  - Claude Code：`~/.claude/settings.json`（另有 MCP 写 `~/.claude.json`）
  - Codex：`~/.codex/auth.json` 与 `~/.codex/config.toml`
  - opencode：`~/.config/opencode/opencode.json`
  - pi：`~/.pi/agent/models.json` 与 `~/.pi/agent/settings.json`
  - Claude Desktop：macOS `~/Library/Application Support/Claude` /
    Windows `%LOCALAPPDATA%\Claude`（Linux 不支持）
- Skills 同步目标：`~/.claude/skills`、`~/.codex/skills`；
  会话扫描：`~/.claude/projects`、`~/.codex/sessions`。
- 以上路径均支持 `LLMSWITCH_*` 环境变量覆盖
  （`LLMSWITCH_DATA_DIR`（自身数据目录整体覆盖，全平台最优先）/
  `LLMSWITCH_CLAUDE_SETTINGS` / `LLMSWITCH_CODEX_AUTH` /
  `LLMSWITCH_CODEX_CONFIG` / `LLMSWITCH_OPENCODE_CONFIG` / `LLMSWITCH_PI_DIR` /
  `LLMSWITCH_CLAUDE_DESKTOP_DIR` / `LLMSWITCH_CLAUDE_JSON` /
  `LLMSWITCH_SKILLS_STORE` / `LLMSWITCH_CLAUDE_SKILLS` /
  `LLMSWITCH_CODEX_SKILLS` / `LLMSWITCH_CLAUDE_PROJECTS` /
  `LLMSWITCH_CODEX_SESSIONS` / `LLMSWITCH_STATS_DIR`），供测试与非常规
  安装使用。

## 开发

见 [CLAUDE.md](CLAUDE.md)（架构、领域层设计要点、UI 硬约束）。
自动化 agent 还必须遵守 [AGENTS.md](AGENTS.md)：每批修改完成后完整编译、
运行测试、创建本地 Git 提交并尝试推送；推送失败时保留本地提交并报告原因。
后续工程事项见 [TODO.md](TODO.md)。
图标来源与许可见 [resources/README.md](resources/README.md)
（Claude / OpenAI logo 来自 simple-icons，CC0）。
