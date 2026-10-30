---
name: llmswitch-domain
description: 修改 llm-switch 的供应商配置、live 写入、路由、MCP、技能同步、会话和用量领域行为时使用。
---

# llm-switch 领域开发

根 [AGENTS.md](../../AGENTS.md) 定义全局边界；本技能承接领域行为与数据契约。

## 按任务读取

- 首次定位模块或修改跨层职责时读 [架构与定位](references/architecture.md)。
- 改 Provider、AppConfig、格式归一、探测、恢复、导入或测试路径时，必读
  [共享配置契约](references/configuration.md)；live 写入和收编还须读下面对应工具。
- Claude Code/Desktop、Codex、OpenCode、Pi、Hermes、Kimi Code：读
  [live 工具契约](references/live-providers.md) 的相应章节。
- dsh 的增删改、切换、收编、凭据、YAML 或模型能力：必读
  [dsh 契约](references/dsh.md)，不得改成隐式整组重建。
- ZCode 的启用、写入、收编或模型清单：必读 [ZCode 契约](references/zcode.md)。
- 改路由、MCP、技能同步、会话或账本边界：必读
  [模块服务契约](references/services.md)。涉及原生活动还读
  [活动集成契约](../../src/agents/README.md)。
- 改文件落盘、备份、日志、SQLite 或扫描位点：必读
  [持久化选择](references/persistence.md) 与共享配置中的文件安全规则。
- 修改受支持工具、解析容错、MCP 重写或并发关系前，必读
  [已知取舍](references/known-tradeoffs.md)，不把现有取舍随手改掉。
- 领域修改涉及页面行为时，同时加载 [UI 技能](../llmswitch-ui/SKILL.md)。

## 实施顺序与不可丢失的语义

1. 先确认修改触及的是本应用配置还是工具实际读取的 live 文件。
   ProviderStore 无内部锁，UI 侧独占；测试可独立实例化，不强制单例。
2. 先读工具当前配置和对应 writer/import/detect/restore 实现；live 修改备份后
   原子落盘，保留无关字段、原生键和其它模型，收编与切换不能混为同一操作。
3. apiFormat、upstreamFormat、容量和能力字段复用 models 归一函数，兼容旧配置；
   空清单和 0 按“不声明”，不猜默认能力；测试覆盖保存、写入和读回。
4. dsh/ZCode/Kimi Code 每次操作只动自己那一条，原生条目收编不改名、不改写；删除区分仅本地
   与连同 live。当前选择在左列，批量同步只走明确的 sync 入口。
5. 路由禁用在 resolver、上游和统计之前短路；旧 routerTools 缺字段默认全开，
   显式空数组保持全关。逐请求日志不迁入 SQLite 队列。
6. 探测不改写配置；Pi 必须尊重 defaultProvider 指向，不从历史条目误判当前项。
   严格 JSON 解析失败不得破坏用户文件，具体被动读取边界见参考。

## 验证

领域行为变化必须补充或更新对应测试，全部真实路径通过环境变量覆盖到临时目录。
优先覆盖与本次变化相关的原生字段保留、增量边界、坏输入和迁移场景。
网络纯解析测试不访问真实网络，路由测试用假上游；清理测试文件用 RAII。
按根规范完成完整编译、ctest、规模与 diff 检查、本地提交及推送尝试。
