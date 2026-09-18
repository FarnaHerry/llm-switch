// store.cppm — llmswitch.store：供应商配置 store（接口）。
//
// 职责：config.json 的读写与 CRUD、把选中供应商写进各工具的 live 配置文件
// （claude-code 的 settings.json / codex 的 auth.json + config.toml /
// opencode 的 opencode.json / pi 的 models.json + settings.json /
// dsh 的 settings.yaml + .credentials.yaml / hermes 的 config.yaml /
// claude desktop 的 3p profile 组）、live 文件备份与收编、配置导出导入。
// 实现单元：store.cpp（核心）/ store_live.cpp（切换与还原）/
// store_import.cpp（收编、探测与导入）/ store_zcode.cpp（ZCode 条目同步）。
// 工具 id 以 models::toolRegistry() 注册表为准。不强制单例 —— 测试可直接
// 实例化多个对象隔离验证。所有失败路径抛 std::runtime_error（中文消息），
// 由调用方（UI）兜底展示。
export module llmswitch.store;

import std;
import nlohmann.json;
import llmswitch.models;

namespace store {

export class ProviderStore {
public:
    ProviderStore() = default;
    // 便于测试直接组装内存配置。
    inline explicit ProviderStore(models::AppConfig config) : config_(std::move(config)) {}

    // 读 dataDir()/config.json：不存在 → 默认配置，且对「组为空且 live 文件
    // 存在」的工具执行首次导入（importLive，失败静默——如 opencode 的 JSON5
    // 文件不能搞垮 load）；文件损坏 → 挪到 config.json.corrupt-<毫秒> 后按
    // 不存在处理，绝不崩溃。
    static ProviderStore load();

    // 原子写 config.json（先写 .tmp 再 rename）。
    void save() const;

    // 当前配置快照（只读；修改一律走下面的方法，保证落盘一致）。
    inline const models::AppConfig& config() const { return config_; }
    // 组访问（tool 不在注册表抛 std::runtime_error；已注册但尚无组时返回空组）。
    const models::ProviderGroup& group(std::string_view tool) const;

    // 主题模式（system / dark / light；其余值原样保存由 UI 兜底），立即落盘。
    void setThemeMode(std::string mode);

    // 窗口关闭行为（ask / tray / quit），立即落盘。
    void setCloseBehavior(std::string behavior);

    // 标题栏中心莲花阵：点击切换页面后是否立即收起导航盘，立即落盘。
    void setRadialNavAutoClose(bool enabled);

    // Claude Code 安装检查：写入/移除 settings.json env.DISABLE_INSTALLATION_CHECKS。
    bool claudeCodeSkipInstallationChecks() const;
    void setClaudeCodeSkipInstallationChecks(bool enabled);

    // 本地路由设置（llmswitch.router），立即落盘。
    void setRouterEnabled(bool enabled);
    void setRouterPort(int port);
    void setRouterFailover(bool enabled);
    // 单 Agent 代理开关；未知工具 id 抛异常，修改后立即落盘。
    void setRouterToolEnabled(std::string_view tool, bool enabled);

    // ---- CRUD（均立即落盘）----
    // id/createdAt 为空/0 时自动生成；返回实际使用的 id（zcode 保存流程
    // 需要它定位生成的条目）。
    std::string addProvider(std::string_view tool, models::Provider provider);
    // 按 provider.id 整体替换；不存在抛异常。
    void updateProvider(std::string_view tool, const models::Provider& provider);
    // ZCode 专用：把 provider 原位同步成 config.json 里的 llmswitch:<id>
    // 条目（不存在则创建），启用状态保持不变——新增/编辑供应商后无需切换
    // 即与 ZCode 页面一一对应；启用互斥仍只在 switchTo 时发生。写失败抛
    // std::runtime_error。
    void upsertZcodeEntry(const models::Provider& provider);
    // ZCode 专用：查询/设置 llmswitch:<id> 条目的 enabled 开关（对应 ZCode
    // 页面里每个供应商的启用/停用）。查询对不存在的条目返回 false；设置
    // 对不存在的条目抛 std::runtime_error。
    [[nodiscard]] bool zcodeEntryEnabled(const std::string& id) const;
    void setZcodeEntryEnabled(const std::string& id, bool enabled);
    // 删除；删掉的是 current 时 current 置空。
    void removeProvider(std::string_view tool, const std::string& id);
    // 复制一份（新 id、名称加「（副本）」），插在原项之后并返回副本。
    models::Provider duplicateProvider(std::string_view tool, const std::string& id);

    // 切换激活供应商：先备份 live 文件再改写，成功后更新 current 并落盘。
    // 各工具写入策略：
    //   claude-code：深合并 settings.json 的 env（ANTHROPIC_BASE_URL /
    //     ANTHROPIC_AUTH_TOKEN；model 与三档映射 haiku/sonnet/opusModel 非空时
    //     写 ANTHROPIC_MODEL / ANTHROPIC_DEFAULT_*_MODEL），其余字段原样保留；
    //   codex：深合并 auth.json 的 OPENAI_API_KEY，codexConfigToml 非空时整体
    //     替换 config.toml（model 非空时再行级重写顶层 model 键）；
    //   opencode：opencode.json 顶层 provider map upsert 条目（npm 按
    //     apiFormat 选 @ai-sdk/anthropic / @ai-sdk/openai-compatible），model
    //     非空写顶层 model="<id>/<model>"；官方文件支持 JSON5 注释，本实现
    //     只认严格 JSON——解析失败抛错且不碰原文件；
    //   pi：models.json 顶层 providers map upsert + settings.json 深合并
    //     defaultProvider/defaultModel，目录 0700 文件 0600；
    //   dsh：settings.yaml 行级 upsert llm-pi-ai.providers 的 llmswitch-<id>
    //     条目 + 文件头 agent-default-model 指向；密钥只写
    //     .credentials.yaml（apiKeyEnv 引用，两份文件均被热监听 → 即时生效）；
    //   hermes：config.yaml 的 custom_providers 列表删旧 llmswitch-* 条目后
    //     追加新条目（api_mode 三档映射），顶层 model 节写 provider（总是）
    //     与 default（model 非空时），其余节原样保留；
    //   claude（Desktop 3p 直连）：两个 claude_desktop_config.json 深合并
    //     deploymentMode=3p，写 configLibrary 下固定 id 的 profile 与
    //     _meta.json；inferenceModels = 主模型 + 三档映射条目（非白名单模型名
    //     借该档安全 route id，菜单显示名放 labelOverride，supports1m 按勾选写入；
    //     实际请求模型保存在 Provider 的三档 *Model 字段）；Linux 不支持抛错。
    // 供应商不存在 / 文件写失败抛 std::runtime_error。
    void switchTo(std::string_view tool, const std::string& id);

    // 探测 live 文件当前对应组内哪个 provider；无匹配返回空串。只读，不改配置。
    std::string detectCurrent(std::string_view tool) const;

    // 恢复厂商原生状态：撤掉本应用对 live 配置写入的覆盖（先备份再改），
    // 组 current 清空并落盘。各工具还原策略：
    //   claude-code：settings.json 的 env 块删除 ANTHROPIC_BASE_URL /
    //     ANTHROPIC_AUTH_TOKEN / ANTHROPIC_MODEL / ANTHROPIC_DEFAULT_*_MODEL
    //     六键（其余键与字段保留）；
    //   codex：auth.json 删 OPENAI_API_KEY（删完为空对象则删文件）；
    //     config.toml 仅当内容与组内某 provider 的 codexConfigToml 完全一致
    //     （即本应用写入且未被手改）才删除，否则不动；
    //   claude（Desktop）：两份 claude_desktop_config.json 删 deploymentMode
    //     键，_meta.json 移除本应用 profile 条目并清 appliedId；Linux 抛错；
    //   dsh：settings.yaml 删 agent-default-model 块与 llmswitch-* 路由条目，
    //     回到内置 deepseek-official 路由（.credentials.yaml 的密钥不代清）。
    // opencode / pi / hermes 没有官方默认状态，抛 std::runtime_error。
    void restoreOfficial(std::string_view tool);

    // 把 live 文件当前内容收编成名为「当前配置」的新 provider（已有匹配项则
    // 复用不重复建），加入组并设为 current 后落盘；live 文件不存在返回
    // 空 Provider（id 为空）且不改动。
    models::Provider importLive(std::string_view tool);

    // 导出整个配置到 path（原子写）。
    void exportTo(const std::filesystem::path& path) const;
    // 导入合并：按 id 覆盖/新增；导入前先落盘当前配置，并把 config.json
    // 复制到 backupsDir()/config.json.<毫秒>.bak 作为回滚快照。旧格式
    // （v1 顶层 claude/codex）文件经 models::fromJson 自动迁移。
    // 文件不是有效 JSON 抛 std::runtime_error（不动用户选的导入文件）。
    void importFrom(const std::filesystem::path& path);

private:
    models::ProviderGroup& groupRef(std::string_view tool);
    models::AppConfig config_;
};

// 用量查询模板的用户覆盖表（cfg::usageTemplatesFile()）原文；文件不存在
// 返回空串。文件存在但读不出来抛 std::runtime_error（带路径，中文消息）；
// 内容校验由 models::parseUsageTemplates 负责。
export std::string loadUsageTemplatesOverride();

// ---- 模块内共享工具（模块链接，不导出）----
// 实现单元之间复用的文件工具、live 格式解析与 ZCode 条目助手；对模块外
// 不可见。定义位置：文件工具在 store.cpp；live 格式工具在 store_live.cpp；
// ZCode 助手在 store_zcode.cpp。
std::int64_t nowMillis();
std::string generateId();
void atomicWrite(const std::filesystem::path& dest, std::string_view content);
std::string readTextFile(const std::filesystem::path& file);
nlohmann::json readJsonOrNull(const std::filesystem::path& file);
nlohmann::json readJsonPassive(const std::filesystem::path& file);
void backupLiveFile(std::string_view tool, const std::filesystem::path& file);
std::string claudeEnvValue(const nlohmann::json& settings, std::string_view key);
void pruneBackups(const std::filesystem::path& dir, const std::string& prefix);

std::string jsonStr(const nlohmann::json& j, std::string_view key);
nlohmann::json readJsonStrict(const std::filesystem::path& file);
std::string envLineKey(std::string_view line);
std::string trimEnvValue(std::string_view value);
std::string readEnvValue(const std::filesystem::path& file,
                         std::string_view key);
void writeEnvValues(const std::filesystem::path& file,
                    const std::vector<std::pair<std::string, std::string>>& targets);
std::string_view trimLeft(std::string_view s);
std::string codexModelLineValue(std::string_view line, bool& commentedOut);
std::filesystem::path claudeDesktopProfileFile();

// dsh（DeepSeek Harness）settings.yaml 行级读取结果与助手（定义在
// store_live.cpp；写侧的行级改写是该文件私有）。
struct DshProviderEntry {
    std::string key;
    std::string baseUrl;
    std::string api;
    std::string apiKeyEnv;
    std::string firstModel;
};
struct DshSettingsInfo {
    std::string defaultProvider;
    std::string defaultModel;
    std::vector<DshProviderEntry> providers;
};
DshSettingsInfo parseDshSettings(std::string_view text);
std::string readDshCredential(const std::filesystem::path& file,
                              std::string_view envName);

// hermes（Hermes Agent）config.yaml 行级读取结果与助手（定义在
// store_live.cpp；写侧的行级改写是该文件私有）。
struct HermesProviderEntry {
    std::string name;
    std::string baseUrl;
    std::string apiKey;
    std::string apiMode;
    std::string model;
    std::string firstModel;
};
struct HermesConfigInfo {
    std::string modelProvider;
    std::string modelDefault;
    std::vector<HermesProviderEntry> providers;
};
HermesConfigInfo parseHermesConfig(std::string_view text);

// Claude Desktop 3p profile 的固定 id（对齐 cc-switch，configLibrary 按 id
// 索引，entries 里注册同名条目）。
inline constexpr std::string_view kClaudeDesktopProfileId =
    "00000000-0000-4000-8000-000000157210";
inline constexpr std::string_view kClaudeDesktopProfileName = "llm-switch";

// ZCode config.json 条目助手：键解析（llmswitch:<id> 优先，原生裸 id 条目
// 复用）、enabled 缺省语义（省略 = 启用）、原位合并。
nlohmann::json buildZcodeEntry(const models::Provider& target,
                               const std::string& baseUrl);
std::string zcodeEntryKeyFor(const nlohmann::json& providers,
                             const std::string& id);
bool zcodeEntryOn(const nlohmann::json& entry);
nlohmann::json mergeZcodeEntry(const nlohmann::json& existing,
                               const nlohmann::json& built);

} // namespace store
