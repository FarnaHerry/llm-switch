// skills.cppm — llmswitch.skills：skills 中央库管理 + 符号链接同步。
//
// 中央库在 dataDir()/skills-store/<name>/（SKILL.md + 附带文件），通过
// create_symlink 同步到各工具的 skills 目录（~/.claude/skills、~/.codex/skills）。
// 无 UI 依赖，路径全部走 llmswitch.config（LLMSWITCH_* 可覆盖，测试可隔离）。
export module llmswitch.skills;

import std;

namespace skills {

export struct SkillInfo {
    std::string name;            // 目录名
    std::string description;     // SKILL.md frontmatter 的 description（宽松解析）
    bool inStore = false;        // 是否在中央库
    std::vector<std::string> linkedTools;   // 已符号链接同步的工具："claude-code" / "codex"
    std::vector<std::string> installedOnlyTools;  // 只装在某工具目录、未入中央库
    std::filesystem::path storePath;  // inStore 时有效
    bool operator==(const SkillInfo&) const = default;
};

export class SkillsStore {
public:
    // 扫描中央库 + 两工具目录，合并视图（按 name 排序）。
    static SkillsStore load();

    // 新建：中央库建目录 + SKILL.md（---\nname: ..\ndescription: ..\n---\n\n正文）。
    // 已在中央库或名字非法时抛中文错。
    void create(std::string_view name, std::string_view description, std::string_view body);

    // 收编：把某工具目录里已安装的 skill 复制进中央库（之后可统一同步）。
    // 工具侧是 dangling 链接、中央库已有同名时抛中文错。
    void importFromTool(std::string_view toolId, std::string_view name);

    // 同步开关：在中央库目录与工具目录间建/删符号链接。建链接失败抛中文错
    // （Windows 上提示可能需要开发者模式/管理员）；unlink 只删 symlink，
    // 不动工具目录里的实体目录。
    void setLinked(std::string_view name, std::string_view toolId, bool linked);

    // 删除：删中央库目录 + 所有工具侧的同名链接（含 dangling）；工具侧是
    // 实体目录（用户自己装的）时跳过，不删。
    void remove(std::string_view name);

    const std::vector<SkillInfo>& skills() const { return skills_; }

    // 读 skill 正文（编辑用；frontmatter 之后的内容，无 frontmatter 则整文件）。
    std::string readBody(std::string_view name) const;

    // 重写 SKILL.md（frontmatter 用新 description，正文整体替换）。
    void updateBody(std::string_view name, std::string_view description, std::string_view body);

private:
    void refresh();
    std::vector<SkillInfo> skills_;
};

} // namespace skills
