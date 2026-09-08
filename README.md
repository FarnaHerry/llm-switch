# llm-switch

[cc-switch](https://github.com/farion1231/cc-switch) 的 C++ 重写：Claude Code /
Claude Desktop / Codex / opencode / pi 五款 AI 编程工具的供应商配置切换，
附本地路由、用量查询、MCP / Skills / 会话管理。C++23 modules + HuxerUI
桌面壳，无 Electron、无运行时依赖（除系统 GTK4 运行库）。

## 功能

- **供应商管理**：五个工具组各自维护供应商列表（新增 / 编辑 / 复制 /
  删除），内置 DeepSeek / Kimi / GLM / OpenRouter 等预设模板；API 协议三档
  （OpenAI Chat Completions / OpenAI Responses / Anthropic Messages）。
- **一键切换**：把选中供应商写进工具的 live 配置文件——
  Claude Code 深合并 `~/.claude/settings.json` 的 env 块
  （`ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN` / `ANTHROPIC_MODEL`，其余字段
  原样保留）；Codex 写 `~/.codex/auth.json` 的 `OPENAI_API_KEY`，并可整段替换
  `~/.codex/config.toml`；opencode 顶层 provider map additive upsert；
  pi 写 models.json + settings.json；Claude Desktop 3p profile（仅
  macOS / Windows）。
- **本地路由**：内置反向代理监听 `http://127.0.0.1:<port>/<tool>/`（默认
  15731），转发到该工具当前供应商并自动替换鉴权头；上游 429/5xx 可选故障
  转移；支持逐 Agent 选择是否接受代理，运行中切换即时生效，并支持开机自启。
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
- 极简黑白主题（深色 / 浅色 / 跟随系统）。

## 构建前提

- CMake ≥ 3.30（本机用 4.4，`import std` 的 experimental UUID 见
  `cmake/CxxImportStdGate.cmake`）、GCC ≥ 16（libstdc++）、Ninja
- HuxerUI 0.2.0：已安装 SDK（`HUXERUI_HOME` 指向前缀）或源码（clone 到
  `third_party/huxerui/`），都没有时用仓库内 Linux 离线包兜底。
  Windows 自定义安装向导（`huxerui package windows`，MSI + Burn 捆绑包 +
  HuxerUI 托管安装器 UI，`platform/windows/package/`）需要含
  `huxerui_add_windows_installer` 的源码/新版 SDK（0.2.0 之后），
  0.2.0 发布包没有该能力
- HuxerUI 源码通道编译需要 gtk4 / libepoxy / libsoup3 开发包
  （Fedora：`sudo dnf install gtk4-devel libepoxy-devel libsoup3-devel`）；
  SDK 通道只需要运行时库

## 构建 / 运行 / 测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./run.sh                 # 或 huxerui run linux（CLI 流程）
```

项目也提供了 Ninja 预设：`cmake --preset ninja-release`、
`cmake --build --preset ninja-release`。

CI（GitHub Actions，`.github/workflows/build.yml`）提供 Linux / Windows /
macOS 三平台构建产物：日常构建在 Actions 页面下载压缩包（保留 14 天），
push `v*` tag 会自动建 release 并挂上各平台包（Windows/macOS 为实验性
job，失败时 release 只挂实际产出的包）。

## 配置存储位置

- llm-switch 自身数据目录：`~/.local/share/llm-switch/`（遵循 XDG；
  `$XDG_DATA_HOME/llm-switch/`）——`config.json`（配置库）、`backups/`、
  `mcp.json`（MCP 统一清单）、`skills-store/`（Skills 中央库）、
  `router/requests.jsonl`（路由请求统计）。
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
图标来源与许可见 [resources/README.md](resources/README.md)
（Claude / OpenAI logo 来自 simple-icons，CC0）。
