# Claude、Codex、OpenCode、Pi、Hermes 与 Kimi Code live 契约

live 文件是被切换工具实际读取的配置文件。文件安全、探测与恢复另见
[共享配置契约](configuration.md)。代码路径相对仓库根目录。

## Claude Code

切换 = 深合并
`~/.claude/settings.json` 的 `env.ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN`
（model 非空时写 `ANTHROPIC_MODEL`；haiku/sonnet/opusModel 三档映射非空时
写 `ANTHROPIC_DEFAULT_HAIKU/SONNET/OPUS_MODEL`），permissions 等其余字段
原样保留。

## Codex

切换 = 深合并 `~/.codex/auth.json` 的 `OPENAI_API_KEY`，且
`Provider.codexConfigToml` 非空时**整体替换** `~/.codex/config.toml`（TOML
不做结构化合并，原文即模板），`Provider.model` 非空时再**行级重写**
config.toml 顶层 `model` 键（只动第一个 `[` 节之前：替换未注释行/注释行，
都没有则开头插入；`applyCodexModel`/`parseCodexModel`，importLive 也会
收回顶层 model）。

## OpenCode

opencode.json 顶层 provider map
additive upsert（npm 段按 apiFormat 三档选 `@ai-sdk/anthropic` /
`@ai-sdk/openai` / `@ai-sdk/openai-compatible`；官方文件允许 JSON5 注释，
改写用 readJsonStrict 解析失败抛错、绝不碰原文件）。

## Pi

models.json
providers upsert（api 字段三档映射 anthropic→anthropic-messages /
openai-responses→openai-responses / 其余→openai-completions）+
settings.json 深合并 defaultProvider/defaultModel（目录 0700、文件 0600）；
条目里已有的**其它模型清单原样保留**（本应用只声明主模型，清单裁成一条会
让 pi 的模型菜单静默少几条），主模型缺失时按原元素形状补一条——裸标量与
`{"id": ...}` 对象两种形状都认（`modelEntryId`/`modelListContains`）。

importLive 若 settings.defaultModel 缺失，回退到默认条目的第一个模型；
不因缺少顶层模型把已有列表收编丢失。

## Hermes

hermes = `~/.hermes/config.yaml` 行级改写：
custom_providers 列表删旧 llmswitch-* 条目后追加新条目（api_mode 三档映射
anthropic→anthropic_messages / openai-responses→codex_responses /
其余→chat_completions），顶层 model 节写 provider（总是）与
default（model 非空时），agent / mcp_servers / v12+ providers dict
等其余节原样保留（目录 0700、文件 0600）；hermes 无官方默认态，
restoreOfficial 抛错。

## Kimi Code

CLI 与 Desktop 共用 `~/.kimi-code/config.toml`（`KIMI_CODE_HOME` 可覆盖目录；
旧 kimi-cli 的 `~/.kimi/config.toml` 需用户自行 `kimi migrate --config-only`，
本应用不把它当第二个工具）。Kimi 的供应商是两条并存的表：`[providers.<key>]`
（type `kimi` / `openai` / `openai_responses` / `anthropic`、base_url、api_key
或 api_key_env / oauth 子表）与 `[models.<alias>]`（provider、model、必填的
max_context_size，可选 capabilities / support_efforts / display_name）；顶层
`default_model` 指向当前模型别名。

本应用只增改自己的 `llmswitch-<id>` provider 与模型别名：新增 / 更新走单条
增量，**切换只改 default_model**；已有条目在切换时内容原样保留（用户在
Kimi 侧的手改由表单「保存」显式覆盖）。`managed:kimi-code`（OAuth 官方账号）
不收编、不覆盖、不删除，由「Kimi Code 官方」常驻卡表达；restoreOfficial 摘掉
全部 `llmswitch-*` 条目并把 default_model 指回官方模型别名。删除本地卡
（连同 live）删选中 provider 及其 model 别名——本应用条目是 `llmswitch-*`，
收编条目是原生键；managed:* 永不删除。Kimi 监听
config.toml（watch 默认开启），切换无需重启。TOML 写入是行级子集：保留注释、
未知表、oauth 子表与其它 provider/model；写前备份、原子落盘。

## Claude Desktop

3p 直连（两份 claude_desktop_config.json 置
deploymentMode=3p + configLibrary 固定 id profile/_meta.json，
**Linux 不支持**）。inferenceModels = 主模型 + 三档映射条目：模型名是
白名单 route id（claude-(sonnet|opus|haiku|fable)-*）直写 name，否则借
该档安全 route id（haiku-4-5/sonnet-4-6/opus-4-8）；菜单显示名写入
labelOverride，1M 能力按 supports1m 声明，实际请求模型保存在 Provider 的
三档 *Model 字段（`claudeDesktopModelEntry`）。
