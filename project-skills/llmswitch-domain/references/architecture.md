# 模块职责与代码定位

代码路径相对仓库根目录。当前工具清单以 models::toolRegistry() 为准：
claude-code / claude desktop（id=claude）/ codex / opencode / pi / dsh / hermes /
zcode / kimi-code。
历史里程碑中的工具数量、文件布局与旧主题不是当前实现依据。

## 领域模块

| 模块 | 主要入口 | 职责 |
|------|----------|------|
| config | src/config.cppm | 数据目录、live/会话/技能路径、环境变量覆盖、深色检测 |
| models | src/models.cppm | 工具注册表、配置模型、归一函数、预设与模板解析 |
| store | src/store.cppm、store*.cpp | 配置读写、CRUD、live 写入、探测、收编与恢复 |
| net | src/net.cppm、net.cpp | 模型 URL 推导、模型列表及用量响应纯解析 |
| router | src/router.cppm、router.cpp | 本地反代、鉴权替换、故障转移、统计与请求日志 |
| mcp | src/mcp.cppm、mcp.cpp | MCP 统一清单与支持工具的 live 同步 |
| skills | src/skills.cppm、skills.cpp | 中央库管理、工具侧软链接同步与合并视图 |
| sessions | src/sessions.cppm、sessions.cpp | 会话枚举、摘要、反向分页、导出与安全删除 |
| usage | src/usage.cppm、usage.cpp | 会话用量解析、扫描和聚合；数据库编排在 UI 层 |
| shimeji | src/shimeji.cppm、shimeji.cpp | Shimeji-ee 包校验、动作/行为解析与状态映射 |
| pet_profile | src/pet_profile.cppm、pet_profile.cpp | Live2D 模型目录与动作/表情/参数配置 |
| pet_packages | src/pet_packages.cppm、pet_packages.cpp | 模型目录导入校验、原子落位与应用内删除 |
| agents | src/agents/ | 原生活动事件、来源健康、桥接和每 Agent adapter/hook 管理 |

store 实现按职责拆分：store.cpp 为本地 CRUD，store_live.cpp 为 live writer、
切换/探测/恢复和 dsh，store_import.cpp 为导入，store_zcode.cpp 为 ZCode 增量
操作，store_kimi.cpp 为 Kimi Code 的 config.toml 增量读写。
ProviderStore 不强制单例，UI 全局实例是 common.cpp::providerStore()，测试独立实例化。

## 数据模型定位

- ToolSpec 的 needsModel/hasApiFormat/hasModelMappings 驱动表单；additiveProviders
  标记 dsh/ZCode 的多供应商并存模式，两列分别展示 live 与本地留存。
- AppConfig.groups 以注册表 id 为键，兼容旧顶层 claude/codex；路由总开关、端口、
  故障转移和 routerTools 持久化；Provider 保存地址、密钥、model/modelFetchUrl、
  三档模型映射、models 清单、协议/URL 模式、用量开关/间隔/查询字段。
- dsh 的 reasoningEfforts/inputModalities/contextWindow/maxTokens 是 per-provider
  声明；reasoningLevels 为规范升序表，reasoningLevelLabel 和位掩码互转复用 models。
  模态表为 text/image，text 是底座；容量 parseTokenCount/formatTokenCount 接受 K/M。
- officialVendorName 决定官方常驻卡；builtinPresets 是第三方模板，
  builtinPresetIconName 决定厂商资源；usage 模板数据在资源包和 dataDir 用户覆盖内，
  parseUsageTemplates/suggestUsageQuery 是纯解析匹配，规则不硬编码进 UI。

## UI 与平台入口

- src/app.cpp 声明 Application，application_hooks 安装应用级服务，默认不创建
  控制中心窗口；窗口按启动模式和用户操作创建，尺寸来自 ControlCenterWindowOptions。
- src/ui/app.cpp 管理主题、AmbientGlow、标题栏莲花径向导航、IndexedPages、托盘
  和关闭策略；pet*.cpp 管理桌宠、Shimeji/Live2D/莲花球与信息卡工作台，
  pet_model_settings.cpp 负责模型导入、选择与删除。当前桌宠路线和完成度
  查 [产品路线图](../../../docs/desktop-pet-roadmap.md) 与
  [迁移计划](../../../docs/pet-workbench-migration-plan.md)。
- agent_page 为工具栏/Pager/表单宿主；providers_page 为供应商列表，provider_* 为
  卡片、表单、用量和网络实现；providers_dsh/zcode 为双列页。
- router_page 提供 routerInstance 单例与开关/日志，router_transport 桥接平台 HTTP；
  stats_page 为统计和 SQLite 编排；mcp/skills/sessions/settings/about 页面分别承载管理。
- common 提供岛屿原语、PageScaffold/PageSection/QuietCard、对话框、图标映射和 store；
  theme_colors.h 是主题唯一入口，Windows 安装器共享；ui.h/app.h 为内部声明。
- platform/{linux,windows,macos}/main.cpp 是 GUI 入口，无 CLI 分流；单实例协调
  在 src/single_instance.*，通过应用级任务将激活投递到 UI。入口 RAII 停止协调器。
  控制中心默认 1080×720、最小 800×600，采用 Custom chrome。

## 更多契约

按改动读取 [共享配置](configuration.md)、[模块服务](services.md)、
[持久化](persistence.md) 和 [已知取舍](known-tradeoffs.md)；
UI 交互与布局细节见 [UI 技能](../../llmswitch-ui/SKILL.md)。
