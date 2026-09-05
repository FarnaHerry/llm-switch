// store.cppm — llmswitch.store：供应商配置 store（接口；实现在 store.cpp）。
//
// 职责：config.json 的读写与 CRUD、把选中供应商写进 Claude Code / Codex 的
// live 配置文件（settings.json / auth.json / config.toml）、live 文件备份与
// 收编、配置导出导入。不强制单例 —— 测试可直接实例化多个对象隔离验证。
// 所有失败路径抛 std::runtime_error（中文消息），由调用方（UI）兜底展示。
export module llmswitch.store;

import std;
import llmswitch.models;

namespace store {

// 工具标识：group()/addProvider() 等方法的 tool 参数取值。
export constexpr std::string_view kToolClaude = "claude";
export constexpr std::string_view kToolCodex = "codex";

export class ProviderStore {
public:
    ProviderStore() = default;
    // 便于测试直接组装内存配置。
    explicit ProviderStore(models::AppConfig config) : config_(std::move(config)) {}

    // 读 dataDir()/config.json：不存在 → 默认配置，且对「组为空且 live 文件
    // 存在」的工具执行首次导入（importLive）；文件损坏 → 挪到
    // config.json.corrupt-<毫秒> 后按不存在处理，绝不崩溃。
    static ProviderStore load();

    // 原子写 config.json（先写 .tmp 再 rename）。
    void save() const;

    // 当前配置快照（只读；修改一律走下面的方法，保证落盘一致）。
    const models::AppConfig& config() const { return config_; }
    // 组访问（未知 tool 抛 std::runtime_error）。
    const models::ProviderGroup& group(std::string_view tool) const;

    // 主题模式（system / dark / light；其余值原样保存由 UI 兜底），立即落盘。
    void setThemeMode(std::string mode);

    // ---- CRUD（均立即落盘）----
    // id/createdAt 为空/0 时自动生成。
    void addProvider(std::string_view tool, models::Provider provider);
    // 按 provider.id 整体替换；不存在抛异常。
    void updateProvider(std::string_view tool, const models::Provider& provider);
    // 删除；删掉的是 current 时 current 置空。
    void removeProvider(std::string_view tool, const std::string& id);
    // 复制一份（新 id、名称加「（副本）」），插在原项之后并返回副本。
    models::Provider duplicateProvider(std::string_view tool, const std::string& id);

    // 切换激活供应商：先备份 live 文件再改写（claude 深合并 settings.json 的
    // env 三字段，其余字段原样保留；codex 深合并 auth.json 的 OPENAI_API_KEY，
    // codexConfigToml 非空时整体替换 config.toml），成功后更新 current 并落盘。
    // 供应商不存在 / 文件写失败抛 std::runtime_error。
    void switchTo(std::string_view tool, const std::string& id);

    // 探测 live 文件当前对应组内哪个 provider（claude 按 baseUrl+apiKey，
    // codex 按 apiKey）；无匹配返回空串。只读，不改配置。
    std::string detectCurrent(std::string_view tool) const;

    // 把 live 文件当前内容收编成名为「当前配置」的新 provider（已有匹配项则
    // 复用不重复建），加入组并设为 current 后落盘；live 文件不存在返回
    // 空 Provider（id 为空）且不改动。
    models::Provider importLive(std::string_view tool);

    // 导出整个配置到 path（原子写）。
    void exportTo(const std::filesystem::path& path) const;
    // 导入合并：按 id 覆盖/新增；导入前先落盘当前配置，并把 config.json
    // 复制到 backupsDir()/config.json.<毫秒>.bak 作为回滚快照。
    // 文件不是有效 JSON 抛 std::runtime_error（不动用户选的导入文件）。
    void importFrom(const std::filesystem::path& path);

private:
    models::ProviderGroup& groupRef(std::string_view tool);
    models::AppConfig config_;
};

} // namespace store
