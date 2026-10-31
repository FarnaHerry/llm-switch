# 供应商、用量与会话页面行为

下文代码路径均相对仓库根目录。数据写入规则另读领域技能对应工具契约。

## 页面壳与导航

- 控制中心顶级 8 区块使用标题栏正中莲花锚点和屏幕中央径向导航盘，点击写
  navPage，经 IndexedPages 切换；主内容区不再为左侧栏留宽度。
  信息卡复用管理功能，迁移规则见既有桌宠专题文档，不删除控制中心入口。
- 所有输入控件统一 M3 outlined：`AppThemed` 主题层设
  `TextFieldStyle.variant=Outlined` 与透明底、1px outline、4px 圆角、聚焦
  primary 2px，Select 也按同一描边语言；调用点仍显式写
  `.Variant(TextFieldVariant::Outlined)`，主题只作兜底。
- AgentPage 是持有 selectedTool、工具栏/Pager 和表单导航的薄宿主。
  新增/编辑与 UsageFormPage 是同一宿主内的第二个 IndexedPages 页面，覆盖
  工具栏与 Pager；表单打开时横向拖动不能切 Agent，返回后保留原列表位置、
  卡片状态与用量时间戳，表单只缩进一次。
- 新增表单的目标选择器使用 toolRegistry，最后一项为“所有 Agent”；切换目标
  保留已填内容，只重置上游格式为目标的原生默认。编辑/用量页不出现选择器。
  formToolIndex 是目标唯一真源，registry.size() 表示所有 Agent，各组独立生成 id。
- 新增页在 formTarget="new" 的首帧直接渲染，不等待编辑数据加载标志。
  新增页换 Agent 的上游默认按当前目标当帧确定，不能在 Lifecycle 里晚一帧重置；
  编辑页则保留已保存格式。普通 dialog 工厂不得创建 State 或调用 UseTheme。

## 供应商卡片与表单

- 官方厂商名非空的工具显示首位官方常驻卡，切换走 restoreOfficial；
  builtinPresets 只放第三方兼容端点，订阅与按量模板入口复用厂商图标。
- 供应商行左信息右操作，选中态由承载底块表示；主模型和三档映射使用行内
  Select，获取模型后共享 fetchedModels，不重新引入旧 ModelPicker 弹窗。
- API Key 使用 TextField 的 TrailingIcon、OnTrailingIconClick 和 Secure；
  眼睛图标仅悬停输入框或已经明文时显示，不另裹 Row 和独立 IconButton。
- 卡片状态文本必须限宽，避免长错误挤掉右侧操作：首行预览最多 26 列、
  Frame.max_width 150，截断时 Tooltip 保留全文；无需截断不挂重复 Tooltip。
  错误首行显示 HttpFailure::Summary 短原因，下一行保留 URL/响应摘要，
  toast 与 Tooltip 提供完整上下文。检查 800×600 和 1080×720 的真实可点击范围。
- dsh/ZCode 每行写入用 write.svg；“当前用哪条”在左列，右列是写入/收编/删除。
  聚合行复用同一按钮但语义是“使用”：写入本地路由条目并把该工具切到聚合项，
  会先启动路由；编辑走 aggregate: 表单。聚合表单勾选供应商后带入其全部已知
  模型，每个模型一行，用拖拽或上移/下移调整兜底顺序；默认模型和模型行显示
  `供应商名/模型` 这一对外 ID，保存时先按供应商内部去重再整组合并。live
  同时存在时删除需区分“只删本应用”与“连同工具一起删”，默认沿用后者。
  两列不可调大 CacheExtent，详见 [性能预算](provider-performance.md)。

## 用量查询与页面可见性

- UsageFormPage 是独立页面，卡片 gauge 入口写 formTarget="usage:"+id。
  用量配置管理开关/间隔和查询字段；普通编辑保存沿用已有用量配置。
- 仅当前 Agent 列表进入或配置变化时检查刷新间隔，隐藏页和表单不查询，离开
  列表取消任务，返回复用未到期缓存；持续停留允许手动刷新。
  关闭或 URL 空不查，间隔 0 仅手动，旧配置默认 10 分钟。
- HTTP 异步 GET 后由 extractByPath 取点分路径/数组下标标量；模板从资源加载，
  用户覆盖和匹配规则见 [共享配置](../../llmswitch-domain/references/configuration.md)。
- 路由/统计定时刷新仅在对应顶级导航可见时运行，State 只在 UI 线程写。

## 会话列表与详情

- 右上角 Agent 图标组与 Agent 管理共用注册表，不建“全部”混合页；Pager 切换
  独立 Agent 历史，只扫描当前 Agent。单次 Worker 批量读取 mtime/size 及每文件
  头尾固定大小摘要，缓存 30 秒，可见行只展示已完成摘要。
- 快速切换/刷新用请求代次丢弃过期结果；重载保留旧列表，刷新图标自转，无文字
  占位；失败也正确结束 loading 并反馈原因。导出/删除成功后才更新或重载。
- 扫描、导出、删除、详情分页全程 RunWorker；详情首次从尾读最近 50 条，定位
  末条，只有实际向上滚动接近顶部才加载更早页，并保持原视口锚点。
- StateList + VirtualList 虚拟化消息，以文件偏移保持稳定身份；不要逐条 Insert(0)
  触发反复重组。进入 UI 的文本上限 16KB，UTF-8 安全截断，全文通过导出获取。
  消息行用无阴影的不透明轻量表面；批次结果一次合并，不退回同步全文件读取。
