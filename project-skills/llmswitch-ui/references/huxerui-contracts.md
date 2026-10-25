# HuxerUI 项目契约

下文代码路径均相对仓库根目录。框架 API 以当前实际源码/SDK 公开契约为准；
本文件保留 llm-switch 的集成约束与已验证易错点，不替代框架技能。

## 来源与声明

- 构建来源按 CMakeCache、配置输出与顶层 CMake 确认，选择规则见
  [构建参考](../../llmswitch-build/references/build-and-ci.md)。
  源码模式读当前源码树 `skills/huxerui-app-development/SKILL.md`；SDK 模式读
  该 SDK 的 `share/huxerui/skills/huxerui-app-development/SKILL.md`。
- 按任务查该版本 references 中的 dsl-style、components、fundamentals、
  layout-and-ui、navigation-and-window；不混用上游不同版本或旧本地副本。
- UI 层使用普通 `.cpp`，不要 `.cppm`：hcg 只扫 `.cpp/.cc/.cxx`。
  composable 不加 inline；入口在 `src/app.cpp`、`src/ui/app.cpp`。
- composable 体内不能有条件编译，hcg 不支持穿行的 #ifdef；宏先在文件作用域
  展开为常量，再供组合使用，例如设置页的版本号文字。
- View 按值传递，具名 View 在 `.With` 等右值限定链式调用前用 std::move；
  临时链与返回值不机械加 move。页面根 Grow(1) + CrossAlign(Stretch)。
- 固定占位不要用 Spacer().With(Frame)：Spacer 自带 Grow(1) 会平分空间；
  改用空 Row/Column。TreeView<Node>(roots, factory, item_info) 放在有界垂直视口，
  节点变化经状态重组声明反映。

## 状态、服务与线程

- UseState/UseTheme 等只能在 composable 函数体调用；普通函数中的 dialog 工厂
  lambda 也不行。把内容拆成 composable，工厂只转发，供应商表单可作参考。
- 受控值以应用状态为权威；State::Get() 只读，修改走赋值写回；TextField 保留
  TextEditingValue，动态兄弟使用稳定 Key（如 provider id）。
- State<T> 需要 operator==。RequestLog 刻意没有该运算符，不存进
  State<vector<RequestLog>>；使用自定义可比较快照或 StateList（见 router_page）。
- State 仅在 UI 线程读写。ProviderStore 无内部锁，UI 侧 CRUD、切换和导入导出
  直接在回调执行；FilePicker async 恢复点本就在 UI 线程。
- 模型列表、用量、连通检测统一用 HuxerUI HttpClient 异步请求；旧 net 阻塞接口
  已删除。会话扫描、导出、删除、详情分页通过 RunWorker，恢复后才写 State。
  不从 Clash-Flux 复制 task_bridge，它早于 SDK 的 RunWorker。
- 剪贴板从 composable 的 UseApplication().Clipboard() 获取，事件回调可捕获
  service 并同步调用 IsAvailable/ReadText/WriteText，不在 worker 线程调用。
- 写库操作后共享 revision +1，包括托盘切换和设置导入，以驱动重读与菜单重建。
  全局实例在 `src/ui/common.cpp::providerStore()`，首次访问 load。

## 纯导航与窗口退役的不同边界

- 供应商新增、编辑、用量配置的进入、取消和返回直接写 formTarget。
  State 通知只是标记重组并请求后续帧，不同步卸载点击节点；禁止为了纯导航
  套 TaskScope::Launch + Delay(0)。Linux 低优先级 idle 队列可能被持续绘制推迟。
  调度公平性曾报告 HuxerUI issue #144；升级 baseline 后需要重测饥饿风险，
  但纯导航直写规则并不以是否发生饥饿为条件。
- 窗口隐藏/退役会同步失效指针派发对象，不能套用上面的导航规则。
  自定义关闭按钮、窗口内菜单隐藏、桌宠形态切换等涉及当前点击窗口的操作，
  拦截 OnCloseRequest 或相关回调并通过 TaskScope::Post 延后执行。
  参考 `src/ui/app.cpp` 的关闭/最小化拦截与 `src/ui/pet.cpp` 的应用级任务。

## Runtime 与窗口生命周期

- 一次性注册放 AppOptions::application_hooks，不能放 composable。
  SystemTrayHandle::OnActivate 在 Runtime 生命周期只允许单个处理器，
  ApplicationContext::Provide 按类型拒绝重复，重组时重复注册会 terminate。
- 应用 hook 中 Provide 服务并注册一次激活处理器；组合期使用 UseService，
  在 Lifecycle 里绑定/解绑当前 WindowHandle。不得把组合期窗口上下文捕获为
  永久激活目标。相关入口为 `src/ui/app.h`、InstallApplication 与托盘 presentation；
  服务名与实现以当前文件为准，不复制旧版 TrayActivationTarget 实现。
- TaskScope、订阅、动画与窗口回调须随生命周期取消/解绑，返回可逆 disposer。
  平台/线程资源约束还见 [RAII 参考](../../llmswitch-build/references/cpp-resources.md)。
