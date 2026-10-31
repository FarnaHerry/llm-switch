# 路由、MCP、技能、会话与账本服务

代码路径均相对仓库根目录。

## LocalRouter 与网络

- cpp-httplib 服务器仅监听 127.0.0.1，默认端口 15731，`/<tool>/` 转发到组内
  current 供应商，按 effectiveBaseUrl 处理 /anthropic、/v1 或完整 URL，并替换鉴权头。
- 故障转移在 429/5xx/连接失败时按组顺序尝试后续供应商；出站使用 UpstreamSession，
  UI 层 PlatformUpstreamSession 经 Post/Launch 调平台 SendAsync，工作线程通过
  条件变量和截止时间等待；停止支持 AbortInFlight。测试注入假上游会话。
- 每个尝试带 sourceApiFormat / targetApiFormat。当前支持 Anthropic Messages
  与 OpenAI Chat 双向转换：Anthropic 客户端 -> Chat 成员走
  `/chat/completions`，Chat 客户端 -> Anthropic 成员走 `v1/messages`；
  请求体、响应体与整段 SSE 都在 `llmswitch.protocol` 中映射，转换器不处理
  鉴权/统计。未支持的方向记录失败并继续聚合兜底。目标协议由
  `targetApiFormat()` 决定，成员协议来自 `ProviderUpstream.apiFormat`
  （空值按 `memberApiFormat()` 推断）。
- 总开关与 routerTools 分别持久化；缺 routerTools 默认全开，显式空数组全关。
  mutex 保护运行时启用集合，变更无需重启；禁用路径返回 403，在 resolver、上游
  与统计前短路。UI 只展示已启用工具接入地址；启动自启跟随持久化设置。
- RequestLog/StatsSnapshot 维护统计，每请求追加 dataDir()/router/requests.jsonl，
  启动回填内存环形缓冲最多 1000 条，文件超 5000 行压至 2500 行，不进 SQLite。
- net 只提供 modelListUrl/候选推导、parseModelIds（data/models 两种形状、去重保序）
  和 extractByPath（点分路径/数组下标标量）；实际网络统一平台 HttpClient。

## MCP

- dataDir()/mcp.json 是统一清单。claude-code 写 ~/.claude.json 顶层 mcpServers
  深合并；codex 对 config.toml 的 [mcp_servers.*] 行级 section 重写；opencode
  写 opencode.json 顶层 mcp。claude desktop/pi/dsh/hermes 等未支持入口抛中文错，
  具体支持集合以 mcp 实现为准，不据供应商注册表推断 MCP 支持。
- Codex 重写会替换全部既有 mcp_servers section；未收编手写项先 importFromTool，
  否则启停会丢失。其它 TOML 内容保留。OpenCode 严格 JSON 解析失败不改原文件。

## Skills 中央库

- dataDir()/skills-store/<name>/ 包含 SKILL.md 及附带文件，通过 create_symlink
  同步到 ~/.claude/skills 与 ~/.codex/skills，合并视图区分中央库、已链接、仅工具侧。
- 这是应用运行时管理用户 skills 的能力，不是仓库自身的 project-skills/ 规范目录。
  测试用 LLMSWITCH_SKILLS_STORE 等覆盖，不能直接污染真实工具目录。

## 会话

- 扫描 ~/.claude/projects/<项目>/*.jsonl 与 ~/.codex/sessions/<年>/<月>/<日>/*.jsonl。
  一次批次读取 mtime/size 和固定大小头尾摘要，摘要 best-effort；详情按需从尾反向分页。
- 删除仅限已知 sessions 根之下，越界抛错；扫描/导出/删除/详情分页均为 worker IO。
  UI 请求代次、缓存和视口规则见 [会话页面](../../llmswitch-ui/references/page-behavior.md)。

## 用量账本与原生活动

- usage 模块保留解析/ScanUsageLogs/聚合口径，stats_page 使用 HuxerUI SQLite 异步 API
  编排 usage/usage.db：usage_records 主键 key、(agent,ts) 索引和 scan_state 表。
- 同步按批次事务提交，位点与记录原子推进；跨轮次冲突字段取 max，保持既有口径。
  旧 usage.jsonl/scan-state.json 已退役，不恢复旧数据读取路径。
- 高频请求日志不进入数据库串行队列；WAL 一致性备份限制见 [持久化](persistence.md)。
- 原生活动与 LocalRouter 独立，仅接收状态所需元数据；hook 用户授权、来源健康和
  不存正文边界见 [活动集成契约](../../../src/agents/README.md)，不要从会话正文
  额外采集桌宠事件，也不把原生活动计入路由请求统计。
