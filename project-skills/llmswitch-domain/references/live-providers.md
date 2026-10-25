# Claude、Codex、OpenCode、Pi 与 Hermes live 契约

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

## Claude Desktop

3p 直连（两份 claude_desktop_config.json 置
deploymentMode=3p + configLibrary 固定 id profile/_meta.json，
**Linux 不支持**）。inferenceModels = 主模型 + 三档映射条目：模型名是
白名单 route id（claude-(sonnet|opus|haiku|fable)-*）直写 name，否则借
该档安全 route id（haiku-4-5/sonnet-4-6/opus-4-8）；菜单显示名写入
labelOverride，1M 能力按 supports1m 声明，实际请求模型保存在 Provider 的
三档 *Model 字段（`claudeDesktopModelEntry`）。
