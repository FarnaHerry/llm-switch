# 已知取舍与支持边界

代码路径相对仓库根目录。这些是现有兼容与数据保留边界，变更前须针对具体任务
核对实现，不能仅因不符合个人偏好而顺手改写。

- **store 无锁与 router resolver 竞态窗口**：ProviderStore 无内部锁、UI 线程
  独占是全局契约；router_page.cpp 的 routerInstance() resolver 会被 router
  的 httplib 后台线程调用，此刻对 providerStore() 做只读组快照拷贝，与 UI
  线程写操作存在理论并发窗口（已知的极小概率取舍，router.cpp 要求 resolver
  线程安全且快速；代码注释已标记）。
- **codex MCP 的 TOML 重写会丢未收编的手写 [mcp_servers.*] 节**：llmswitch.mcp
  按行级 section 重写 config.toml——丢弃全部既有 [mcp_servers.*] 节后按当前
  启用清单重新生成（其余内容原样保留）。用户手写的 mcp_servers 若未先
  importFromTool 收编进 mcp.json，首次启停后丢失。
- **opencode 配置不支持 JSON5 注释**：opencode.json / 顶层 mcp 改写都走严格
  JSON 解析，带注释的官方文件会抛中文错且绝不碰原文件（让用户手动去注释）。
- **dsh 的 flow→块摊平会丢掉 flow 区域内的注释**：settings.yaml 的
  `llm-pi-ai.providers` 若是 flow 风格（用户手写或其它工具写入），本应用在读写
  之前会把这整段摊平成块风格——只改排版、键值与顺序原样保留，但 flow 区域内部
  的行尾注释不保留（dsh 自己生成的文件没有注释）。不摊平的话，往 flow 块后插
  块条目会写出非法 YAML，dsh 的 watcher 会保留上一份好文档、切换静默失效。
- **MCP 不支持 claude desktop / pi / dsh / hermes / kimi-code**：setEnabled/importFromTool 对这些
  工具抛「该工具暂不支持 MCP 管理」。
- **路由地址和关于页链接仍是纯文本**：框架已提供应用层 Clipboard 服务，
  但这些页面尚未接入复制或浏览器跳转。后续若新增交互，先核对当前版本公开 API，
  不把旧版“框架没有浏览器 API”的说法当作永久限制。接入复制按钮时使用
  `UseApplication().Clipboard()`，并按 `IsAvailable()` 控制可用状态。
