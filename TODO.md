# TODO

## 供应商增量模式（dsh / zcode）

工具的 live 配置本身是「多供应商并存 + 一个默认指向」，切换只该改指向，
增删改该是增量的；本应用另有一份自己的留存清单，两边会各自漂移，所以页面
按左右两列对照（左 = live 实况、右 = 本地留存），逐条可写入/收编。

- [x] dsh：`~/.dsh/settings.yaml` 的 llm-pi-ai.providers **单条增量**
  （`writeDshProvider` / `adoptDshProvider` / `removeDshProvider` / 增改复制删 /
  切换各只动自己那一条，`syncDshProviders` 整组重建只留给显式的「全部写入
  dsh」按钮），供应商页左右两列 + 「全部写入 dsh」/「全部收编」+ 逐条
  写入/收编/删除；左列在文件里没有内置 deepseek-official 时补一条只读合成行
  （ToolSpec.additiveProviders）。
- [x] zcode：`~/.zcode/v2/config.json` 的 provider map 同一套**单条增量**
  （`writeZcodeProvider` / `adoptZcodeProvider` / `removeZcodeProvider` /
  增改复制删 / 切换各只动自己那一条，`syncZcodeProviders` 整组重建只留给显式的
  「全部写入 ZCode」按钮），供应商页左右两列 + 全部写入 / 全部收编 + 逐条
  写入/收编/删除（删除确认问「只删本应用」还是「连同 ZCode 一起删」）；
  `ToolSpec.additiveProviders` 已打开。与 dsh 的两处差别：默认指向是条目自己的
  `enabled`（互斥只覆盖本应用托管的 llmswitch:*），密钥就在条目里（没有独立
  凭据文档）。**收编不改写 live**：ZCode 原生条目保留它自己的键与内容，写入/
  更新原位合并、保留它自己维护的字段（options 其它键、systemDisabledReason），
  `builtin:*` 不收编不删除。
- [x] 默认指向的动作放进**左列**（两列页共用一条规则）：它改的是 live 自己的
  状态，不要求那一条已被本应用纳管。dsh 走 `setDshDefaultKey`（只改
  agent-default-model，未纳管的手写路由也能设为默认；合成官方行上 = 清回内置
  官方路由），zcode 走 `enableZcodeKey`（只翻 enabled，未纳管的原生条目也能
  直接启用，builtin:* 的启停不代管）；右列只留写入 / 收编 / 删除。
- [x] 收编**不改名**（dsh 跟 zcode 对齐）：`llmswitch-` 前缀只属于本应用自己
  创建的条目，收编进来的原生条目保持它自己的键与 `apiKeyEnv`，`adoptDshProvider`
  只记进本地列表、settings.yaml 一字不动；写 / 切换 / 删除的目标键统一走
  `dshEntryKeyFor`（带前缀的优先，否则原生键即 id）。文件里真有一条
  `deepseek-official` 手写条目时也跟别的条目一样可收编（以前禁止是因为收编会
  改名、把内置路由名从文件里抹掉，这条理由已经不存在）。
- [x] dsh / zcode 双列页卡顿：预取从 480 收到 128，行内「键 · 模型 · 访问地址」
  合成一条 Text（`DshMetaLine` / `ZcodeMetaLine`）；实测一屏挂载节点 dsh
  250 → 201、zcode 256 → 193，静止帧 10.4/11.2 → 8.8/8.6ms（对照页 5.2ms 未变），
  滚动帧均值 17.6/14.5 → 10.1/8.9ms。根因是 linux 后端没有跨帧批次缓存，逐帧
  成本几乎只跟已挂载节点与光栅面积有关；store 读文件不是瓶颈（页面体每次挂载
  只跑一次）。
- [x] 右列「写入 / 更新」换自绘图标 `write.svg`（文件 + 从边界插进去的一条
  墨块 = 只写这一条）：Icon Set v1.0 的 `upload.svg` 是「底座 + 竖箭头」，
  24px 下认不出语义，也和整组写入的批量动作混同。

## AI 桌宠

大计划与分级明细见 `docs/desktop-pet-roadmap.md`（定位：桌宠是默认入口，管理窗口是
控制中心；P0 启动/迁移规则 + 原生 Agent 活动接入 + Live2D 交付 → P1 信息卡成为
日常工作面板 → P2 快捷提问 → P3 个性与扩展）。此处只留批次勾选：

- [x] 已有基础：`LocalRouter::inflight()` + Poll 按 tool 聚合 +
      完成提醒状态机（批次结束静默 ≥8s 挂 ✓，点击/新批次消解）。
- [x] 已有基础：streaming 呼吸参数、活跃 Agent 浮标（≤2 轮换）、
      ✓ 标记与点击分流、右键消解入口。
- [x] 已有基础：petCompact 胶囊模式 + owner 锚定卡片
      （各 Agent 今日请求/tokens/当前供应商）+ 跳转主窗页面（MainNavTarget）。
- [ ] P0：桌宠优先启动、旧配置迁移、单实例/托盘唤回与故障回退。
- [ ] P0：Claude Code / Codex 本地活动来源 spike；不依赖路由也能识别工作状态。
- [ ] P0：Live2D 许可、模型随包策略与 Linux/Windows/macOS 实机交付验收。
- [ ] P1：信息卡从统计卡扩展为日常活动面板，覆盖来源状态和最近完成/失败。
- [ ] P2：信息卡快捷提问、流式回复与可配置提示词；不执行终端命令。
- [x] 已有基础：Live2D 构建选项、配置、渲染主体、设置页/信息卡切换与莲花回退；
      Linux GLES 3.1/VBO 修复已进入 Lib-Live2D fork，剩余发布验收见路线图 P0。

## 发布与分发

- [ ] 为 Linux RPM/DEB 发布包加入正式签名与验证链路：使用 CI 专用签名密钥，
  发布公钥与指纹，并补充用户导入、校验和仓库 `gpgcheck` 配置文档。
