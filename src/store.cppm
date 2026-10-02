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

// dsh live 实况条目：settings.yaml 的 llm-pi-ai.providers 下每条手写路由。
// 供应商页左列直接展示它，右列是本应用留存（config.json 组）——两边对照。
export struct DshLiveProvider {
    std::string key;          // providers map 键（llmswitch-<id> / 用户手写键）
    std::string displayName;  // 条目的 displayName（缺失为空）
    std::string baseUrl;
    std::string api;          // 原始 api 拼写（anthropic-messages 等）
    std::string apiFormat;    // 归一到 models::normalizeApiFormat 三档
    std::string apiKeyEnv;
    std::string apiKey;       // 从 .credentials.yaml 按 apiKeyEnv 读回（可空）
    std::string model;        // 首个模型条目 id（可空）
    bool isDefault = false;   // agent-default-model.provider == key
    // 已纳管（本应用条目 llmswitch-<id> 且组内存在该 id）时的组内 id；
    // 空 = live 独有（未纳管），页面提供「收编」。
    std::string providerId;
    // 合成行：settings.yaml 里没有这条，它由 dsh 自己的适配器注册
    // （只有内置的 deepseek-official 路由是这样）。左列补出它是为了让
    // 「当前默认到底用哪条」可见；它不在文件里，所以既不能收编也不能删除。
    bool builtin = false;
};

// ZCode live 实况条目：config.json 的 `provider` map 下每一条。
// ZCode 的条目键有三种：本应用托管的 `llmswitch:<id>`、ZCode 自己页面上
// 新建的原生条目（键是它自己生成的 id）与官方套餐 `builtin:*`。密钥就在
// 条目里（options.apiKey），不像 dsh 那样另存凭据文档。
export struct ZcodeLiveProvider {
    std::string key;          // provider map 键
    std::string displayName;  // 条目的 name（缺失为空）
    std::string baseUrl;      // options.baseURL
    std::string kind;         // 原始 kind 拼写（anthropic / openai-compatible）
    std::string apiFormat;    // 归一到 models::normalizeApiFormat 三档
    std::string apiKey;       // options.apiKey（可空：OAuth 条目没有）
    std::string model;        // 模型清单首个（可空）
    std::vector<std::string> models;  // 模型清单（保序；空 = 条目没有清单）
    bool enabled = false;     // enabled 字段（缺省 = 启用）
    // 已纳管（组内存在对应供应商）时的组内 id；空 = live 独有（未纳管），
    // 页面提供「收编」。
    std::string providerId;
    // `builtin:*`：ZCode 官方套餐条目，不是第三方供应商，既不能收编也不能
    // 删除（官方状态由「ZCode 官方」常驻卡表达）。
    bool builtin = false;
};

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
    // 删除本地留存；删掉的是 current 时 current 置空。
    // eraseLive 只对 dsh 有意义（别的工具本就不在 live 里留条目）：false = 只删
    // 本应用这条，settings.yaml 原样不动——右列那条删除确认的「只删本应用」，
    // 删完 live 条目仍在，左列随后把它显示成未纳管的手写路由，用户还能再收编
    // 回来。默认 true 与历史行为一致（dsh 的 llmswitch-<id> 一并摘掉）。
    void removeProvider(std::string_view tool, const std::string& id,
                        bool eraseLive = true);
    // 复制一份（新 id、名称加「（副本）」），插在原项之后并返回副本。
    models::Provider duplicateProvider(std::string_view tool, const std::string& id);

    // ---- dsh 增量多供应商（settings.yaml 的 llm-pi-ai.providers）----
    // dsh 的 live 配置是「多条手写路由并存」的累加列表：本应用把组内供应商
    // 逐条写成 llmswitch-<id> 条目，切换只改 agent-default-model 指向。
    // 供应商页用左右两列对照 live 实况与本地留存——live 会被 dsh 自己或其它
    // 工具改动，两边本来就可能不同步。
    //
    // 写侧一律是**单条增量**：每个入口只动自己那一条（syncDshProviders 是
    // 唯一例外，它是「全部写入 dsh」按钮显式触发的整体重建）。绝不能把增删改
    // 顺手做成整组重建——那等于把本地全部供应商一次性推给 dsh，用户在 dsh 侧
    // 手工整理过的条目前脚刚删掉一条就会被别人补回来。

    // live settings.yaml 里 llm-pi-ai.providers 的实况列表（只读）。文件里没有
    // dsh 内置的 deepseek-official 路由时补一条 builtin 合成行（默认指向内置
    // 路由时，由它表达「当前用哪条」）。
    [[nodiscard]] std::vector<DshLiveProvider> dshLiveProviders() const;
    // 显式整体重建（「全部写入 dsh」按钮）：把组内全部供应商逐条重建为
    // llmswitch-<id> 条目（含模型能力声明与 .credentials.yaml 密钥），清掉不再
    // 属于组内的孤儿 llmswitch-* 条目，并把被收编过来的裸键（键 == 组内 id）
    // 接管成 llmswitch-<id>；其它手写条目（含 dsh 内置路由）一律原样保留。
    // defaultProviderId 非空 = agent-default-model 指向它；clearDefault = 删掉
    // 该块回到内置官方路由（两者同时给时 clearDefault 优先）。另外，live 里
    // 指向已被删除供应商的悬空默认会自动清回官方路由。
    void syncDshProviders(const std::string& defaultProviderId = {},
                          bool clearDefault = false);
    // 单条增量写入（供应商页每行的「写入 / 更新」）：只把该供应商重建为
    // llmswitch-<id> 条目（含密钥 upsert），别的条目与默认指向一字不动。
    // id 不在组内抛 std::runtime_error。
    void writeDshProvider(const std::string& id);
    // 收编 live 里的一条手写条目成供应商卡：条目键映射成组内 id
    // （llmswitch-<id> 剥前缀，裸键即 id），同 id 已存在则原位更新（以 live
    // 为准，保留原 createdAt）。收编后只重建这一条（键改名 llmswitch-<id>、
    // 内容不变）；它正是 dsh 默认路由时 agent-default-model 同步改指。
    // 条目不存在抛 std::runtime_error。
    models::Provider adoptDshProvider(const std::string& key);
    // 删除 live 里的一条条目（本应用的 llmswitch-<id> 或用户手写键都行），
    // 只删这一条、别的条目一字不动；被删的正是默认路由时
    // agent-default-model 块一并清除。条目不存在抛 std::runtime_error。
    void removeDshProvider(const std::string& key);

    // ---- ZCode 增量多供应商（config.json 的 provider map）----
    // 与 dsh 同一套模型，两个差别：
    //   1. 「默认指向」是条目自己的 `enabled` 开关（ZCode 允许多条同时启用，
    //      本应用只保证自己托管的那几条互斥——切换时停用其余 llmswitch:*）；
    //   2. 密钥就在条目里，没有单独的凭据文档。
    // 同样是**单条增量**：只有 syncZcodeProviders 做整组重建。
    //
    // ZCode 自己页面上的原生条目（键是它自己的 id）一律原样保留：收编只把它
    // 记进本地列表，既不改名也不改写；「写入 / 更新」原位更新该条目但保留
    // 它自己维护的其它字段（options 里的其余键、systemDisabledReason 等）。
    // builtin:* 是官方套餐条目，不收编、不删除。

    // live config.json 的 provider map 实况（只读）。
    [[nodiscard]] std::vector<ZcodeLiveProvider> zcodeLiveProviders() const;
    // 单条增量写入（右列「写入 / 更新」）：只重建这一条，别的条目一字不动。
    // id 不在组内抛 std::runtime_error。
    void writeZcodeProvider(const std::string& id);
    // 收编 live 里的一条非 builtin 条目：记进本地列表（键即身份，
    // llmswitch:<id> 剥前缀），live 一字不动；同 id 已存在则原位更新（以 live
    // 为准，保留原 createdAt）。条目不存在 / builtin 抛 std::runtime_error。
    models::Provider adoptZcodeProvider(const std::string& key);
    // 从 config.json 删掉这一条（别的条目原样保留）。builtin:* 抛错，
    // 条目不存在抛错。
    void removeZcodeProvider(const std::string& key);
    // 显式整组重建（「全部写入 ZCode」）：把组内每条写成 llmswitch:<id>
    // （原生键的条目原位更新、不另起重复条目），清掉不再属于组内的孤儿
    // llmswitch:* 条目；builtin:* 与 ZCode 原生条目一律不动。
    void syncZcodeProviders();

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

    // ---- dsh 单条增量写入/删除（定义在 store_live.cpp）----
    // 把 p 写成 llmswitch-<p.id> 条目：overwrite=false 且条目已在 live 里时内容
    // 原样保留（「设为默认」不该覆盖用户在 dsh 侧的手改），true 时整条重建；
    // 裸键 <p.id>（收编前的形状）在同一 id 的新条目写入时被移除。makeDefault
    // = true 时 agent-default-model 指向新键，否则只在原默认正好指向被改名的
    // 裸键时跟着改指。别的条目、无关键、注释一律不动；密钥 upsert 进
    // .credentials.yaml。
    void writeDshEntry(const models::Provider& p, bool overwrite,
                       bool makeDefault);
    // 只从 live 删掉这个键（别的条目与默认指向都不动）；它正是默认路由时
    // agent-default-model 块一并清除。返回该键在 providers 里是否存在。
    bool eraseDshEntry(const std::string& key);

    // ---- ZCode 单条增量写入/删除（定义在 store_zcode.cpp）----
    // 把 p 写成它对应的条目（优先已有的 llmswitch:<p.id>，其次原生裸键
    // <p.id>，都没有才新建 llmswitch:<p.id>）：overwrite=false 且条目已存在时
    // 内容原样保留（「设为启用」不该覆盖用户在 ZCode 侧的手改），true 时整条
    // 重建（原生条目原位合并，它自己维护的字段保留）。makeEnabled=true 时
    // 置 enabled=true。没有变化就不碰文件。
    void writeZcodeEntry(const models::Provider& p, bool overwrite,
                         bool makeEnabled);
    // 只从 config.json 删掉这个键；builtin:* 抛错，键不存在返回 false。
    bool eraseZcodeEntry(const std::string& key);
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
// pi 的 models.json 模型清单助手（定义在 store_live.cpp）：元素既可能是裸标量
// （"m1"）也可能是对象（{"id": "m1"} / {"name": "m1"}），两种形状都认。
std::string modelEntryId(const nlohmann::json& element);
bool modelListContains(const nlohmann::json& list, std::string_view id);
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
    std::string displayName;
    std::string baseUrl;
    std::string api;
    std::string apiKeyEnv;
    std::string firstModel;
    // 首个模型条目声明的推理档位（reasoningEfforts 的键，规范升序；
    // 未声明时为空）。
    std::vector<std::string> reasoningEfforts;
    // 同一个模型条目的容量与输入模态（官方字段 contextWindow / maxTokens /
    // input；0 与空清单 = 未声明）。
    std::int64_t contextWindow = 0;
    std::int64_t maxTokens = 0;
    std::vector<std::string> inputModalities;
};
struct DshSettingsInfo {
    std::string defaultProvider;
    std::string defaultModel;
    // agent-default-model 里的推理等级（dsh 的模型选择状态之一：官方键
    // reasoningEffort）。本应用不建模它，但改写这一块时必须原样带回去——
    // 否则切换一次就把用户在 dsh 里选的推理等级清掉。
    std::string defaultReasoningEffort;
    std::vector<DshProviderEntry> providers;
};
DshSettingsInfo parseDshSettings(std::string_view text);
std::string readDshCredential(const std::filesystem::path& file,
                              std::string_view envName);
// pi/dsh 的 api 字段反映射（三档，经 models::normalizeApiFormat 归一的反向；
// 未知拼写返回空串）。定义在 store_live.cpp，import 侧与 dsh 增量同步共用。
std::string piApiFormatValue(std::string_view api);

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
// 条目键 → 组内 id（llmswitch:<id> 剥前缀，原生键即 id）。
std::string zcodeProviderIdFor(std::string_view key);
// config.json 的一个 provider 条目 → Provider（name/apiFormat/baseUrl/apiKey/
// model/models/modelsMeta）。收编与全量导入共用同一套读法，两边不会漂移。
models::Provider zcodeProviderFromEntry(std::string_view key,
                                        const nlohmann::json& entry);

} // namespace store
