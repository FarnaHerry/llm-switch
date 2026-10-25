# 原生 Agent 活动接入调研与适配决策

> 调研日期：2026-10-25。本文记录公开官方文档、GitHub 项目 README / 设计文档中的接口，以及本仓库的阶段性实现状态。Claude Code / Codex 的 hook 安装、loopback 接收与桌宠状态合并已有实现；关闭路由后的真实会话 spike 已有部分本机结果，完整端到端状态呈现仍待验证。

## 结论

1. 桌宠应优先使用 Agent 官方提供的 lifecycle hooks、plugin 或 extension；这些入口能直接报告开始、工具执行、等待用户与结束，不必从会话正文推断状态。
2. 对 hook 输入只做严格字段白名单解析。输入虽然可能包含 prompt、回复、命令、工具参数、工具输出、transcript 路径和工作目录，但事件适配器只需产生来源、短期会话关联键、状态、发生时间和错误/等待标志；原始 JSON 不落盘、不进日志、不发往网络。
3. `LocalRouter` 保持现有独立来源。路由关闭时也应能由原生 Agent 事件驱动桌宠。Agent 来源关闭、未安装、权限未确认、不可用或首次尚未上报时，一律显示未知/未接入；只有来源已确认健康且观察到明确空闲时才可显示空闲。
4. Claude Code 与 Codex 仍是 P0 首批。Codex 当前有正式 hooks 文档，`notify` 只覆盖回合完成且载荷含消息文本，不作为活动主通道。会话 JSONL 只在 hook 缺口经验证后考虑作元数据回退，不能解析或保存正文。
5. 先做状态观察，不做权限代理、自动批准、MCP 呈现命令或提示词/回复内容采集。Agent 的权限决定继续留在 Agent 自己的 UI/终端。

## GitHub 桌宠项目的接入做法

| 项目 | 公开做法 | 可借鉴点 | 不照搬的边界 |
|---|---|---|---|
| [WindyWin/vpet-agent-pet](https://github.com/WindyWin/vpet-agent-pet) | C++ / Qt 桌宠；安装流程让用户选择连接 Claude Code、Codex，并可选择 Agent 会话启动时带起桌宠；项目描述支持并发 session、等待输入和完成/失败提示。 | 同为 C++ 桌面宠物，证明“桌宠与 Agent 生命周期绑定”可以通过安装时的显式集成完成。它也把连接步骤放在 Agent 设置中，便于用户以后关闭或修复。 | 不把其 README 所列能力视作本项目平台验证；只借鉴入口和状态展示方式。 |
| [nudoo/codex-pets](https://github.com/nudoo/codex-pets)（fork 自 `rullerzhou-afk/clawd-on-desk`） | Claude Code 使用 command hooks，并为权限事件提供 HTTP hook；Codex 使用官方 hooks，另有 `~/.codex/sessions/` JSONL 状态/元数据回退；其他工具各有独立 hook/plugin/extension adapter。 | hooks-first、按工具独立 adapter、只对缺失事件加日志回退，适合逐步扩展到多 Agent。其文档也明确区分“观察状态”和“接管权限”。 | 本项目不需要权限浮层；JSONL 回退不是默认方案，且不得为了显示状态读取正文。 |
| [Ponpon55837/agent-pets](https://github.com/Ponpon55837/agent-pets) | 用户逐个安装 adapter；界面显示已安装/部分配置/不可用、能力、诊断和测试状态；hook 向 `127.0.0.1` 本地 receiver 报事件，按安装令牌认证。 | adapter registry、显式安装、健康检查和可解释错误值得采用。测试事件只能证明 receiver 到 UI 的链路，不能证明 Agent 自己确实触发 hook。 | 本项目已采用独立于 `LocalRouter` 的令牌保护 loopback receiver；不开放 LAN，也不把 Agent 状态统计混入路由请求统计。 |

## 八个已注册工具的公开接口盘点

| 注册 id | 可用的官方/公开观察入口 | 状态覆盖与注意事项 | 建议 |
|---|---|---|---|
| `claude-code` | Claude Code command / HTTP hooks；用户级配置通常在 `~/.claude/settings.json`。 | `SessionStart`、`UserPromptSubmit`、`PreToolUse` / `PostToolUse`、`PermissionRequest`、`Stop`、`StopFailure`、`SessionEnd`、subagent 事件可表示工作、等待、结束和失败。输入字段含 `prompt`、`tool_input`、`tool_response`、`last_assistant_message`、transcript 路径等敏感内容；hook 配置在会话启动时加载。 | P0。安装时显式 opt-in；增量合并且仅移除带本应用标记的 hook；只解析 `session_id`、事件名和必要状态，不读其余字段。hook 不作阻塞、改写或批准。 |
| `claude`（Claude Desktop） | 本次未在 Anthropic 公开文档中确认独立桌面客户端的通用 lifecycle hook/API。它与 Claude Code CLI 是不同产品；MCP 配置不能证明存在被动活动事件。 | 不根据 Claude Code hooks 推断 Claude Desktop 可接入；也不扫描未公开的内部数据库来补齐。经 `LocalRouter` 的请求仍可见。 | 暂列“未接入/未知”。等 Anthropic 提供受支持的本地 lifecycle 接口再评估。 |
| `codex` | Codex lifecycle hooks；`~/.codex/hooks.json`、活动配置层的 `hooks.json` 或 `config.toml`。 | 官方事件覆盖 `SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PermissionRequest`、`PostToolUse`、`Stop`、`Interrupt`、`SessionEnd` 与 subagent 等。hooks 没有专用的 turn failure 事件，`Stop` 本身不能区分成功与失败。非托管 hook 必须由用户 review/trust；用户或管理员可关闭 hooks。`SessionEnd` 可能延迟到关闭/归档，不能单独作为实时结束信号。stdin 含 `prompt`、transcript 路径及事件相关字段；应用只取 `session_id` 与 `hook_event_name`。 | 首批 hook 安装和状态桥接已实现；本机已确认 Codex TUI hooks Active，真实成功任务和退出期间都启动过 helper，逐事件状态尚未从桌宠可见层确认。Codex app-server 的 `turn/completed` 提供 Completed / Interrupted / Failed 状态和可选 error，可作为 hook 覆盖缺口的官方候选来源；若采用只读取状态与错误标志，不读取错误文本，并评估如何只订阅用户启用的本地会话，再决定是否采用，优先于读取会话 JSONL。安装前检查 hook feature，不覆盖 `hooks=false`，保留 Codex 自己的 trust 流程。未单独验证前不宣称覆盖 Codex Desktop。 |
| `opencode` | 官方 JS/TS plugin；本地全局插件目录 `~/.config/opencode/plugins/`，也支持项目插件目录。 | 公开事件包括 `session.status`、`session.idle`、`session.error`、`session.updated`、`permission.asked/replied`、`tool.execute.before/after`。插件回调在 Agent 进程内运行；可选择只读 `event` 监听，避免修改工具输入/输出。 | 后续 native adapter。插件只发归一状态，不依赖消息更新事件；用户级安装可逆，插件目录无需改写其他插件。 |
| `pi` | Pi coding-agent extension；全局目录 `~/.pi/agent/extensions/`，支持 `pi.on(...)` 订阅。 | SDK 暴露 `agent_start/end`、`turn_start/end`、`tool_execution_start/end`、`session_shutdown` 等事件。部分事件对象包含 assistant messages、工具参数/结果；扩展只取事件类型、会话关联和错误布尔值。 | 后续 native adapter。Pi 本身已有 Node runtime，可使用用户级 extension；不为 llm-switch 嵌入 Node。 |
| `dsh` | DeepSeek Harness 公共插件事件/API。`agent/status` 在 `idle` 与 `running` 间转换，另有 `agent/created`、`agent/disposed`；session/turn 事件可提供更细粒度。 | 事件 API 属于 dsh 进程内；当前需验证已安装 CLI/Desktop profile 的插件加载和跨进程事件发送路径。更细的 `session/event` 可能携带消息/流片段，桌宠无需订阅内容流。 | 后续 native adapter。优先用 `agent/status` 和创建/销毁事件，不依赖翻阅 session event log；另确认桌面版 profile 与 CLI profile 的覆盖差异。 |
| `hermes` | Hermes Agent shell hooks 或 Python plugin hooks；shell 配置在用户 `config.yaml`，plugin 可注册 `on_session_start/end`、工具及 API observer hooks。 | canonical `on_session_end` 带 `completed` / `failed` / `interrupted`，但不同 CLI/TUI/gateway 退出路径字段可能减少；部分 API observer 载荷带请求、回复和错误文本。Shell hook 首次执行需要逐项授权。 | 后续 native adapter。选择观测型 hook；保留首次授权，不借机安装会拦截/变换型 hook。按 Hermes surface 实测缺失事件与结束时序。 |
| `zcode` | ZCode hooks；用户级 `~/.zcode/cli/config.json`，需 `hooks.enabled=true`；插件可携带 `hooks/hooks.json`。 | `SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PermissionRequest`、`PostToolUse`、`PostToolUseFailure`、`Stop`。stdin JSON 含 prompt、工具参数/结果、错误、`last_assistant_message` 和临时 transcript 路径；hook 配置在 session 创建时快照。当前公开文档称项目级 hooks 不执行。 | 后续 hook adapter。不得擅自打开用户全局 hooks 总开关；若未启用，提示用户在 Agent 内显式启用或维持未知。hook 输出保持空、成功退出，避免影响模型/工具决策。 |

## 建议的统一事件边界

每个 adapter 只向应用级 aggregator 提交归一后的短期事件：

```text
source id + 内存期 session key + state + occurred_at + optional error/wait flag
```

- `session key` 仅用于当前进程内合并多 session；应用退出后丢弃原始 session id 与映射，不持久化。
- 不读取或保留 prompt、assistant 回复、工具命令、工具参数/结果、标题、transcript 文件路径、完整工作目录、API key 或凭据。项目名也不作为桌宠状态事件的必需字段。
- 可以展示“Agent 正在工作”与“Agent 正等待用户”，但只有提供真实 token-stream 事件的来源才标为 streaming；普通 hooks 只能可靠表达 working。
- `PermissionRequest` 只映射为 waiting；不显示完整命令、不执行批准/拒绝，也不取代 Agent 自身权限界面。
- `Stop` / turn completion 映射为短暂 done；失败事件映射 error；session dispose/end 清除该 session。结束信号缺失或超时后转 unknown/stale，不能静默变成 idle。
- 多 session 先按 `(source, session key)` 去重；聚合展示优先级为 waiting → error → working → done → observed idle。`LocalRouter` 与 native event 的请求数不相加，路由统计仍单独显示；首批实测同一 Agent 同时经过路由和 hook 时的状态合并。
- 来源健康状态单独表达：disabled / not-installed / needs-agent-trust / unavailable / observing。健康状态与“Agent 正空闲”不是同一概念。

## 安装、维护和诊断约定

1. 每个 Agent 一项设置，默认不改写 Agent 配置；用户点“连接”后再安装，点停用后停止接收，卸载仅删本应用拥有的条目与脚本。
2. 对 JSON/TOML/YAML 配置做保留未知字段的增量合并；用稳定标记识别本应用 hook，安装重复运行幂等。现有用户 hook、显式 disable、Codex trust 状态都原样保留。
3. 不调用 Agent CLI 跑任意提示，不把 prompt 内容塞到命令行。hook helper 对支持的事件请求异步运行、无输入回显并 fail-open；Codex `Stop` / `SubagentStop` 仅输出固定成功 JSON，Codex `SessionEnd` 可能同步运行。bridge 不可用时 Agent 正常工作不应依赖桌宠进程。
4. UI 为每个 adapter 显示“未检测 / 已检测未连接 / 已连接 / 等待 Agent 信任 / 连接错误”，提供只验证本地接收链路的测试事件与有界诊断。不要把“测试事件成功”说成“真实 Agent 已上报”。
5. 事件 receiver 必须独立于 `LocalRouter` 开关与端口。本仓库已采用随机令牌、仅绑定 `127.0.0.1` 的短期 loopback bridge；不开放 LAN、不提供远程上报。
6. 单个 Agent 配置损坏、脚本缺失、版本不兼容或插件异常，不影响其他 adapter、路由和控制中心。健康检查只做有界读操作，不自动覆盖未知配置。

## 落地顺序与验收

### P0：Claude Code 与 Codex

1. 归一事件 contract、内存 aggregator、loopback bridge、增量 hook 管理与桌宠状态合并已实现；`LocalRouter` 保持独立。
2. 为两个 Agent 做受控本机 spike：分别关闭 LocalRouter，显式安装 hook，测试开始、工具执行、等待输入、正常结束、失败、中断、并发 session、应用先启动/后启动、Agent 配置中已有用户 hook 等情况。2026-10-25 部分实测：Codex TUI 不调用工具的文本任务返回成功，`/hooks` 显示应用 hooks Active，进程跟踪观察到任务期间与会话退出时的 helper；Claude Code 的最小真实请求失败，但该会话观察到 4 次 helper 执行。hook 输入未读取；事件级状态和桌宠展示仍未逐项验收。
3. 记录 Agent 版本、触发事件、延迟、缺失事件及安装前后配置差异；Codex 需分别验证 hook 未信任、hooks 关闭和 `SessionEnd` 延迟场景。Codex hooks 没有专用 failure 事件；官方 app-server `turn/completed` 的状态 enum 可区分 completed / interrupted / failed，应先评估该本地接口，再考虑受限的 JSONL 元数据回退。
4. 对已实现接入做隐私探针：注入唯一的假 prompt/命令字符串，确认它们未进入 llm-switch 数据目录、日志、bridge payload 或桌宠 State。

P0 完成条件仍以 [桌宠路线图](desktop-pet-roadmap.md) 的验收为准；文档调研不视为 spike 已完成。

### 后续 adapters

先实现有明确官方本地事件契约的 `zcode`、`opencode`、`pi`、`dsh`、`hermes`，每个 source 单独启停与健康状态；`claude`（Claude Desktop）继续未知，除非新增可验证的官方 lifecycle API。排序可按实际用户装机率和版本支持调整，不以工具注册顺序推断覆盖优先级。

## 参考资料

### Agent 官方资料

- Anthropic：[Claude Code Hooks reference](https://code.claude.com/docs/en/hooks)
- OpenAI：[Codex Hooks](https://developers.openai.com/codex/hooks)；[Codex app-server protocol](https://github.com/openai/codex/tree/main/codex-rs/app-server)；[`TurnStatus` / `TurnCompletedNotification`](https://github.com/openai/codex/blob/main/codex-rs/app-server-protocol/src/protocol/v2/turn.rs)
- OpenCode：[Plugins and events](https://opencode.ai/docs/plugins/)
- Pi：[Pi coding-agent SDK and extensions](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/sdk.md)；[extension API](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/extensions.md)
- DeepSeek Harness：[public agent API](https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/core/agent/README.md)；[core event contract](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/subsystems/core.md)
- Hermes：[Event Hooks](https://github.com/NousResearch/hermes-agent/blob/main/website/docs/user-guide/features/hooks.md)
- ZCode：[Hooks](https://zcode.z.ai/en/docs/hooks)；[public GitHub repository](https://github.com/zai-org/ZCode)

### 桌宠与观测项目

- [vpet-agent-pet](https://github.com/WindyWin/vpet-agent-pet)
- [codex-pets](https://github.com/nudoo/codex-pets)；其 [Agent setup guide](https://github.com/nudoo/codex-pets/blob/main/docs/guides/setup-guide.md)
- [agent-pets](https://github.com/Ponpon55837/agent-pets)
