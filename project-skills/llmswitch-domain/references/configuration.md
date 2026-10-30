# 共享配置、探测、恢复与测试隔离

代码路径相对仓库根目录。各工具写入前另读对应 live 契约；用量页面交互详见
[UI 页面行为](../../llmswitch-ui/references/page-behavior.md)。

- **apiFormat 三档**（models.cppm）：`openai-chat`（默认；`""`/旧值
  `"openai"`/未知值都归此档）/ `openai-responses` / `anthropic`。序列化存
  原值，判定/显示/映射一律经 `models::normalizeApiFormat` 归一 +
  `apiFormatLabel` 显示名，各处不各自解释字符串。
- **上游 URL 格式**：Provider 的 `upstreamFormat` 归一为 `anthropic` 或
  `openai`，关闭 `fullUrl` 时 `effectiveBaseUrl` 分别追加 `/anthropic` 或
  `/v1`；`fullUrl` 打开时保留输入地址原样。缺少新字段的旧配置按完整 URL
  兼容，避免历史地址重复追加。
- **用量查询**：Provider.usageEnabled + usageRefreshMinutes + usageUrl/usagePath/usageLabel
  （开关关闭或 usageUrl 空 = 不查，刷新间隔 0 = 仅手动，旧配置缺字段自动取默认
  10）；每个供应商在独立用量配置页控制开关和刷新间隔，HuxerUI HttpClient 异步 GET 后由 `extractByPath` 按点分
  路径（支持数组下标）取标量；官方用量模板表不写在代码里——数据随资源包发布
  （`resources/raw/usage_templates.json`，只收有官方文档的端点），用户可在
  `dataDir()/usage_templates.json` 放同格式文件整体覆盖（设置页展示路径），
  `models::parseUsageTemplates` 容错解析、`suggestUsageQuery` 按 baseUrl
  子串匹配。配置页、惰性刷新、缓存和隐藏页取消约束统一见
  [UI 页面行为](../../llmswitch-ui/references/page-behavior.md)。
- **管理可见性**：AppConfig.disabledTools 存用户关闭的 Agent id（空 = 全部
  启用；存关闭项让新增工具默认可见）。只影响供应商/路由/会话/MCP/Skills 等
  页面展示，不修改 live 配置或 routerTools。
- **本地路由设置**：AppConfig.routerEnabled（启动自启）/ routerPort（默认
  15731）/ routerFailover / routerTools（逐 Agent 代理选择，旧配置默认全开），
  store 有对应 setter；AppRoot 首组合时自启（见 src/ui/app.cpp 的 Lifecycle）。
  LocalRouter 内部以 mutex 保护启用集合，运行中切换无需重启；禁用路径返回 403，
  且在 resolver、上游与统计之前短路。
- **聚合供应商**：`Provider.upstreams` 非空即聚合（无单一直连 baseUrl）。
  编辑页只从当前 Agent 的现有 Provider 勾选成员，写入
  `ProviderUpstream.providerId`（凭据仍只存原 Provider）；旧的直填
  `baseUrl/apiKey/model` 成员继续兼容，`providerId` 为空时才使用。
  选中时 `switchTo` 把 Agent 指向 `127.0.0.1:<routerPort>/<tool>` 并写占位
  key；`SwitchProvider`（UI 入口）先启动本地路由、启用该工具的 routerTools
  并落盘 routerEnabled，避免下次启动失效。LocalRouter 把每个成员当作独立
  尝试：请求体 `model` 精确匹配的成员优先，其余按配置顺序；引用成员从同组
  Provider 解析 URL/key/model，指向聚合项或已删除项的引用跳过；成员
  429/5xx/连接失败继续下一条，不受组级 routerFailover 关闭影响。Codex
  没有 config.toml 模板时由 store 生成最小 `model_providers.llmswitch` 段
  （wire_api=chat）。成员协议必须与目标 Agent 一致。
- **文件安全约定**（store*.cpp 中的文件助手）：`atomicWrite`（`.tmp` →
  rename，失败回落 remove+rename）；`readJsonOrNull`（解析失败把坏文件挪到
  `<file>.corrupt-<毫秒>` 再按无内容继续，绝不崩溃）；`backupLiveFile`（改写
  任何 live 文件前复制到 `backups/<tool>/<文件名>.<毫秒>.bak`，每工具每文件
  只留最近 10 份）。importFrom 前额外备份 config.json 本体。
- **首次导入**：`ProviderStore::load()` 在组为空且 live 文件存在时自动
  `importLive`——把当前生效配置收编成名为「当前配置」的 provider 并设为
  current（已有匹配项则复用不重复建）。zcode / kimi-code 例外：每次 load
  都按 live 配置全量对齐，只补/更新本地卡，不删除本地卡，current 跟
  default/enabled 状态一致。
- **detectCurrent**：读 live 文件与组内 provider 匹配（claude 按
  baseUrl+apiKey，codex 按 apiKey，opencode 按顶层 model 前缀，dsh/hermes
  按默认路由键，kimi-code 按 default_model → model 别名 → provider 键），
  只读不改配置。pi 特殊：先看 settings.json 的
  defaultProvider 是否就是组内 id；是别家的键（用户手写路由）时**只按那一条
  条目**的 baseUrl+apiKey 匹配，不遍历 models.json 的其它条目——本应用切换
  只 upsert、从不删旧条目，历史条目会一直留在文件里，扫全部会把「pi 实际用着
  别的路由」误判成某个历史供应商正在生效；只有 defaultProvider 为空或悬空
  （指向不存在的条目）才退回遍历全部条目。pi 的探测走 readJsonPassive（解析
  失败按无内容，不挪文件），与「只读」契约一致。
- **restoreOfficial**：恢复厂商原生状态（UI 入口 = 供应商列表首位的
  「官方」常驻卡，claude-code / claude / codex / zcode / dsh / kimi-code
  等 officialVendorName 非空的工具；opencode / pi / hermes 无官方
  默认态抛错）——claude-code 删 settings.json env 块的 ANTHROPIC_* 六键；
  codex 删 auth.json 的 OPENAI_API_KEY（删完为空对象则删文件），
  config.toml 仅当内容与组内某 provider 的模板（应用 model 后）完全一致
  才删除（用户手改过的文件不动）；claude desktop 删两份 config 的
  deploymentMode 键 + _meta.json 移除本应用条目/清 appliedId；dsh 删
  settings.yaml 的 agent-default-model 块与 llmswitch-* 手写路由条目，
  回到内置 deepseek-official 路由（.credentials.yaml 的密钥不代清）。
  ZCode 关闭 llmswitch:* 并重新启用首个 builtin:*，细节见 [ZCode 契约](zcode.md)；
  Kimi Code 删全部 llmswitch-* provider/model 别名并把 default_model 指回
  managed:kimi-code 的官方模型。
  改前照常备份，组 current 清空。
- **内置预设**：只收第三方兼容端点（官方厂商由常驻卡承担，不进预设）；
  opencode / pi / hermes 无官方厂商故只有第三方。
- **测试友好**：live 路径全部支持环境变量覆盖——
  `LLMSWITCH_CLAUDE_SETTINGS` / `LLMSWITCH_CODEX_AUTH` / `LLMSWITCH_CODEX_CONFIG`
  / `LLMSWITCH_OPENCODE_CONFIG` / `LLMSWITCH_PI_DIR` /
  `LLMSWITCH_ZCODE_CONFIG` /
  `LLMSWITCH_DSH_SETTINGS` / `LLMSWITCH_DSH_CREDENTIALS` /
  `LLMSWITCH_HERMES_CONFIG` / `LLMSWITCH_KIMI_CONFIG`（后者的默认路径还
  尊重官方 `KIMI_CODE_HOME` 目录覆盖）/
  `LLMSWITCH_CLAUDE_DESKTOP_DIR`（live 配置）；
  `LLMSWITCH_CLAUDE_JSON`（~/.claude.json，MCP）/
  `LLMSWITCH_SKILLS_STORE` / `LLMSWITCH_CLAUDE_SKILLS` / `LLMSWITCH_CODEX_SKILLS`
  （skills）/ `LLMSWITCH_CLAUDE_PROJECTS` / `LLMSWITCH_CODEX_SESSIONS`
  （会话）/ `LLMSWITCH_STATS_DIR`（路由统计目录，默认 dataDir()/router/）
  ——`~` 展开只认 HOME，dataDir 优先 LLMSWITCH_DATA_DIR，再按平台路径解析
  （Linux 走 XDG_DATA_HOME/HOME）；test_store 等用
  setenv 指到 `temp/llmswitch-test-<pid>` 即可完全隔离。
- ProviderStore **不强制单例**（测试可实例化）；UI 侧的全局实例在
  `src/ui/common.cpp::providerStore()`（首次访问即 load）。
