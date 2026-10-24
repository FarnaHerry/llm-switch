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

**双向理念（2026-10-24 补，OpenPet 思路的引申）**：桌宠不只是 Agent 状态的
**出口**（数据中心），也是用户向 Agent / 供应商**发话的入口**——桌宠是
用户与 Agent 之间往返的最短路径。左侧（输出）：路由流量 → 状态/浮标/
信息卡；右侧（输入）：快捷提问与快捷指令经本地路由直达当前生效供应商，
无需打开主窗、无需打开 CLI。P2 的「输入媒介」组即这一理念的落地。

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

- [x] `LocalRouter` 增加在途请求计数 `inflight()`：请求进入转发时 +1、
  完成（成功/失败/中止）时 -1；独立于统计锁的 atomic 计数 + RAII 守卫
  （含 AbortInFlight 中止与异常出口），`test_router` 增加可阻塞会话断言
  （在途=1、完成归零、stop 中止归零、校验拒绝不计入）。
- [x] `PetController::Poll` 改造：读 `inflight()`（真 streaming 态）+
  `recentLogs(24)` 按 tool 聚合（活跃 Agent 列表 ≤2、最近 60s 窗）；
  thinking 猜测窗退役，首个 Poll 只对齐日志水位（历史日志不触发提醒）。
- [x] 完成提醒：Poll 维护「活动批次」状态机（连续有流量 → 静默 ≥8s 视为
  批次结束）；批次内含成功请求则挂 done-pending-ack，点击桌宠或新批次
  开始时消解。批次内全部失败则只走 error，不挂 ✓。

验收：test_router 全绿（含 5b 在途计数三路径）；桌宠在真实 CLI
（claude-code + codex 同时跑任务）下状态切换正确、✓ 消解路径手测——
**手测项待实机验证**（本批合入后首个版本发布前过一遍）。

**P0-2 桌宠视觉与交互承接（UI 层）**

- [x] Orb 绘制扩展：streaming 态呼吸参数随在途状态切换（空闲 ±3.5%/~6s、
  streaming ±6%/~2.5s）；活跃 Agent 浮标（顶缘药丸，`ToolIcon` 冷白 tint，
  ≤2 个）；done-pending-ack 的 ✓ 徽标（Canvas 右上角静态绘制）。
- [x] 点击行为分流：done-pending-ack 时点击 = 消解标记（不再直接开主窗），
  其余状态点击 = 打开主窗口（现有）。
- [x] 右键菜单增「消解完成提醒」入口（有提醒时可用）。
- [x] reduced-motion 下 ✓ 徽标与 Agent 浮标仍显示（静态、不动画）。

验收：深浅主题下角标/浮标/✓ 可读（浮标与角标为固定深色玻璃药丸，
不随主题，静态球体身份一致）；无头探针冒烟暂缓——状态切换由
PetController::Poll 纯逻辑驱动，实机验证随 P0-1 验收一起过。

### P1 —— 信息卡与用量贯通（第三批 + 第四批）

**P1-1 胶囊 / 信息卡双形态（白龙同款精髓）**

- [x] 胶囊模式（AppConfig.petCompact，默认关）：桌宠缩为小胶囊（状态点 +
  今日请求数，状态点随 streaming/failure/done 变色），右键菜单/设置页切换；
  胶囊是同一窗口的另一种组合（小尺寸窗口重建），保留拖拽/右键/点击全套交互。
- [x] 信息卡：点击（胶囊或球）在桌宠旁展开锚定卡片窗口（owner =
  桌宠、随桌宠退役，工作区夹取定位）：各 Agent 今日请求 / tokens / 失败数
  （Poll 按本地 0 点聚合 recentLogs）与当前生效供应商，再点收起；卡片遵循
  应用主题（深浅随 Poll 跟随）。完成提醒挂起时点击仍是消解提醒。
- [x] 卡片行 / 底部按钮点击 → 激活主窗口并跳转对应页面（Agent 管理页 /
  统计页）：新增应用级 `MainNavTarget` 服务（TrayActivationTarget 同模式，
  主窗口 Lifecycle 绑定 navPage）。

验收：三平台窗口行为（置顶、锚定、收起）——CI 三平台构建回执 + 实机
验证随下个发布版；卡片数据与统计页一致（同一 recentLogs 聚合口径）。

**P1-2 用量面板贯通**

- [ ] 信息卡接入用量账本（`stats_page.cpp` 的 SQLite 查询先例，读路径
  只读复用）；白龙「今日 24h 曲线」降级为「今日请求数 + tokens 汇总 +
  每 Agent 一行」，不做图表（首轮）。
- [ ] （可选）供应商 usage 端点数据上卡片：仅 `Provider.usageEnabled` 的
  供应商，复用现有轮询缓存，不新增请求。

### P2 —— 伙伴感与对外能力（远期，按需启动）

- [ ] **输入媒介 · 快捷提问（闲聊）**：经本地路由打到当前生效供应商
  （OpenAI 兼容端点现成）；输入框放信息卡，流式回显（框架 HttpClient
  流式）。「向 Agent 发话的最短路径」——不打开主窗、不打开 CLI。
- [ ] **输入媒介 · 快捷指令**：用户可配置 prompt 模板（AppConfig 持久化，
  设置页管理），桌宠右键菜单/信息卡一键把模板发给指定 Agent 的路由端点，
  回显进信息卡并与该 Agent 的活动浮标联动（发出后能看到它开始 thinking）。
- [ ] 主动搭话（Desk-Pet 借鉴）：**必须带预算与静默时段**（每天 N 条、
  静默时间段、忙碌检测=streaming 时不打扰），默认关闭；素材仅限路由侧
  事件（「今天 codex 跑了 47 个任务」），不做屏幕/剪贴板感知。
- [ ] 对外事件 API（OpenPet 借鉴，可选，双向）：`POST 127.0.0.1:<routerPort>/pet/`
  让本地脚本/其它 Agent 驱动桌宠说话/换状态；仅本机、默认关。
- [ ] 视频全屏让位（白龙借鉴）：依赖平台级全屏检测，HuxerUI 暂无公开
  API——挂起，等框架能力。
- [~] **Live2D 可选皮肤**（2026-10-24 评估后定为可选，非默认；同日提前
  启动集成）：经 [FarnaHerry/Lib-Live2D](https://github.com/FarnaHerry/Lib-Live2D)
  （钉 `936694b`，`huxerui_use_library` 同 Lib-SQLite 模式）引入。
  已落地：`LLMSWITCH_LIVE2D` 构建选项（默认 OFF，OFF 时 stub 顶替、
  构建零依赖）；`petLive2D` 配置 + 设置页/信息卡切换（禁用构建里开关
  灰掉并说明）；`pet_live2d.cpp` 皮肤主体（扫 `dataDir()/live2d/` 首个
  `*.model3.json` 异步加载，ModelView 渲染，streaming 时 playback_rate
  1.15、Ready 循环 Idle、点命中区播 TapBody；无模型/失败显示占位药丸
  而非静默回退）；窗口 300×420，模式互斥 live2d > compact > 球。
  进行中 → **Linux 首编译已通过**（2026-10-24，build-live2d：Lib-Live2D +
  Cubism Framework + Core 静态库全链路首验，ctest 9/9）。期间修掉三处：
  ① Lib-Live2D v0.1.0 的 Linux 链接漏 OpenGL（Cubism Framework 用 GLES2
  符号但只链 GLEW）——消费侧补链 `OpenGL::GL`，上游修复待 Lib-Live2D
  后续版本；② `LLMSWITCH_LIVE2D_PATH` 本地检出覆盖（GitHub 拉取受限时
  免网络）；③ 性能探针目标排除 pet_live2d 真实现、ON 时补 stub 符号。
  待办：实机运行验证；Windows 验证待 CI；CI 接入需先解决 Cubism SDK 的
  获取（官方 CDN 对不稳链路不友好，需缓存 artifact 或文档化手动步骤）。
  **内置示例已接入**（2026-10-24）：构建期把 Lib-Live2D preview 的官方
  示例 Hiyori + model_licenses 暂存进 `resources/raw/live2d/`（gitignore
  不入库，选项关闭时移除），加载优先级 = 用户 dataDir 模型 → 内置 Hiyori →
  占位提示；RawResource 开启器在组合期解析成 map（`UseRawResource` 是
  组合期 API，回调内不可调用——preview 同款模式）。资源包 +4.8MB 仅在
  ON 构建存在。许可注意：模型与 Cubism SDK 不入 git、不随发布包分发
  （发布包是否带示例待许可确认，当前仅本地构建）。
  背景评估（2026-10-24）：成本基线——Cubism Core 每平台 ~1-3MB + 模型
  磁盘 5-15MB / 显存 16-64MB（2048² RGBA 纹理即 16MB），参数/物理/姿态
  每帧 ~1-3ms 且持续更新（待机呼吸不静止）；同等内容下仍远省于帧序列
  路线。皮肤架构——默认始终是程序化莲花（零资产零额外库），Live2D 为
  可选档，状态语义/交互/数据中心能力与莲花完全同构（换皮不换脑）。

### 非目标（明确不做）

- **帧序列位图动画素材**：几十上百 MB 的帧缓存路线（白龙同款）成本
  高于 Live2D 且不可参数化，不走；程序化 Canvas 绘制仍是默认皮肤。
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

- 2026-10-24：**信息卡自定义标题栏 + 小窗菜单裁剪修复**。信息卡改
  Custom chrome + 24pt `WindowTitleBar`（与主应用/安装器统一，AGENTS.md
  立为硬约束）；卡片内容色从写死深色改为跟随主题。实机暴露框架约束：
  `MenuHandle::ShowAt` 是窗口内浮层、被窗口边界裁剪——球形态菜单精简为
  5 项无分隔（≈188pt 恰好装下 200×200），胶囊态右键改为展开信息卡，
  信息卡补「胶囊/球形态」切换按钮（胶囊回球的主入口）。
- 2026-10-24：**P1-1 完成**。胶囊模式（petCompact，状态点变色 + 今日次数，
  小尺寸重建）；信息卡（owner 锚定窗口，每 Agent 今日请求/tokens/失败数 +
  当前供应商，点击桌宠展开/收起）；`MainNavTarget` 应用级导航服务（卡片
  跳页 + 激活主窗）。轻量级退役/隐藏同步处理信息卡（双句柄代次）。
  store 往返测试覆盖 petCompact。
- 2026-10-24：**P0-1 + P0-2 完成**。`LocalRouter::inflight()`（atomic +
  RAII 守卫，AbortInFlight/异常不泄漏）；Poll 重写为真 streaming + 批次
  状态机（静默 8s 挂 ✓，点击/右键/新批次消解）+ 活跃 Agent 聚合（60s 窗、
  ≤2）；UI 承接（✓ 徽标静态绘制、顶缘 Agent 浮标药丸、点击分流、右键
  消解入口）。test_router 增 5b 在途计数三路径断言，9/9 全绿；实机手测
  随下个发布版验证。
- 2026-10-24：立项；调研结论与分级计划成文（本文档）。
- 2026-10-24：Live2D 评估完成（成本基线见 P2 条目）——定为**可选皮肤**
  路线：默认程序化莲花不变，Live2D 运行时下载、不进主安装包；启动前置
  是 Lib-Live2D 的 Windows/Linux 平台验证、Cubism 许可评估与
  ExternalTexture 透明表面集成 spike。
- 2026-10-24：补入**双向理念**（OpenPet 思路引申）：桌宠既是 Agent 状态
  的出口（数据中心），也是用户向 Agent 发话的入口（输入媒介）；P2 的
  快捷提问/快捷指令/对外事件 API 归入「输入媒介」组落地。
