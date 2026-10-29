# llm-switch AI Agent 统一工作规范

本文件是仓库根目录及所有子目录的统一规范入口，适用于全部 AI 编程工具。
项目技能及其引用承接本文件授权的专项约束；有冲突时以本文件和用户明确指令为准。
子目录 AGENTS.md 可增加其作用域的约束。CLAUDE.md 首行保持 `@AGENTS.md`，
各工具入口只引用规范，不另存相互冲突的副本。

## 文档规模与存放

- 根 AGENTS.md 必须同时满足最多 150 行、6,000 字符；字符按 Unicode 计，含换行。
- 单个项目 SKILL.md 最多 100 行；超限先合并重复内容或外移长细节。
- 禁止压成长行、堆叠语句或删除必要约束来规避上限。
- 项目技能统一放 `project-skills/<技能名>/SKILL.md`，独立文件纳入版本管理。
- 长细节放所属技能的 `references/` 或引用已有专题文档，均由本项目版本管理。
- 根文件明确任务与技能的对应关系；技能说明何时读取哪份参考，不默认加载全部细节。
- 修改规范后运行下方规模检查，并人工核对必要约束、引用、冲突与 Git 跟踪状态。

## 产品与架构边界

- llm-switch 是 cc-switch 的 C++23 模块化重写，以本地 AI 桌宠为默认入口，
  管理窗口是按需打开的多 Agent 控制中心；现有供应商、路由、MCP、Skills、
  会话与统计能力及旧导航入口在迁移期间必须保留，不为桌宠体验删掉成熟功能。
- 桌宠状态汇总路由与用户启用的原生 Agent 来源，不能只依赖可选 LocalRouter；
  来源未接入或不可用时显示 unknown/未接入，不能伪报 idle。
- 活动来源按最小数据读取：未经用户选择不得读取屏幕或剪贴板；桌宠事件只留
  状态所需元数据，不新增会话正文采集或远程上报。
- Shimeji 负责轻量 Q 版帧动画，Live2D 负责高阶模型与交互，莲花球是无模型或
  加载失败时的回退；帧缓存必须有界。SDK、模型许可与平台支持未验收前，
  不把未确认的二进制或模型放入默认发行包。
- 桌宠工作先读 [产品路线图](docs/desktop-pet-roadmap.md)；涉及信息卡或页面迁移，
  再读 [迁移计划](docs/pet-workbench-migration-plan.md)。计划不能当成已实现状态。
- 保持 C++23 modules + HuxerUI + CMake 架构；UI 普通源在 `src/ui/*.cpp`，
  领域模块在 `src/*.cppm` 及对应实现，原生活动模块在 `src/agents/`。
- 不引入 Electron、新数据库或大型依赖绕过现有架构。已有用量 SQLite 账本
  保持批量事务；逐请求路由日志仍写 JSONL，选型细节见领域技能。
- 出站网络统一使用 HuxerUI 平台 HttpClient，不 vendor curl/OpenSSL。
  HuxerUI 与 Lib-SQLite 沿用下游 fork，来源与 pin 以
  [依赖说明](third_party/README.md)、CMake 和 CI 为准，不在多处复制版本清单。

## 开始修改前的必读入口

先读相关现有代码，再按任务加载以下项目技能；跨层修改同时读取相关技能。
技能与参考中的适用约束必须执行，历史归档仅供查证，不覆盖当前规范。

| 任务 | 必读技能及后续入口 |
|------|--------------------|
| UI、桌宠窗口、状态、交互、主题、图标 | [llmswitch-ui](project-skills/llmswitch-ui/SKILL.md) |
| 配置、供应商、路由、MCP、Skills、会话、用量 | [llmswitch-domain](project-skills/llmswitch-domain/SKILL.md) |
| C++ 模块、资源管理、构建、依赖、平台、CI、规范维护 | [llmswitch-build](project-skills/llmswitch-build/SKILL.md) |

UI 工作还必须读取实际使用版本的 `huxerui-app-development/SKILL.md`：
源码模式取当前源码树 `skills/`，SDK 模式取其 `share/huxerui/skills/`。
公开 API 以该版本为准；项目技能不复制上游手册，不混用源码与 SDK 契约。
活动来源和 hooks 工作另读 [活动集成契约](src/agents/README.md)；资源改动另读
[资源规范与许可](resources/README.md)。技能内含模块定位和专项参考阅读条件。

## 通用实现硬约束

- 保持用户已有修改，不顺手重排、格式化或重写无关代码；领域行为变化必须补充
  或更新相应测试，测试使用环境变量与临时目录隔离真实用户配置。
- 所有需释放的资源先有 RAII 所有者再使用；禁止裸 new/delete/malloc/free，
  锁使用标准 RAII 锁，线程须停与 join，临时文件失败时清理。
  平台专用调用和 closer 都须位于对应编译分支，不能只凭本机通过判定跨平台安全。
- `import std;` 后不再 include 标准头；C/系统头放全局模块片段。
  UI 普通 TU 按需自行 include，模块内联规则与所有者范本见构建技能。
- State 只在 UI 线程读写，ProviderStore 无内部锁，UI 侧独占；现有 router
  resolver 的后台只读快照竞态是已知取舍，不得据此扩展后台 store 访问。
- 会话扫描、导出、删除、详情分页必须通过 RunWorker 离开 UI 线程；可重复加载
  用请求代次丢弃过期结果，成功和失败都结束加载态。网络使用平台异步 API。
- 纯 State 导航直接写状态；隐藏或退役当前正被点击的窗口须拦截关闭请求并经
  TaskScope::Post 延后，避免指针派发收尾访问失效对象；具体边界见 UI 技能。
- 剪贴板仅在 UI 线程通过 `UseApplication().Clipboard()` 使用；TreeView 必须
  在有界垂直视口。Runtime 一次性注册放 application_hooks，不放 composable。
- live 改写先备份并原子落盘，保留工具无关字段和原生条目；探测保持只读。
  dsh/ZCode 增删改走单条增量，整组同步只走显式入口，细则先读领域技能。
- 路由总开关与逐 Agent 开关都须保留；选择持久化到 AppConfig.routerTools，
  缺字段默认全启用，显式空数组允许全关；运行中即时生效，禁用路径返回 403，
  在 resolver、上游访问和请求统计前短路。
- UI 写库后更新共享 revision，保持供应商页、托盘与相关视图一致。
  主题唯一入口为 `src/ui/theme_colors.h`，图标映射和许可要求见 UI 技能。

## 每批修改后的固定流程

以下流程对文档修改同样适用；CI 跳过纯文档提交不免除本地门禁。

1. 使用 Ninja 配置并完整编译：
   `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
   `cmake --build build --parallel 4`
   已有构建目录若用其他生成器，不原地切换或删除，另建目录并相应替换后续路径。
2. 运行相关测试，默认至少 `ctest --test-dir build --output-on-failure`。
3. 运行 `python3 project-skills/llmswitch-build/scripts/check_guidance.py`，
   再运行 `git diff --check`，检查规范规模、空白和补丁格式。
4. 检查 `git status --short` 和 diff，只暂存本批文件，避免夹带无关文件、
   密钥或构建产物；创建说明清楚的本地 Git commit。
5. 尝试推送当前分支到其上游；没有上游时用 `git push -u origin <当前分支>`。
6. 推送失败保留本地 commit，不回滚、不删除、不反复索取权限，最终报告原因；
   成功则报告 commit ID 和目标分支。

编译或测试失败时不得声称完成；先修复本批引入的问题。明确属于外部环境限制时，
保留安全的本地修改并如实报告。无 CLI 分流；日常启动优先使用 `./run.sh`。

## Git 安全边界

- 不覆盖或丢弃用户改动，不用 `git reset --hard`、`git checkout --` 等破坏性命令。
- 默认不改写历史、不 push --force；未经明确要求不变基或 amend 已公开提交。
- 用户所说“每次保存”指创建本地 commit，不只是保留工作区修改。
