---
name: llmswitch-build
description: 处理 llm-switch 的 C++23 模块、RAII、CMake 构建、依赖、跨平台打包、CI 和项目规范维护时使用。
---

# llm-switch 工程与规范维护

根 [AGENTS.md](../../AGENTS.md) 规定固定收尾流程；本技能提供工程细节和定位入口。

## 按任务读取

- 改模块接口、系统资源、锁、线程或平台条件编译前，必读
  [C++ 与资源契约](references/cpp-resources.md)，先写所有者再使用资源。
- 配置、运行、升级依赖、修改 CMake/平台入口/CI 或打包发布前，必读
  [构建与 CI](references/build-and-ci.md)、[依赖说明](../../third_party/README.md)，
  并核对实际 CMakeCache、顶层 CMakeLists 和 build.yml。
- 维护 AGENTS.md、项目技能、参考或工具入口前，必读
  [规范维护与迁移对照](references/guidance-maintenance.md)。
- 只有追查历史决策或旧故障时才读 [历史归档](references/history.md) 对应条目；
  旧网络栈、配色、工具数量与历史待办不作为当前契约。

## 工程约束

- Ninja 是默认生成器，不原地切换或删除已有其它生成器的构建目录。
  Release 默认使用 `build`，Debug 可改配置或使用对应 preset。
- HuxerUI 选用顺序：显式源码、仓库内源码、安装 SDK、Linux 离线 SDK；
  源码缺平台开发依赖时按 CMake 回落，FORCE_SDK 是显式跳过源码的开关。
- API 依据实际使用的 HuxerUI 版本；pin 集中在现有依赖文档、CMake/CI，
  不向项目技能复制上游手册或过期本地 skill。
- `import std` 后不 include 标准头；普通 UI TU 自行 include，系统头位于
  全局模块片段。命名模块类内定义不自动获得头文件的隐式 inline 语义。
- JSON 模块不使用 `.items()` 结构化绑定，改用 it.key()/it.value()。
- 资源一律 RAII，平台专用 closer 和调用一起进入平台分支；本机编译通过
  不能证明其它平台符号可用。已有流对象不用再包无意义的资源所有者。
- 不为节省额度关闭 main/PR 的三平台构建；发布必须满足现有跨平台门禁。
  workflow_dispatch 不等于授权发布，tag release 行为以当前 workflow 为准。

## 本地验证

执行根规范的配置、完整构建、ctest 和 git diff 检查。
规范规模检查使用仓库自带脚本，无第三方 Python 依赖：

```bash
python3 project-skills/llmswitch-build/scripts/check_guidance.py
```

根文件同时限制行数与字符数，每个项目 SKILL.md 限制行数；不靠长行规避。
维护引用时检查相对路径可达、适用条件明确，新增项目文件必须进入 Git。
纯文档提交可能被 CI paths-ignore 跳过，仍执行本地编译测试。
成功后仅暂存本批文件，创建本地 commit 并尝试推送；失败报告遵循根规范。
