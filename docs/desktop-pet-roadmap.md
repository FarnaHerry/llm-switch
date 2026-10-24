# AI 桌宠路线图（desktop pet roadmap）

> 状态：规划中（2026-10-24 立项）。本文档是 AI 桌宠演进的唯一计划来源，
> 按批次推进；每批完成后在此勾选并补记实测结论。TODO.md 只保留索引。

## 1. 背景与定位

llm-switch 的桌宠（`src/ui/pet.h` / `pet.cpp`，v0.3.8 起随应用发布）目前是
「透明玻璃球 + 呼吸莲花」：每秒轮询本地路由统计，最近 3s 有请求完成则呼吸
加深加快（thinking），最近一次请求失败 6s 内红脉冲外环（failure），底部
角标显示今日请求数；点击打开主窗口、拖拽移动、右键菜单（主题跟随应用），
支持置顶与轻量级模式（隐藏即退役、唤出重建）。

对标调研（2026-10）后的定位结论：**多 Agent 数据中心桌宠**。

- [codex-bailong-desktop-pet](https://github.com/Wunsch2048/codex-bailong-desktop-pet)
  （本项目主要参照）证明了「桌宠 = AI 工作的数据中心 + 有生命的伙伴」这个
  组合的价值：实时任务状态机（思考/编辑文件/运行命令/等待审批/压缩上下文/
  完成）、持续存在直到点击消解的完成提醒、额度环 + Token 图表 + 日报、
  胶囊/卡片双形态、状态绑定的工作动画。
- 白龙只能读 Codex 一个工具的日志文件；llm-switch 的**本地路由天生是全部
  注册表 Agent（claude-code / claude / codex / opencode / pi / dsh / hermes /
  zcode）的流量入口**，`router::RequestLog` 已含 tool / provider / status /
  latency / tokens——这是白龙做不到的差异化：**一个桌宠看到所有 Agent**。
- 其它参照：[OpenPet](https://github.com/X-T-E-R/OpenPet)（桌宠作为 Agent
  输出设备的事件词汇表：thinking / 工具运行 / 成功 / 失败 / 注意）、
  [V1rtual Desk-Pet](https://github.com/V1rtual-Klavte/Desk-Pet)（主动搭话
  的预算与静默时段）、[Open-LLM-VTuber](https://open-llm-vtuber.github.io)
  （可打断语音、屏幕/剪贴板感知——暂不借鉴，见「非目标」）。

## 2. 状态词汇表（目标形态）

桌宠状态 = f(路由流量)，全部从本地侧推导，不依赖 Agent 配合：

| 状态 | 来源 | 表现（莲花球） |
|------|------|----------------|
| idle | 无在途请求、最近无活动 | 慢呼吸（~6s 周期） |
| streaming（per-tool） | `LocalRouter::inflight()` > 0 | 呼吸加深加快 + 活跃 Agent 名浮标 |
| done-pending-ack | 一波活动后静默 ≥ N 秒 | 「✓」标记持续存在，点击或新流量消解 |
| error | 最近一次请求失败（红脉冲窗内） | 红脉冲外环（现有） |
| hidden | 轻量级隐藏 / 退役 | 无（托盘勾选态跟随，现有） |

「分 Agent 感知」：`recentLogs(N)` 按 tool 聚合出活跃 Agent（白龙同款
最多 2 个、更多轮换的语义），角标与信息卡都按 Agent 分行展示。

## 3. 分级计划

### P0 —— 数据中心核心（第一批 + 第二批）

**P0-1 路由侧真状态（领域层，先行）**

- [ ] `LocalRouter` 增加在途请求计数 `inflight()`：请求进入转发时 +1、
  完成（成功/失败/中止）时 -1；与现有统计同一把锁，`test_router` 增加
  并发在途/异常路径不泄漏计数的断言。
- [ ] `PetController::Poll` 改造：读 `inflight()`（真 streaming 态）+
  `recentLogs(24)` 按 tool 聚合（活跃 Agent 列表、每 Agent 最近请求状态）；
  thinking 猜测窗退役，改为「in-flight 期间 streaming，批次结束后进入
  done 判定」。
- [ ] 完成提醒：Poll 维护「活动批次」状态机（连续有流量 → 静默 ≥8s 视为
  批次结束）；批次内含成功请求则挂 done-pending-ack，点击桌宠或新批次
  开始时消解。批次内全部失败则只走 error，不挂 ✓。

验收：test_router 全绿；桌宠在真实 CLI（claude-code + codex 同时跑任务）
下状态切换正确，✓ 消解路径手测。

**P0-2 桌宠视觉与交互承接（UI 层）**

- [ ] Orb 绘制扩展：streaming 态呼吸参数随在途计数微调；活跃 Agent 浮标
  （最多 2 个小徽标：Agent 图标（`ToolIcon` 现成）+ 呼吸点，多 Agent 轮换）；
  done-pending-ack 的「✓」标记（Canvas 自绘，与红脉冲同层）。
- [ ] 点击行为分流：done-pending-ack 时点击 = 消解标记（不再直接开主窗），
  其余状态点击 = 打开主窗口（现有）。
- [ ] 右键菜单增「消解完成提醒」入口（有提醒时可用）。
- [ ] reduced-motion 下 ✓ 标记与浮标仍显示（静止、不动画）。

验收：深浅主题下角标/浮标/✓ 可读；无头探针（tests/ 模式）可选补一版
状态切换冒烟。

### P1 —— 信息卡与用量贯通（第三批 + 第四批）

**P1-1 胶囊 / 信息卡双形态（白龙同款精髓）**

- [ ] 胶囊模式（AppConfig.petCompact，默认关）：桌宠缩为小胶囊（状态点 +
  今日请求数），经右键菜单/设置页切换；胶囊仍是同一窗口的另一种组合
  （小尺寸窗口重建），保留拖拽/右键/点击穿透外全部交互。
- [ ] 信息卡：点击（胶囊或球）在桌宠旁展开锚定卡片窗口（HuxerUI owner
  window，框架 desktop_pet 示例的 settings 同款机制）：各 Agent 今日
  请求 / tokens / 当前生效供应商（`routerInstance().snapshot()` +
  `providerStore()`，UI 线程读），再点收起。卡片遵循主题与 UI 视觉硬约束
  （PageSection 风格、10pt 浮层圆角）。
- [ ] 卡片行点击 → 激活主窗口并跳转对应页面（Agent 管理页 / 统计页）。

验收：三平台窗口行为（置顶、锚定、收起）；卡片数据与统计页一致。

**P1-2 用量面板贯通**

- [ ] 信息卡接入用量账本（`stats_page.cpp` 的 SQLite 查询先例，读路径
  只读复用）；白龙「今日 24h 曲线」降级为「今日请求数 + tokens 汇总 +
  每 Agent 一行」，不做图表（首轮）。
- [ ] （可选）供应商 usage 端点数据上卡片：仅 `Provider.usageEnabled` 的
  供应商，复用现有轮询缓存，不新增请求。

### P2 —— 伙伴感与对外能力（远期，按需启动）

- [ ] 闲聊：经本地路由打到当前生效供应商（OpenAI 兼容端点现成）；
  输入框放信息卡，流式回显（框架 HttpClient 流式）。
- [ ] 主动搭话（Desk-Pet 借鉴）：**必须带预算与静默时段**（每天 N 条、
  静默时间段、忙碌检测=streaming 时不打扰），默认关闭；素材仅限路由侧
  事件（「今天 codex 跑了 47 个任务」），不做屏幕/剪贴板感知。
- [ ] 对外事件 API（OpenPet 借鉴，可选）：`POST 127.0.0.1:<routerPort>/pet/`
  让本地脚本驱动桌宠说话/换状态；仅本机、默认关。
- [ ] 视频全屏让位（白龙借鉴）：依赖平台级全屏检测，HuxerUI 暂无公开
  API——挂起，等框架能力。

### 非目标（明确不做）

- **帧序列动画素材 / Live2D**：程序化 Canvas 绘制是项目视觉约束
  （参考 AGENTS.md「UI 视觉硬约束」），不引入位图帧包。
- **屏幕感知 / 前台窗口感知 / 剪贴板感知**：隐私边界 + 平台 API 依赖。
- **TTS / 语音交互**：与 llm-switch 的工具定位不符。
- **行走 / 飞行等物理动画**：透明 Utility 窗口拖拽已覆盖核心交互。

## 4. 技术要点与风险

- `LocalRouter` 在途计数：httplib worker 并发调用，注意与统计锁的顺序
  （先减计数再写统计，避免 stop 路径死锁）；`AbortInFlight` 中止的请求
  必须也走 -1。
- 状态机全部在 UI 线程（Poll 每秒一次），`RequestLog` 无 `operator==`
  （刻意），聚合结果要转成有 `==` 的轻量快照结构再进 `State`（AGENTS.md
  关键约定 3）。
- 胶囊/卡片窗口：`OpenWindow` + owner + `WindowIdentity.key` 路由（pet
  同款先例）；轻量级退役必须一并退役卡片（PetController 持双句柄）。
- 用量账本查询是异步 Task：卡片组合期 `UseService` 式按需查一次，
  不做常驻轮询（AGENTS.md 持久化/线程约定）。
- 平台差异：Wayland 无被动显示/置顶回退（已处理）；透明命中测试在
  Linux/X11 与 Windows 语义不同——胶囊模式缩小后注意拖拽区仍完整。

## 5. 批次推进记录

- 2026-10-24：立项；调研结论与分级计划成文（本文档）。
