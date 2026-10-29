# 项目规范维护与迁移对照

## 维护约束

- 根 AGENTS.md 同时最多 150 行、6,000 Unicode 字符（含换行）。
- 项目技能统一存 project-skills/<name>/SKILL.md，每个最多 100 行；长内容优先
  合并重复，再移到该技能 references/ 或引用已有专题文档，不压成长行或删必要约束。
- 项目技能与项目参考均纳入 Git。不要放到被忽略的 .claude/、.agents/ 或用户全局
  skills 目录，也不提交 third_party/huxerui 的上游手册副本。
- 根规范提供任务阅读路由，技能提供带触发条件的参考入口；跨层任务读取相应技能。
  不依赖工具对自定义目录的自动发现，也不要求工具入口预加载全部 references。
- 规则只有一个专项来源；根文件保留必要底线，细节通过引用承接。
  CLAUDE.md 保持 @AGENTS.md 导入，Copilot 等入口只引用根规范与技能路由。
- 引用使用相对路径；代码定位注明相对仓库根。框架 API 核对实际源码/SDK 版本，
  项目专题文档复用原路径。历史记录显式标为非规范，不能当作当前实现。

## 2026-10-26 迁移对照

原根规范为 1,075 行、55,617 字符。下列原章节全部有去向，不靠删除约束达标。
表中的参考文件均在相应项目技能 references/ 内，路径相对 project-skills/。

| 原章节 | 当前归属 |
|--------|----------|
| 统一入口、项目定位、产品优先级 | 根 AGENTS.md；已有桌宠路线图及迁移计划 |
| 项目与代码边界 | 根硬约束；领域 architecture、UI huxerui-contracts、现有依赖说明 |
| HuxerUI 开发参考 | UI huxerui-contracts；构建 build-and-ci |
| 构建/运行/测试 | 根收尾命令；构建 build-and-ci |
| 架构表与导航说明 | 领域 architecture/services；UI page-behavior |
| 领域层设计要点 | 领域 configuration/live-providers/dsh/zcode/services |
| dsh/ZCode 双列预算 | UI provider-performance；页面入口从两技能均可到达 |
| C++23 命名模块内联 | 构建 cpp-resources |
| 关键约定 | 根硬约束；UI huxerui-contracts/presentation；构建 cpp-resources |
| 资源与 RAII 硬约束 | 根底线；构建 cpp-resources 的规则与所有者盘点 |
| 持久化载体选择 | 领域 persistence/services |
| 已知取舍 | 领域 known-tradeoffs |
| 多平台/CI | 构建 build-and-ci；已有 third_party/README.md、CMake/CI |
| 历史里程碑 | 构建 history（原条目保留，非现行规范） |
| UI 视觉硬约束、资源约束 | UI presentation；现有 resources/README.md |
| 固定收尾流程、Git 安全 | 根规范完整保留，未放宽文档修改门禁 |

历史中仍适用的独有细节另提取进当前参考：ZCode 原生字段与模型清单、
表单目标/首帧、卡片状态限宽、会话文本上限、用量账本批量事务，不能只藏在归档。

## 冲突与过期项的处理依据

- 原“关键约定”要求所有卸载点击节点的导航 Delay(0)，与新版导航规则冲突。
  src/ui/agent_page.cpp 的 goBack 和表单入口直接写 State；现行规则明确纯导航直写。
  src/ui/app.cpp、pet.cpp 对当前点击窗口的 Hide/Close 经 Post 延后，保留窗口安全边界。
- net.cppm 当前只导出纯解析/URL 函数，provider_network.cpp 使用平台异步 HTTP；
  已删除 fetchModels/fetchUsage 等同步接口，不把旧 RunWorker 网络说明迁为现行规则。
- src/ui/theme_colors.h 当前是“石墨/晴石”；旧“深海”和水墨配色保留在历史，
  当前视觉只引用主题唯一入口。原“见 CLAUDE.md 的 UI 硬约束”修正为 UI presentation。
- src/single_instance.*、platform/*/main.cpp、src/ui/launch_at_login.cpp 已有相关实现；
  “无单实例/开机自启”是旧待办，归档保留但不列为当前缺口，验收仍以代码/专题文档为准。
- src/store*.cpp 已拆分，architecture 更新定位；ZCode 启用只保证托管条目互斥，
  不沿用旧里程碑“停用其余所有条目”的描述，官方恢复作为显式动作单独说明。
- dsh 的 restoreOfficial 直接清默认块和托管条目；当前未调用 syncDshProviders。
  删除参考中“restoreOfficial 收尾也整组重建”的过期调用关系，保留显式同步唯一入口。
  readDshCredential 有 refs 时只读块内，否则兼容预发布 flat 布局；相应收编说明
  保留布局边界，不把旧版“所有情况只认 refs”写成绝对规则。
- CI Windows 已拆成共享 SDK 构建与应用/安装器消费者；技能引用当前 workflow，
  不沿用“各消费者都源码构建一份”的旧假设。依赖 pin 不重复散写到新入口。

## 验收方法

1. 运行 `python3 project-skills/llmswitch-build/scripts/check_guidance.py`。
   脚本按 UTF-8 解码后的 Unicode 字符计数（包含换行），检查根双上限和各技能行数。
2. 检查新增技能 frontmatter 的 name/description、适用边界及必读参考是否完整。
   人工审阅不能只看行数；不能把必要细节降为可选或用长行绕过限制。
3. 从根入口递归核对 Markdown 相对链接存在且各参考可达，查找旧章节跳转与旧入口引用。
4. 检查 git status、diff、git check-ignore 和暂存列表；项目 skills/references/scripts
   全部追踪，只暂存本批文件，保留用户原有未跟踪图片和 UI 探针。
5. 按根规范完整配置、编译、测试、diff 检查、commit 并尝试 push；规模通过不能替代它们。
