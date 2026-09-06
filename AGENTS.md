# llm-switch Agent 工作规范

本文件适用于仓库根目录及全部子目录。任何自动化 agent 修改本项目时必须遵守。

## 修改完成后的固定流程

每一批用户要求的修改完成后，按以下顺序收尾：

1. 运行完整编译：`cmake --build build -j 4`。如果构建目录尚未配置，先运行
   `cmake -S . -B build -G Ninja`。
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
  在配置期检查 `#FFFFFF` alpha-mask、`flywhite` 和 `dry-brush` 标记。
- 太极主题选择器、轻岛屿表面和水墨卡片规范见 `CLAUDE.md` 的“UI 硬约束”。
