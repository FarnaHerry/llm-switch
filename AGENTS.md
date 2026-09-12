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
- 保持用户已有改动，不顺手重排、格式化或重写无关代码。领域行为改变必须补充或
  更新相应测试。
- HuxerUI 源码优先来自 `third_party/huxerui`，CI 固定版本见
  `.github/workflows/build.yml`。当前基线 `d1d2daa` 提供 ApplicationHandle 的
  Clipboard/Directories 服务与 TreeView；剪贴板只在 UI 线程通过
  `UseApplication().Clipboard()` 使用，TreeView
  必须位于有界垂直视口。
- 会话目录扫描、导出和删除属于文件 IO，必须通过 `RunWorker` 离开 UI 线程；
  可重复触发的加载必须以请求代次丢弃过期结果，并在成功或失败时正确结束加载态。
- 本地路由包含总开关和逐 Agent 代理开关。逐 Agent 选择必须持久化到
  `AppConfig.routerTools`，旧配置缺字段时默认全部启用；运行中切换应即时生效，
  被禁用路径不得访问 resolver、上游或写入请求统计。

## UI 视觉硬约束

- 总体风格是现代界面与东方水墨兼容的“岛屿风”：全景水墨只作为环境背景；内容
  仍以轻量、分层、可读的岛屿组织，不把山水细节压到交互内容之上。
- “卡片水墨化”是让卡片本身具有断续墨线、飞白和角部淡晕，不是仅把规则矩形设为
  透明，也不是把一张水墨画塞进卡片。通用卡片使用 `ink_card_frame.svg` 表达边界，
  不恢复规整 Border；弹窗可保留规则边框以保证浮层识别度。
- 一级轻岛圆角 10pt、二级岛 6pt；颜色必须来自 `ResolveIslandTheme(theme)` 等
  语义主题层，不在页面中散落硬编码明暗色。
- 主题只用太极图形选择器，不显示“深色/浅色/跟随系统”等文字提示。阴阳平衡表示
  跟随系统，玄墨突出表示深色，宣纸突出表示浅色；点击循环切换
  `system → dark → light`。悬停时持续旋转，移出时冻结在当下角度并保留状态。
- 保证交互可辨识、文本对比度和响应式布局；水墨纹理不能牺牲信息层级或功能识别。

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

- 24×24 功能图标遵守 `resources/README.md` 的单套无色水墨图标规范；深浅模式
  由运行时 tint 自适应，禁止维护 `_selected/_dark/_light` 重复资源。CMake 会
  在配置期检查 `#FFFFFF` alpha-mask 与 `_selected` 重复资源。
- 太极主题选择器、轻岛屿表面和水墨卡片规范见 `CLAUDE.md` 的“UI 硬约束”。
- 图标表现必须通过项目的 `IconButton`/主题 tint 管线完成，不为深浅模式复制资源，
  不依赖 SVG 自带的固定前景色。
