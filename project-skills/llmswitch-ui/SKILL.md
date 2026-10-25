---
name: llmswitch-ui
description: 修改 llm-switch 的 HuxerUI 页面、桌宠窗口、状态交互、主题和资源时使用。
---

# llm-switch UI 开发

根 [AGENTS.md](../../AGENTS.md) 定义全局边界；本技能承接 UI 专项约束。

## 开工与阅读路由

1. 从 CMakeCache、配置输出与顶层 CMake 确认实际 HuxerUI 来源。
   源码模式读取该源码树的 `skills/huxerui-app-development/SKILL.md`；
   SDK 模式读取其 `share/huxerui/skills/huxerui-app-development/SKILL.md`。
   以当前版本的公开 API 为准，不拿被忽略的旧本地 skill 副本作依据。
2. 所有 UI 代码修改先读 [HuxerUI 项目契约](references/huxerui-contracts.md)，
   再按布局、状态、任务或窗口主题读实际框架技能对应 references。
3. 改布局、主题、窗口、图标或预设资源，必读
   [视觉与资源契约](references/presentation.md) 和
   [资源来源与许可](../../resources/README.md)。
4. 改供应商表单、卡片、用量刷新、Pager 或会话页，必读
   [页面行为](references/page-behavior.md)。dsh/ZCode 双列页还必读
   [性能预算](references/provider-performance.md) 与领域技能对应工具契约。
5. 改桌宠、信息卡、活动展示或窗口迁移，必读
   [桌宠路线图](../../docs/desktop-pet-roadmap.md)；页面迁移另读
   [迁移计划](../../docs/pet-workbench-migration-plan.md)，活动来源另读
   [集成契约](../../src/agents/README.md)。保留旧入口，区分计划与实现。

## 实施约束

- UI 用普通 `.cpp` 走 hcg；composable 不加 inline，体内不放条件编译。
- UseState/UseTheme 等只在 composable 中调用，普通 dialog 工厂只转发内容。
- 受控值以应用 State 为权威；Get 只读，修改写回；动态兄弟使用稳定语义 Key，
  TextField 保留完整 TextEditingValue，View 按值传递、具名链式调用按需 move。
- 纯 State 导航直接写状态；不要用 Launch + Delay(0) 包装纯导航。
  当前点击窗口的隐藏、关闭退役或形态切换经 Post 延后，不同步销毁窗口。
- State/Clipboard 只在 UI 线程操作；网络走平台异步请求，会话 IO 走 RunWorker，
  重复加载丢弃过期结果，离开页面取消任务。不要复制旧 task_bridge。
- Runtime 级注册只在 application_hooks 一次完成；组合内用服务与 Lifecycle
  绑定/解绑当前窗口，返回清理闭包，不捕获组合期窗口作为永久激活目标。
- 写库后递增共享 revision；隐藏页和表单不查询供应商用量，路由/统计轮询只在
  对应顶级导航可见时运行。不能以性能优化删除管理能力。

## 验证与交付

交互修改检查进入、返回、取消、错误和隐藏页行为；布局检查最小 800×600 视口，
长文本不能挤掉操作按钮。列表改动检查虚拟化、稳定身份和异步加载结束态。
按根规范完成编译、测试、规模检查、diff 检查、提交与推送。
