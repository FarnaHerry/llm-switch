# llm-switch AI Agent 统一工作规范

本文件是项目内 AI 编程工具的唯一权威规范，适用于仓库根目录及全部子目录。
Claude、Codex、Gemini、Copilot、Cursor、Windsurf 及其他自动化 agent 修改本项目时
都必须遵守。各工具专用入口只能引用或补充本文件，不得复制出相互冲突的规则；
子目录中的 `AGENTS.md` 可以对其作用域增加更严格的约束。

## 项目与代码边界

- 项目是 C++23 modules + HuxerUI 桌面应用；UI 普通源在 `src/ui/*.cpp`，领域模块
  在 `src/*.cppm` 与对应实现文件。不要引入 Electron、数据库或新的大型依赖来
  绕过现有架构。
- C++23 命名模块约定：`.cppm` 命名模块 purview 中定义在 class 体内的成员函数
  不继承 `#include` 头文件在全局模块中的隐式 `inline` 规则；如果成员函数定义
  保留在模块接口中且希望作为接口内联函数使用，必须显式声明 `inline`
  （`constexpr` / `consteval` 除外）。普通命名空间函数不因 `.cppm` 或头文件
  自动 inline，不得全量添加 `inline`；`inline` 也不保证实际内联替换，性能判断
  需结合编译器、LTO/IPO 和反汇编验证。
- 开始修改前先读与任务相关的现有代码和文档。HuxerUI 开发细节、架构和已知取舍
  见 `CLAUDE.md`；图标资源规则与来源许可见 `resources/README.md`。
- 修改 HuxerUI 应用 UI 前，必须读取当前实际使用版本的
  `huxerui-app-development` skill 主文档，并按任务查阅其 `references/`；源码模式
  优先读取 `third_party/huxerui/skills/huxerui-app-development/SKILL.md`，SDK 模式
  读取当前 SDK 的 `share/huxerui/skills/huxerui-app-development/SKILL.md`，实现以该
  版本的公开 API 约定为准。
- 保持用户已有改动，不顺手重排、格式化或重写无关代码。领域行为改变必须补充或
  更新相应测试。
- HuxerUI 源码优先来自 `third_party/huxerui`，CI 固定版本见
  `.github/workflows/build.yml`。当前基线 `0c51262` 包含 `64264cb` 的 Linux 有界
  LRU 文本布局缓存，以及上游 Application/Window 所有权重构和跨平台 HTTP
  流式请求修复；
  剪贴板只在 UI 线程通过
  `UseApplication().Clipboard()` 使用，TreeView
  必须位于有界垂直视口。
- 会话目录扫描、导出和删除属于文件 IO，必须通过 `RunWorker` 离开 UI 线程；
  可重复触发的加载必须以请求代次丢弃过期结果，并在成功或失败时正确结束加载态。
- 本地路由包含总开关和逐 Agent 代理开关。逐 Agent 选择必须持久化到
  `AppConfig.routerTools`，旧配置缺字段时默认全部启用；运行中切换应即时生效，
  被禁用路径不得访问 resolver、上游或写入请求统计。

## 资源与 RAII 硬约束

- **凡是获取后必须释放的东西，一律由 RAII 所有者包裹**，不得在函数体里裸拿
  句柄再靠每条分支手写 `close` / `pclose` / `free` / `join` / `RegCloseKey`。
  覆盖范围：POSIX fd（`open`/`socket`/`accept`）、`FILE*` 与 `popen` 管道、
  平台句柄（Windows `HKEY`/`HANDLE`）、线程、互斥量与锁、临时文件、以及
  HuxerUI 的 `TaskScope` / 订阅 / 动画句柄。
- **裸 `new` / `delete` / `malloc` / `free` 一律禁止**：用 `std::unique_ptr` /
  `std::shared_ptr` / 标准容器；Pimpl 用 `std::unique_ptr<Impl>`（见
  `LocalRouter`、`UsageStore`）。
- **锁只用 `std::lock_guard` / `std::unique_lock` / `std::scoped_lock`**，不手写
  `lock()` / `unlock()`；需要条件变量时用 `std::unique_lock` + `wait*`。
- **平台 API 用带自定义 deleter 的 `std::unique_ptr` 或一个极小的 Guard 类**，
  例如 `config.cppm` 的 `UniquePipe`（`popen` → `pclose`）与 `UniqueRegKey`
  （`RegOpenKeyExA` → `RegCloseKey`）；不要为它们引入堆分配以外的运行时开销。
- **获取失败与异常路径都不能泄漏**：先构造所有者，再用 `Valid()` / `operator bool`
  判断；错误分支直接 `return`，由析构负责释放（`src/single_instance.cpp` 的
  `UniqueFd` 是范本）。
- **线程必须由持有者负责停与 join**：持有者析构里 `stop()` + `join()`，或用一个
  析构即停的 RAII 包装；新起的线程不得无人 join。已有范本：
  `LocalRouter::Impl::~Impl`、`single_instance::Coordinator::~Coordinator`、
  测试里的 `FakeUpstream::~FakeUpstream`。线程创建本身失败也要回滚已获取的状态
  （见 `LocalRouter::Impl::start`），不留「已运行但无人监听」的假象。
- **临时文件用守卫**：`<file>.tmp` 这类半成品在成功 `rename` 后显式 `Release()`，
  失败/提前返回/抛异常时由析构删除，不在用户目录留孤儿（`TempFileGuard`，
  见 `src/store.cpp`、`src/mcp.cpp`、`src/skills.cpp`、`src/router.cpp`、
  `src/usage.cpp`）。
- **不要用「文档里写着记得关」代替 RAII**：漏掉一条错误分支就是泄漏；新增资源
  类型时先写所有者，再写使用它的代码。

## UI 视觉硬约束

- 总体风格是冷调石板的“岛屿风”：环境层只有一层主题派生的程序化光晕，内容
  仍以轻量、分层、可读的岛屿组织，不把环境装饰压到交互内容之上。
- **顶级页面没有自己的卡片**：页面与标题栏共用同一块窗口表面（`PageScaffold` /
  `AgentPage` 只负责内边距与滚动，不画背景与圆角），应用名与页面标题共享同一条
  左右边线（壳层 `shellInset` 必须与页面内边距取同一个值）。纵向同样不留缝隙：
  页面顶部内边距为 0、壳层标题栏与页面之间不声明 `Spacing`，页面内容紧接标题栏
  下沿；标题栏高度由平台解析（`WindowTitleBar` 按平台最小值测量），不得用硬编码
  高度去"对齐"页面。层次感由
  `AmbientGlow` 环境光 + 下面分层的分区与卡片表达。
- 卡片克制使用：页面常规分区用 `PageSection`（标题 + 内容平铺在页面表面上）
  配 `SectionDivider` 发丝线划分，不把一页串成一列盒子；重复列表条目
  （供应商/技能/MCP/会话行）用无边框 `QuietCard` 靠表面色差分层。通用
  `Card`（主题 raised 表面、6pt 圆角、1pt 语义描边、内边距的单层轻量样式）
  只用于少数需要强调的独立块；弹窗保留更强描边与阴影以保证浮层识别度。
- 圆角：浮层/弹窗 10pt、二级岛 6pt；颜色必须来自 `ResolveIslandTheme(theme)`
  等语义主题层，不在页面中散落硬编码明暗色。
- 强调色（`primary`）只用于可交互状态（按钮 / 选中 / 开关 / hover 描边与辉光）；
  分组标题、正文等非交互文字用 `on_surface` / `on_surface_variant`，不随主题变蓝。
- 辉光只用分层描边与阴影表达（SDK 无模糊滤镜）；标题栏悬停可由多层同心环 +
  低透明度宽描边 + 主题阴影构成。
- 主题只用太极图形选择器，不显示“深色/浅色/跟随系统”等文字提示。阴阳平衡表示
  跟随系统，深色外环突出表示深色，浅色外环突出表示浅色；点击循环切换
  `system → dark → light`。悬停时持续旋转，移出时冻结在当下角度并保留状态。
- 保证交互可辨识、文本对比度和响应式布局；环境装饰不能牺牲信息层级或功能识别。

## 修改完成后的固定流程

每一批用户要求的修改完成后，按以下顺序收尾：

1. 使用 Ninja 配置并运行完整编译：
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`，然后运行
   `cmake --build build --parallel 4`。项目默认生成器固定为 Ninja；如果已有
   构建目录使用其他生成器，不要原地切换或删除，改用新的构建目录。
2. 运行相关测试；默认至少运行 `ctest --test-dir build --output-on-failure`。
3. 运行 `git diff --check`，检查空白错误和补丁格式。
4. 只暂存本批任务范围内的文件，创建说明清楚的本地 Git commit。
5. 尝试将当前分支推送到其上游；没有上游时使用
   `git push -u origin <当前分支>`。
6. 如果推送失败，不回滚、不删除本地 commit，也不反复索取权限；保留本地提交，
   在最终回复中说明失败原因。推送成功时报告 commit ID 和目标分支。

**推 main / 开 PR 都会触发三平台构建**：只有 `docs/**`、`**.md` 这类纯文档改动因
`paths-ignore` 不触发——所以"提交完没看到 CI"要先确认这次提交是否只动了文档，不要
据此以为 CI 被关掉了。无论有没有 CI，第 1、2 步的本地编译与测试都是本仓库的门禁。

编译或测试失败时不得声称任务完成。应先修复本次修改引入的问题；若失败来自明确
的外部环境限制，则保留安全的本地改动并如实报告。

## Git 安全边界

- 不覆盖或丢弃用户已有修改，不使用 `git reset --hard`、`git checkout --` 等
  破坏性命令。
- 提交前检查 `git status --short` 和 diff，避免夹带无关文件、密钥、构建产物。
- 默认不改写历史，不使用 `push --force`；除非用户明确要求，否则不执行变基或
  amend 已公开提交。
- 用户明确要求的“每次保存”指创建本地 commit，不只是保留未提交工作区修改。

## UI 资源约束

- 24×24 功能图标遵守 `resources/README.md` 的单套无色图标规范；深浅模式
  由运行时 tint 自适应，禁止维护 `_selected/_dark/_light` 重复资源。CMake 会
  在配置期检查 `#FFFFFF` alpha-mask 与 `_selected` 重复资源。
- 太极主题选择器、轻岛屿表面和卡片规范见 `CLAUDE.md` 的“UI 硬约束”。
- 图标表现必须通过项目的 `IconButton`/主题 tint 管线完成，不为深浅模式复制资源，
  不依赖 SVG 自带的固定前景色。
