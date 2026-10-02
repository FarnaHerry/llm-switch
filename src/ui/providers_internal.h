// providers_internal.h — 供应商页跨编译单元的私有组件声明.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"

import nlohmann.json;
import llmswitch.models;  // 下面的声明直接用 models::Provider / ToolSpec 等类型

namespace llmswitch::ui::provider_detail {

huxerui::Task<std::vector<std::string>> FetchModelIdsWithFallback(
    std::shared_ptr<huxerui::HttpClient> http, std::vector<std::string> urls,
    std::string apiKey, std::string upstreamFormat);

huxerui::Task<std::string> FetchUsageText(
    std::shared_ptr<huxerui::HttpClient> http, models::Provider provider);

huxerui::Task<double> FetchLatency(std::shared_ptr<huxerui::HttpClient> http,
                                   std::string url);

void WriteUsageCache(UsageCache cache, const std::string& id,
                     std::string text);

} // namespace llmswitch::ui::provider_detail

namespace llmswitch::ui {

huxerui::View ProviderFormPage(std::string tool, models::Provider initial,
                               bool isNew, huxerui::State<int> revision,
                               huxerui::State<std::string> formTarget,
                               huxerui::View targetSelector);

huxerui::View UsageFormPage(std::string tool, models::Provider initial,
                            huxerui::State<int> revision,
                            huxerui::State<std::string> formTarget);

// ---- 模型清单 / 每模型参数辅助（provider_form_models.cpp）----
// modelsMeta 是 zcode 条目 models map 的原值回放（reasoning/modalities/
// limit），读写都走这组函数，改写一律「复制 → mutate → 写回」。
std::vector<std::string> FilterModelIds(
    const huxerui::StateList<std::string>& models, std::string_view query);
// 模型清单去重（保持顺序）：下拉弹层 VirtualList 以模型 id 为 key，重复
// key 会直接 abort，进下拉前统一去重。
std::vector<std::string> DedupeModels(std::vector<std::string> values);
void ReplaceModelList(const huxerui::StateList<std::string>& destination,
                      std::vector<std::string> values);
nlohmann::json WithModelMeta(
    nlohmann::json meta, const std::string& id,
    const std::function<void(nlohmann::json&)>& mutate);
bool ModelHasModality(const nlohmann::json& meta, const std::string& id,
                      const char* kind, const char* value);
nlohmann::json WithModelModality(nlohmann::json meta, const std::string& id,
                                 const char* kind, const char* value, bool on);
void ApplyModelLimit(huxerui::State<nlohmann::json> meta, const std::string& id,
                     const char* field, const std::string& text);
std::string ModelLimitText(const nlohmann::json& meta, const std::string& id,
                           const char* field);
bool ModelHasReasoning(const nlohmann::json& meta, const std::string& id);

// 模型选择器（锚定 Popup 的搜索 + 列表）；displayTarget 有效时点选同步
// 回填显示名，onPicked 非空时点选后额外回调（清单添加行直接入清单）。
huxerui::View ModelSelect(
    huxerui::StateList<std::string> fetched,
    huxerui::State<huxerui::TextEditingValue> search,
    huxerui::State<huxerui::TextEditingValue> target,
    huxerui::State<huxerui::TextEditingValue> displayTarget = {},
    std::function<void(const std::string&)> onPicked = {});

// ---- 用量刷新间隔（provider_usage_form.cpp 定义，供应商表单页共用）----
extern const std::vector<huxerui::StringVariant> kUsageIntervals;
extern const std::vector<int> kUsageMinutes;
int UsageIntervalIndex(int minutes);

// ---- 供应商新增/编辑表单（provider_form*.cpp）----
// 表单字段集合：新增/编辑共用一组 State 句柄（State 是可拷贝句柄，
// 归打开表单页的组合作用域所有）。upstreamFormat 为上游 URL 格式下标
// （0=Anthropic，1=OpenAI）；fullUrl 打开时 URL 原样使用，不追加默认后缀。
// apiFormat 为 opencode / pi 的 API 适配器下标（0=OpenAI 兼容（默认，
// 存空串）/ 1=anthropic / 2=openai-responses）。
// model 及 haiku/sonnet/opus 为模型字段；后三档映射（claude 系）每档包含
// 菜单显示名、实际请求模型和 supports1m 声明。各模型下拉只保留搜索值：
// 选中后清空控件显示，实际模型始终由对应的 TextField 状态保存。
// 用量查询配置不在此——已拆到独立的 UsageFormPage（卡片 gauge 按钮进入）。
struct FormStates {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<huxerui::TextEditingValue> baseUrl;
    huxerui::State<huxerui::TextEditingValue> modelFetchUrl;
    huxerui::State<huxerui::TextEditingValue> apiKey;
    huxerui::State<huxerui::TextEditingValue> model;
    huxerui::State<bool> modelSupports1m;
    huxerui::State<huxerui::TextEditingValue> website;
    huxerui::State<huxerui::TextEditingValue> notes;
    huxerui::State<huxerui::TextEditingValue> toml;  // 仅 codex 展示
    huxerui::State<int> upstreamFormat;
    // upstreamFormat 是「为哪个目标 Agent 定的」：两者不等说明还没人给当前
    // agent 定过，用该 agent 的原生默认现算（见 ProviderFormPage 的组合期重置），
    // 这样切换目标不需要回写 State 再等一帧。
    huxerui::State<std::string> upstreamFormatTool;
    huxerui::State<bool> fullUrl;
    huxerui::State<int> apiFormat;                   // 仅 opencode / pi 展示
    // dsh 推理档位位掩码（下标 = models::reasoningLevels() 的下标）；
    // 其他 agent 不展示也不用。
    huxerui::State<int> reasoningMask;
    // dsh 模型条目的官方能力声明：输入模态位掩码（下标 =
    // models::inputModalities() 的下标）、上下文窗口与最大输出 token 数
    // （文本输入，支持 262144 / 256K / 1M 拼写；空 = 不声明）。
    huxerui::State<int> inputMask;
    huxerui::State<huxerui::TextEditingValue> contextWindow;
    huxerui::State<huxerui::TextEditingValue> maxTokens;
    huxerui::State<huxerui::TextEditingValue> haiku;   // 仅 claude 系展示
    huxerui::State<huxerui::TextEditingValue> sonnet;
    huxerui::State<huxerui::TextEditingValue> opus;
    huxerui::State<huxerui::TextEditingValue> haikuDisplayName;
    huxerui::State<huxerui::TextEditingValue> sonnetDisplayName;
    huxerui::State<huxerui::TextEditingValue> opusDisplayName;
    huxerui::State<bool> haikuSupports1m;
    huxerui::State<bool> sonnetSupports1m;
    huxerui::State<bool> opusSupports1m;
    huxerui::State<huxerui::TextEditingValue> modelSearch;
    huxerui::State<huxerui::TextEditingValue> haikuSearch;
    huxerui::State<huxerui::TextEditingValue> sonnetSearch;
    huxerui::State<huxerui::TextEditingValue> opusSearch;
    // 预设携带的非表单字段（当前为用量查询配置）：预设 chip 点选时随
    // FillForm 更新，新增保存时从这里取；编辑时忽略（用量归 UsageFormPage）。
    huxerui::State<models::Provider> presetCarry;
};

// 表单状态初始化（ProviderFormPage 用）：hcg 要求 composable 返回 View（不能
// 抽返回 FormStates 的 composable 辅助），宏在调用点展开。各字段以入参
// provider 为初值——UseState 初值只在首次组合生效，配合表单页的
// .Key("form:" + target) 换编辑目标即整体重建状态。
#define LLMSWITCH_FORM_STATES_INIT(p)                                       \
    {huxerui::UseState(huxerui::TextEditingValue{(p).name}),                \
     huxerui::UseState(huxerui::TextEditingValue{(p).baseUrl}),             \
     huxerui::UseState(huxerui::TextEditingValue{(p).modelFetchUrl}),        \
     huxerui::UseState(huxerui::TextEditingValue{(p).apiKey}),              \
     huxerui::UseState(huxerui::TextEditingValue{(p).model}),               \
     huxerui::UseState((p).modelSupports1m),                                \
     huxerui::UseState(huxerui::TextEditingValue{(p).website}),             \
     huxerui::UseState(huxerui::TextEditingValue{(p).notes}),               \
     huxerui::UseState(huxerui::TextEditingValue{(p).codexConfigToml}),     \
     huxerui::UseState((p).upstreamFormat == "anthropic" ? 0 : 1),          \
     huxerui::UseState(std::string{}),                                      \
     huxerui::UseState((p).fullUrl),                                         \
     huxerui::UseState((p).apiFormat == "anthropic"         ? 1             \
                      : (p).apiFormat == "openai-responses" ? 2             \
                                                            : 0),           \
     huxerui::UseState(models::reasoningEffortMask((p).reasoningEfforts)),   \
     huxerui::UseState(models::inputModalityMask((p).inputModalities)),     \
     huxerui::UseState(huxerui::TextEditingValue{                           \
         models::formatTokenCount((p).contextWindow)}),                     \
     huxerui::UseState(huxerui::TextEditingValue{                           \
         models::formatTokenCount((p).maxTokens)}),                         \
     huxerui::UseState(huxerui::TextEditingValue{(p).haikuModel}),          \
     huxerui::UseState(huxerui::TextEditingValue{(p).sonnetModel}),         \
     huxerui::UseState(huxerui::TextEditingValue{(p).opusModel}),           \
     huxerui::UseState(huxerui::TextEditingValue{(p).haikuDisplayName}),    \
     huxerui::UseState(huxerui::TextEditingValue{(p).sonnetDisplayName}),   \
     huxerui::UseState(huxerui::TextEditingValue{(p).opusDisplayName}),     \
     huxerui::UseState((p).haikuSupports1m),                                \
     huxerui::UseState((p).sonnetSupports1m),                               \
     huxerui::UseState((p).opusSupports1m),                                 \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState(huxerui::TextEditingValue{}),                         \
     huxerui::UseState((p))}

void FillForm(const FormStates& fs, const models::Provider& p);
std::string ApiFormatFromIndex(int index);
std::string UpstreamFormatFromIndex(int index);
std::string DefaultUpstreamFormat(std::string_view tool);

// 每 agent 表单策略：通用字段的标签/必填规则与 agent 专属区块开关。
// 各 agent 的策略常量定义在各自的 provider_form_<agent>.cpp，本文件与
// 公共区块代码不出现任何 tool == 分支。
struct AgentFormPolicy {
    std::string urlLabel;
    std::string keyLabel;
    bool urlRequired;
    bool keyRequired;
    std::string primaryModelLabel;  // 空 = 无主模型行（zcode：清单即唯一输入）
    bool showMappings;              // 三档模型映射（claude-code / claude desktop）
    bool showApiFormat;             // API 协议（opencode / pi）
    bool showToml;                  // codex config.toml 原文
    // dsh 模型条目的官方能力区块（输入模态 / 容量 / 推理档位）。
    bool showDshModelFields = false;
};
const AgentFormPolicy& AgentPolicyFor(std::string_view tool);
const AgentFormPolicy& ClaudeCodeFormPolicy();
const AgentFormPolicy& ClaudeDesktopFormPolicy();
const AgentFormPolicy& CodexFormPolicy();
const AgentFormPolicy& OpenAiCliFormPolicy();  // opencode / pi 共用
const AgentFormPolicy& DshFormPolicy();        // dsh：OpenAI 兼容 + 推理档位
const AgentFormPolicy& ZcodeFormPolicy();

// ---- 公共区块（provider_form_common.cpp）----
// 各区块都是 composable，返回拼好的 View，由调度器放进表单 Column。

// 预设模板区（仅新增）：订阅站与按量 API 分组点选预填。
[[huxerui::composable]] huxerui::View PresetsSection(
    std::string_view tool, const FormStates& fs,
    huxerui::StateList<std::string> fetchedModels);
// 通用字段：名称 / 官网 / 备注 / 完整 URL / URL / 模型获取 URL（高级选项）/
// 上游格式 / API Key（悬停眼睛切换明文）。
[[huxerui::composable]] huxerui::View CommonFields(
    const AgentFormPolicy& policy, const FormStates& fs,
    huxerui::State<bool> showKey, huxerui::State<bool> keyHover,
    huxerui::State<bool> showModelFetchOptions);
// 「获取模型列表」按钮 + 拉取任务（结果进 fetchedModels 选择源）。
[[huxerui::composable]] huxerui::View ModelFetchButton(
    const FormStates& fs, huxerui::State<bool> fetching,
    huxerui::StateList<std::string> fetchedModels, huxerui::TaskScope tasks,
    std::shared_ptr<huxerui::HttpClient> http, huxerui::ToastHandle toast);
// 主模型行（primaryModelLabel 为空的 agent 没有这一行）+ 1M 勾选。
[[huxerui::composable]] huxerui::View PrimaryModelRow(
    const AgentFormPolicy& policy, const FormStates& fs,
    huxerui::StateList<std::string> fetchedModels);
// ---- agent 专属区块 ----

// claude-code / claude desktop：三档模型映射（Haiku/Sonnet/Opus）。
[[huxerui::composable]] huxerui::View MappingFields(
    const FormStates& fs, huxerui::StateList<std::string> fetchedModels);
// codex：config.toml 原文（多行，切换时整体替换）。
[[huxerui::composable]] huxerui::View TomlField(const FormStates& fs);
// opencode / pi：API 协议（OpenAI 兼容 / Anthropic / OpenAI Responses）。
[[huxerui::composable]] huxerui::View ApiFormatFields(const FormStates& fs);

// dsh：模型条目的官方能力声明（输入模态 input / 容量 contextWindow、maxTokens /
// 推理档位 reasoningEfforts）。dsh 的 llm-pi-ai 把「模型能力」放在模型条目上，
// 手工声明的路由不声明就什么都没有：不声明 input 收不了图片、不声明
// reasoningEfforts 模型菜单里连「推理等级」都不出现。字段名与 dsh 官方设置页
// （@deepseek-ai/dsh-llm-pi-ai 的 PiAiModelProfile）一致。
[[huxerui::composable]] huxerui::View DshModelFields(const FormStates& fs);

// 校验 dsh 的容量输入（0 = 空 = 不声明；负数 = 无法解析），失败时 toast 并
// 返回 false，由调用方中止保存。返回解析结果供装配使用。
bool DshCapacitiesValid(const FormStates& fs, huxerui::ToastHandle toast);
std::vector<std::string> DshInputModalities(const FormStates& fs);
std::int64_t DshCapacity(const huxerui::State<huxerui::TextEditingValue>& field);

// zcode：条目启用开关 + 模型清单（每模型参数面板：输入/输出模态、
// 上下文窗口、最大输出、思维链——直接改 modelsMeta 原值）。
[[huxerui::composable]] huxerui::View ZcodeFields(
    const FormStates& fs, huxerui::State<bool> zcodeEnabled,
    huxerui::State<bool> fetching, huxerui::StateList<std::string> fetchedModels,
    huxerui::StateList<std::string> modelList,
    huxerui::State<huxerui::TextEditingValue> addModel,
    huxerui::State<nlohmann::json> modelMeta, huxerui::State<int> expandedMeta,
    huxerui::State<huxerui::TextEditingValue> contextInput,
    huxerui::State<huxerui::TextEditingValue> outputInput,
    huxerui::TaskScope tasks, std::shared_ptr<huxerui::HttpClient> http,
    huxerui::ToastHandle toast);
bool ZcodeModelListValid(huxerui::StateList<std::string> modelList,
                         huxerui::ToastHandle toast);
void AssembleZcodeProvider(models::Provider& p,
                           huxerui::StateList<std::string> modelList,
                           huxerui::State<nlohmann::json> modelMeta);
void AfterSaveZcode(const std::string& savedId, bool enabled);

huxerui::View OfficialCard(std::string tool, bool active,
                           huxerui::ToastHandle toast,
                           huxerui::State<int> revision);

huxerui::View ProviderCard(std::string tool, const models::Provider& provider,
                           bool active, bool isCurrent,
                           huxerui::TaskScope tasks,
                           huxerui::ToastHandle toast,
                           huxerui::State<int> revision,
                           UsageCache usageCache,
                           huxerui::State<std::size_t> formToolIndex,
                           huxerui::State<std::string> formTarget);

// ---- dsh 增量多供应商页（providers_dsh.cpp）----
// ToolSpec::additiveProviders 的工具不走单列卡片列表，而是左右两列对照：
// 左 = live 实况，右 = 本应用留存。由 ProvidersPage 按注册表标记分流进来
// （签名即它需要的那部分参数）。批量动作、逐条收编 / 写入 / 设为默认都在
// 这一页里。
[[huxerui::composable]] huxerui::View DshProvidersPage(
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget);

// ---- ZCode 增量多供应商页（providers_zcode.cpp）----
// 与 dsh 同一套左右两列，差别在「当前用哪条」是条目自己的 enabled、密钥就在
// 条目里、且 ZCode 原生条目收编时不改名不改写（保留它自己的配置）。
[[huxerui::composable]] huxerui::View ZcodeProvidersPage(
    huxerui::State<int> revision, UsageCache usageCache,
    huxerui::State<std::size_t> formToolIndex,
    huxerui::State<std::string> formTarget);

} // namespace llmswitch::ui
