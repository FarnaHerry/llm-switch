// provider_form_aggregate.cpp — 聚合供应商编辑页。
//
// 聚合供应商不再手填 URL/Key：直接从当前 Agent 的现有供应商里勾选供应商，
// 凭据只保留在原供应商条目里。勾选会带入它已知的全部模型（主模型
// 优先，再按 models 清单顺序），每个模型单独一条成员，可拖拽或用上下按钮
// 调整尝试顺序；对 Agent 暴露的模型 ID 按供应商内部去重后整组合并，并拼成
// 「供应商名/模型」。选中后 Agent 的 live 配置指向本地路由，由 router 按请求
// model 精确匹配成员并剥掉前缀，429/5xx 时按列表顺序继续尝试。成员与目标
// Agent 协议不同时由 router 转换（当前支持 Anthropic 目标接收 OpenAI Chat
// 成员）；聚合项不能嵌套另一个聚合项。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "providers_internal.h"

import llmswitch.models;
import llmswitch.net;
import llmswitch.store;

namespace llmswitch::ui {
namespace {

struct MemberInfo {
    std::string id;
    std::string name;
    std::string url;
    std::string apiFormat;
    std::vector<std::string> models;
};

// 聚合的一条模型成员。同一供应商会按模型拆成多条；顺序就是 router 的
// 兜底顺序。apiFormat 是成员端点协议（"anthropic" / "openai-chat" /
// "openai-responses"），与目标 Agent 不同时由 router 转换。
struct ModelEntry {
    std::string providerId;
    std::string providerName;
    std::string model;
    std::string apiFormat;

    bool operator==(const ModelEntry&) const = default;
};

const std::vector<std::string> kAggregateProtocolLabels = {
    "Anthropic Messages",
    "OpenAI Chat",
    "OpenAI Responses",
};

int ProtocolIndex(std::string_view format) {
    if (format == "anthropic") return 0;
    if (format == "openai-responses") return 2;
    return 1;
}

std::string ProtocolFromIndex(int index) {
    if (index == 0) return "anthropic";
    if (index == 2) return "openai-responses";
    return "openai-chat";
}

std::string NormalizedProtocol(std::string_view format) {
    return std::string(models::normalizeApiFormat(format));
}

bool ConversionSupported(std::string_view target, std::string_view source) {
    // 当前第一刀：Anthropic 目标（Claude 系）接收 OpenAI Chat 成员。
    return target == source ||
           (target == "anthropic" && source == "openai-chat");
}

struct ModelDrag {
    std::string key;
};

std::string EntryKey(const ModelEntry& entry) {
    return entry.providerId + "\x1f" + entry.model;
}

std::string ModelSummary(const std::vector<std::string>& models) {
    if (models.empty()) return "未设置模型";
    if (models.size() == 1) return "1 个模型：" + models.front();
    return std::format("{} 个模型 · 首个 {}", models.size(), models.front());
}

std::vector<std::string> MergeFetchedModels(
    const models::Provider& provider,
    const std::vector<std::string>& fetched) {
    std::vector<std::string> merged = models::knownModels(provider);
    for (const auto& id : fetched) {
        if (!id.empty() &&
            std::ranges::find(merged, id) == merged.end()) {
            merged.push_back(id);
        }
    }
    return merged;
}

std::vector<MemberInfo> CandidateMembers(std::string_view tool,
                                         std::string_view selfId,
                                         std::string_view targetApiFormat) {
    // Claude / Codex 没有 apiFormat 字段，才从 URL/上游格式推断成员协议；
    // 其他工具的旧聚合项没有协议字段时默认与目标一致，保持原有直通行为。
    const bool inferMemberProtocol =
        tool == "claude-code" || tool == "claude" || tool == "codex";
    std::vector<MemberInfo> members;
    try {
        const auto& group = providerStore().group(std::string(tool));
        for (const auto& provider : group.providers) {
            if (provider.id == selfId ||
                models::isAggregateProvider(provider)) {
                continue;
            }
            members.push_back(MemberInfo{
                .id = provider.id,
                .name = provider.name.empty() ? provider.id : provider.name,
                .url = models::effectiveBaseUrl(provider),
                .apiFormat =
                    inferMemberProtocol
                        ? std::string(
                              models::memberApiFormat(tool, provider))
                        : std::string(targetApiFormat),
                .models = models::knownModels(provider),
            });
        }
    } catch (...) {
    }
    return members;
}

bool HasEntry(const std::vector<ModelEntry>& entries,
              std::string_view providerId, std::string_view model) {
    return std::ranges::any_of(
        entries, [providerId, model](const ModelEntry& entry) {
            return entry.providerId == providerId && entry.model == model;
        });
}

void AppendMemberModels(std::vector<ModelEntry>& entries,
                        const MemberInfo& member,
                        std::string_view apiFormat) {
    if (member.models.empty()) {
        if (!HasEntry(entries, member.id, {})) {
            entries.push_back(ModelEntry{.providerId = member.id,
                                         .providerName = member.name,
                                         .model = {},
                                         .apiFormat = std::string(apiFormat)});
        }
        return;
    }
    for (const auto& model : member.models) {
        if (!HasEntry(entries, member.id, model)) {
            entries.push_back(ModelEntry{.providerId = member.id,
                                         .providerName = member.name,
                                         .model = model,
                                         .apiFormat = std::string(apiFormat)});
        }
    }
}

std::map<std::string, std::string> InitialProtocols(
    const models::Provider& initial,
    const std::vector<MemberInfo>& candidates) {
    std::map<std::string, std::string> protocols;
    for (const auto& candidate : candidates) {
        protocols[candidate.id] = candidate.apiFormat;
    }
    for (const auto& member : initial.upstreams) {
        if (member.providerId.empty() || member.apiFormat.empty()) continue;
        protocols[member.providerId] = NormalizedProtocol(member.apiFormat);
    }
    return protocols;
}

std::vector<ModelEntry> InitialEntries(
    const models::Provider& initial,
    const std::vector<MemberInfo>& candidates,
    const std::map<std::string, std::string>& protocols) {
    std::vector<ModelEntry> existing;
    std::vector<std::string> selectedProviders;
    for (const auto& member : initial.upstreams) {
        std::string id = member.providerId;
        if (id.empty()) {
            // 旧版直填成员：按访问 URL 尝试迁移到现有供应商；匹配不到就
            // 不再保留，用户需要重新勾选。
            for (const auto& candidate : candidates) {
                if (!member.baseUrl.empty() &&
                    candidate.url == member.baseUrl) {
                    id = candidate.id;
                    break;
                }
            }
        }
        if (id.empty()) continue;
        const auto candidate =
            std::ranges::find_if(candidates, [&id](const MemberInfo& c) {
                return c.id == id;
            });
        if (candidate == candidates.end()) continue;
        if (std::ranges::find(selectedProviders, id) ==
            selectedProviders.end()) {
            selectedProviders.push_back(id);
        }
        const auto protocolIt = protocols.find(id);
        const std::string protocol =
            protocolIt != protocols.end() ? protocolIt->second
                                          : candidate->apiFormat;
        // model 为空的旧成员不先造占位行；下面按供应商展开时统一补。
        if (!member.model.empty() &&
            !HasEntry(existing, candidate->id, member.model)) {
            existing.push_back(ModelEntry{.providerId = candidate->id,
                                          .providerName = candidate->name,
                                          .model = member.model,
                                          .apiFormat = protocol});
        }
    }
    // 按供应商首次出现的顺序输出：保留已有模型顺序，再补该供应商其余
    // 已知模型。旧聚合每个供应商通常只有一条，编辑时自然展开成整组。
    std::vector<ModelEntry> entries;
    for (const auto& id : selectedProviders) {
        const auto candidate =
            std::ranges::find_if(candidates, [&id](const MemberInfo& c) {
                return c.id == id;
            });
        if (candidate == candidates.end()) continue;
        const auto protocolIt = protocols.find(id);
        const std::string protocol =
            protocolIt != protocols.end() ? protocolIt->second
                                          : candidate->apiFormat;
        std::vector<ModelEntry> providerEntries;
        for (const auto& entry : existing) {
            if (entry.providerId == id) providerEntries.push_back(entry);
        }
        AppendMemberModels(providerEntries, *candidate, protocol);
        for (auto& entry : providerEntries) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::string ExposedModel(const ModelEntry& entry) {
    return models::memberModelId(entry.providerName, entry.model);
}

// Agent 侧看到的模型清单：每个供应商内部先去重，再按供应商首次出现顺序
// 合并，最后统一拼上「供应商名/模型」前缀。列表顺序与 router 的兜底顺序
// 相互独立。
std::vector<std::string> ModelChoices(const std::vector<ModelEntry>& entries) {
    std::vector<std::string> providerOrder;
    for (const auto& entry : entries) {
        if (std::ranges::find(providerOrder, entry.providerId) ==
            providerOrder.end()) {
            providerOrder.push_back(entry.providerId);
        }
    }
    std::vector<std::string> models;
    const auto add = [&models](const std::string& model) {
        if (!model.empty() &&
            std::ranges::find(models, model) == models.end()) {
            models.push_back(model);
        }
    };
    for (const auto& providerId : providerOrder) {
        for (const auto& entry : entries) {
            if (entry.providerId == providerId) add(ExposedModel(entry));
        }
    }
    return models;
}

std::string InitialDefaultModel(const models::Provider& initial,
                                const std::vector<ModelEntry>& entries) {
    if (!initial.model.empty()) {
        for (const auto& entry : entries) {
            const std::string exposed = ExposedModel(entry);
            if (initial.model == exposed || initial.model == entry.model) {
                return exposed;
            }
        }
    }
    for (const auto& entry : entries) {
        if (!entry.model.empty()) return ExposedModel(entry);
    }
    return {};
}

void MoveEntry(huxerui::State<std::vector<ModelEntry>> entries,
               std::size_t index, std::ptrdiff_t offset) {
    auto list = entries.Get();
    const auto target = static_cast<std::ptrdiff_t>(index) + offset;
    if (target < 0 ||
        target >= static_cast<std::ptrdiff_t>(list.size())) {
        return;
    }
    std::swap(list[index], list[static_cast<std::size_t>(target)]);
    entries = std::move(list);
}

} // namespace

[[huxerui::composable]] huxerui::View AggregateFormPage(
    std::string tool, models::Provider initial, bool isNew,
    huxerui::State<int> revision, huxerui::State<std::string> formTarget) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    const auto http = huxerui::UseService<huxerui::HttpClient>();
    auto fetching = huxerui::UseState(false);

    auto name = huxerui::UseState(
        huxerui::TextEditingValue{initial.name});
    auto notes = huxerui::UseState(
        huxerui::TextEditingValue{initial.notes});
    const std::string targetFormat =
        std::string(models::targetApiFormat(tool, initial));
    const std::vector<MemberInfo> candidates =
        CandidateMembers(tool, initial.id, targetFormat);
    const auto initialProtocols = InitialProtocols(initial, candidates);
    auto memberProtocols = huxerui::UseState(initialProtocols);
    auto ordered = huxerui::UseState(
        InitialEntries(initial, candidates, initialProtocols));
    auto chosenModel = huxerui::UseState(
        InitialDefaultModel(initial, ordered.Get()));
    auto dropTargetKey = huxerui::UseState<std::string>({});
    auto manualProvider = huxerui::UseState<std::size_t>(0);
    auto manualModel = huxerui::UseState(huxerui::TextEditingValue{});

    const std::vector<ModelEntry> entries = ordered.Get();
    const std::vector<std::string> models = ModelChoices(entries);
    std::string effectiveModel = chosenModel.Get();
    if (std::ranges::find(models, effectiveModel) == models.end()) {
        effectiveModel = models.empty() ? std::string{} : models.front();
    }
    const std::size_t modelIndex =
        effectiveModel.empty()
            ? 0
            : static_cast<std::size_t>(std::distance(
                  models.begin(), std::ranges::find(models, effectiveModel)));

    std::vector<std::string> manualProviderNames;
    manualProviderNames.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        manualProviderNames.push_back(candidate.name);
    }
    const std::size_t manualIndex =
        candidates.empty()
            ? 0
            : std::min(manualProvider.Get(), candidates.size() - 1);

    const bool hasLegacyEntries = std::ranges::any_of(
        initial.upstreams, [](const models::ProviderUpstream& member) {
            return member.providerId.empty();
        });

    std::vector<huxerui::View> memberRows;
    memberRows.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const std::size_t selectedCount = static_cast<std::size_t>(
            std::ranges::count_if(entries, [&candidate](const ModelEntry& entry) {
                return entry.providerId == candidate.id;
            }));
        const bool checked = selectedCount > 0;
        const auto protocols = memberProtocols.Get();
        const auto protocolIt = protocols.find(candidate.id);
        const std::string protocol =
            protocolIt != protocols.end() ? protocolIt->second
                                          : candidate.apiFormat;
        const bool converted = protocol != targetFormat &&
                               ConversionSupported(targetFormat, protocol);
        const bool unsupported = protocol != targetFormat && !converted;
        // 协议和地址前缀是两件事：把 Anthropic 供应商切成 Chat 时，
        // 仍带 /anthropic 的地址会拼出错误端点，先明确提示用户改 URL。
        const bool addressMismatch =
            converted && protocol == "openai-chat" &&
            candidate.url.contains("/anthropic");
        huxerui::View row = huxerui::Row {
            huxerui::Checkbox(checked).OnChanged(
                [ordered, memberProtocols, member = candidate](bool on) {
                    auto list = ordered.Get();
                    std::erase_if(list, [&member](const ModelEntry& entry) {
                        return entry.providerId == member.id;
                    });
                    if (on) {
                        const auto protocols = memberProtocols.Get();
                        const auto it = protocols.find(member.id);
                        AppendMemberModels(
                            list, member,
                            it != protocols.end() ? it->second
                                                  : member.apiFormat);
                    }
                    ordered = std::move(list);
                }),
            huxerui::Column {
                huxerui::Text(candidate.name).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    theme.colors.on_surface}),
                huxerui::Text(
                    checked ? std::format("已带入 {} 个模型", selectedCount)
                            : ModelSummary(candidate.models))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}),
                huxerui::Row {
                    huxerui::Select(
                        kAggregateProtocolLabels,
                        ProtocolIndex(protocol),
                        [](const std::string& option) {
                            return huxerui::Text(option).Key(option);
                        })
                        .Label("成员协议")
                        .OnChanged([ordered, memberProtocols,
                                    id = candidate.id](std::size_t index) {
                            const std::string format = ProtocolFromIndex(
                                static_cast<int>(index));
                            auto protocols = memberProtocols.Get();
                            protocols[id] = format;
                            memberProtocols = protocols;
                            auto list = ordered.Get();
                            for (auto& entry : list) {
                                if (entry.providerId == id) {
                                    entry.apiFormat = format;
                                }
                            }
                            ordered = std::move(list);
                        })
                        .With(huxerui::Frame{.width = 190.0F}),
                    huxerui::Text(
                        addressMismatch
                            ? "地址仍带 /anthropic，请先用 Chat 兼容地址"
                            : (converted
                                   ? "自动转换为目标协议"
                                   : (unsupported ? "暂不支持转换"
                                                  : "与目标一致，直通")))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kChip),
                            (unsupported || addressMismatch)
                                ? theme.colors.error
                                : (converted
                                       ? theme.colors.primary
                                       : theme.colors
                                             .on_surface_variant)}),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center)),
            }.With(huxerui::Spacing(4.0F), huxerui::Grow(1.0F)),
            huxerui::Text(candidate.url.empty() ? "未设置 URL" : candidate.url)
                .Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kChip),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
               huxerui::Padding(
                   huxerui::EdgeInsets::Symmetric(12.0F, 8.0F)),
               huxerui::Background(checked ? theme.colors.primary_container
                                           : theme.colors.surface_container_high),
               huxerui::CornerRadius(theme.shapes.extra_small),
               huxerui::Border{checked ? theme.colors.primary
                                       : theme.colors.outline,
                               1.0F});
        memberRows.push_back(std::move(row).Key("aggregate-member:" +
                                                candidate.id));
    }

    huxerui::View memberList =
        memberRows.empty()
            ? huxerui::View{
                  huxerui::Text(
                      "当前 Agent 还没有可用的普通供应商；请先在供应商页新增，"
                      "再回来做聚合。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::Column(std::move(memberRows))
                                .With(huxerui::Spacing(8.0F),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Stretch))};

    std::vector<huxerui::View> orderRows;
    orderRows.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const ModelEntry entry = entries[index];
        const std::string key = EntryKey(entry);
        const bool dropActive = dropTargetKey.Get() == key;
        huxerui::View dragHandle =
            huxerui::Image(app::images::menu)
                .With(huxerui::Frame{24.0F, 24.0F},
                      huxerui::Tooltip("按住拖动调整顺序"),
                      huxerui::PointerCursor(
                          huxerui::PointerCursorKind::Grab),
                      huxerui::DragSource(
                          ModelDrag{.key = key},
                          huxerui::DragGesture{
                              .minimum_press_duration =
                                  std::chrono::duration<double>{0.18}}))
                .On<huxerui::DragSourceEvents::Ended>(
                    [dropTargetKey](const huxerui::DragDropResult&) {
                        dropTargetKey = "";
                    })
                .On<huxerui::DragSourceEvents::Canceled>(
                    [dropTargetKey](const huxerui::DragEvent&) {
                        dropTargetKey = "";
                    })
                .Key("aggregate-drag:" + key);
        huxerui::View entryRow = huxerui::Row {
            std::move(dragHandle),
            huxerui::Text(std::format("{:02}", index + 1))
                .Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kChip),
                    theme.colors.on_surface_variant}),
            huxerui::Column {
                huxerui::Text(entry.model.empty() ? "未设置模型"
                                                  : ExposedModel(entry))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::Monospace(font_size::kBody),
                        theme.colors.on_surface}),
                huxerui::Text(entry.providerName)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}),
            }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F)),
            huxerui::Button("移除")
                .OnClick([ordered, index] {
                    auto list = ordered.Get();
                    if (index < list.size()) {
                        list.erase(list.begin() +
                                   static_cast<std::ptrdiff_t>(index));
                        ordered = std::move(list);
                    }
                })
                .With(huxerui::Tooltip("从聚合中移除这个模型")),
            huxerui::Button("上移")
                .OnClick([ordered, index] { MoveEntry(ordered, index, -1); })
                .With(huxerui::Enabled(index > 0),
                      huxerui::Tooltip("向前移动，提高兜底优先级")),
            huxerui::Button("下移")
                .OnClick([ordered, index] { MoveEntry(ordered, index, 1); })
                .With(huxerui::Enabled(index + 1 < entries.size()),
                      huxerui::Tooltip("向后移动，降低兜底优先级")),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
               huxerui::Padding(
                   huxerui::EdgeInsets::Symmetric(10.0F, 7.0F)),
               huxerui::Background(dropActive ? theme.colors.primary_container
                                              : theme.colors.surface_container_high),
               huxerui::CornerRadius(theme.shapes.extra_small),
               huxerui::Border{dropActive ? theme.colors.primary
                                          : theme.colors.outline,
                               1.0F},
               huxerui::DropTarget::Accepts<ModelDrag>())
            .On<huxerui::DropEvents<ModelDrag>::Entered>(
                [dropTargetKey, key](const ModelDrag&,
                                     const huxerui::DropEvent&) {
                    dropTargetKey = key;
                })
            .On<huxerui::DropEvents<ModelDrag>::Exited>(
                [dropTargetKey, key](const ModelDrag&,
                                     const huxerui::DropEvent&) {
                    if (dropTargetKey.Get() == key) dropTargetKey = "";
                })
            .On<huxerui::DropEvents<ModelDrag>::Dropped>(
                [ordered, dropTargetKey, key](const ModelDrag& payload,
                                               const huxerui::DropEvent&) {
                    dropTargetKey = "";
                    if (payload.key == key) return;
                    auto list = ordered.Get();
                    const auto from = std::ranges::find_if(
                        list, [&payload](const ModelEntry& entry) {
                            return EntryKey(entry) == payload.key;
                        });
                    if (from == list.end()) return;
                    const auto target = std::ranges::find_if(
                        list, [&key](const ModelEntry& entry) {
                            return EntryKey(entry) == key;
                        });
                    if (target == list.end() || from == target) return;
                    const bool movingDown =
                        std::distance(list.begin(), from) <
                        std::distance(list.begin(), target);
                    ModelEntry value = std::move(*from);
                    list.erase(from);
                    auto insertAt = std::ranges::find_if(
                        list, [&key](const ModelEntry& entry) {
                            return EntryKey(entry) == key;
                        });
                    if (insertAt == list.end()) return;
                    if (movingDown) ++insertAt;
                    list.insert(insertAt, std::move(value));
                    ordered = std::move(list);
                });
        orderRows.push_back(std::move(entryRow).Key("aggregate-order:" + key));
    }

    huxerui::View orderList =
        orderRows.empty()
            ? huxerui::View{
                  huxerui::Text(
                      "勾选供应商后，它的全部已知模型会按默认顺序出现在这里。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::Column(std::move(orderRows))
                                .With(huxerui::Spacing(6.0F),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Stretch))};

    huxerui::View modelPicker =
        models.empty()
            ? huxerui::View{
                  huxerui::Text(
                      "选中的供应商都未设置模型；切换后 Agent 会沿用现有模型。")
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
            : huxerui::View{
                  huxerui::Select(
                      models, modelIndex,
                      [](const std::string& option) {
                          return huxerui::Text(option).Key(option);
                      })
                      .Label("默认模型")
                      .OnChanged([chosenModel, models](std::size_t index) {
                          if (index < models.size()) {
                              chosenModel = models[index];
                          }
                      })};

    const std::string targetProtocolLabel =
        targetFormat == "anthropic"
            ? "Anthropic Messages"
            : (targetFormat == "openai-chat"
                   ? "OpenAI Chat"
                   : (targetFormat == "openai-responses"
                          ? "OpenAI Responses"
                          : targetFormat));
    const std::string conversionHint =
        targetFormat == "anthropic"
            ? "OpenAI Chat 成员会由本地路由转换成 Anthropic 请求/响应；"
              "OpenAI Responses 成员暂未支持转换。"
            : (tool == "codex"
                   ? "Codex 聚合固定用 chat wire_api：Chat 成员直通，"
                     "Anthropic/Responses 成员暂未支持转换。"
                   : "成员协议按供应商配置自动推断；与目标不一致时由本地"
                     "路由尝试转换，暂不支持的方向会明确报错。");
    const huxerui::View protocolHint =
        huxerui::View{
            huxerui::Column {
                huxerui::Text("目标协议：" + targetProtocolLabel)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                huxerui::Text(conversionHint)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}),
            }.With(huxerui::Spacing(3.0F))};

    huxerui::View body = huxerui::Column {
        huxerui::ScrollView(
            huxerui::Column {
                huxerui::Text(
                    "聚合供应商把当前 Agent 的多个现有供应商组织成一个可选"
                    "条目。勾选供应商会带入它保存的全部模型，每个模型是一条"
                    "成员；对 Agent 暴露的模型 ID 会拼成「供应商名/模型」，"
                    "并先按供应商内部去重后整组合并。请求先发到本地路由，按"
                    "模型精确匹配，成员返回 429/5xx 或连接失败时按下方模型"
                    "顺序继续尝试。每个供应商会显示成员协议；Claude 系聚合"
                    "可自动把 OpenAI Chat 成员转换为 Anthropic Messages。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                huxerui::TextField(name.Get())
                    .Label("名称")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([name](const huxerui::TextEditingValue& v) {
                        name = v;
                    }),
                huxerui::TextField(notes.Get())
                    .Label("备注（可选）")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([notes](const huxerui::TextEditingValue& v) {
                        notes = v;
                    }),
                protocolHint,
                huxerui::Row {
                    huxerui::Text("选择现有供应商")
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kBody)
                                .WithWeight(huxerui::FontWeight::SemiBold),
                            theme.colors.on_surface}),
                    huxerui::Button(fetching.Get() ? "获取中…"
                                                   : "获取全量模型")
                        .OnClick([=] {
                            if (fetching.Get() || candidates.empty()) return;
                            fetching = true;
                            tasks.Launch([=]() -> huxerui::Task<void> {
                                int updated = 0;
                                int failed = 0;
                                for (const auto& candidate : candidates) {
                                    try {
                                        models::Provider provider;
                                        for (const auto& item :
                                             providerStore().group(tool).providers) {
                                            if (item.id == candidate.id) {
                                                provider = item;
                                                break;
                                            }
                                        }
                                        if (provider.baseUrl.empty() ||
                                            provider.apiKey.empty()) {
                                            ++failed;
                                            continue;
                                        }
                                        const std::string base =
                                            models::effectiveBaseUrl(provider);
                                        const auto urls =
                                            net::modelListUrlCandidates(
                                                base,
                                                provider.upstreamFormat);
                                        const auto ids = co_await
                                            provider_detail::FetchModelIdsWithFallback(
                                                http, urls,
                                                provider.apiKey,
                                                provider.upstreamFormat);
                                        if (ids.empty()) {
                                            ++failed;
                                            continue;
                                        }
                                        provider.models =
                                            MergeFetchedModels(provider, ids);
                                        providerStore().updateProvider(
                                            tool, provider);
                                        auto list = ordered.Get();
                                        const bool selected = std::ranges::any_of(
                                            list, [&candidate](
                                                      const ModelEntry& entry) {
                                                return entry.providerId ==
                                                       candidate.id;
                                            });
                                        if (selected) {
                                            const auto protocols =
                                                memberProtocols.Get();
                                            const auto protocolIt =
                                                protocols.find(candidate.id);
                                            const std::string protocol =
                                                protocolIt != protocols.end()
                                                    ? protocolIt->second
                                                    : candidate.apiFormat;
                                            for (const auto& id : ids) {
                                                if (!HasEntry(list,
                                                              candidate.id,
                                                              id)) {
                                                    list.push_back(ModelEntry{
                                                        .providerId =
                                                            candidate.id,
                                                        .providerName =
                                                            candidate.name,
                                                        .model = id,
                                                        .apiFormat = protocol,
                                                    });
                                                }
                                            }
                                            ordered = std::move(list);
                                        }
                                        ++updated;
                                    } catch (...) {
                                        ++failed;
                                    }
                                }
                                fetching = false;
                                revision = revision.Get() + 1;
                                toast.Show(std::format(
                                    "已更新 {} 个供应商的模型（{} 个失败）",
                                    updated, failed));
                            });
                        })
                        .With(huxerui::Enabled(!fetching.Get() &&
                                               !candidates.empty()),
                              huxerui::Tooltip(
                                  "按各供应商的地址与密钥拉取完整模型列表")),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::MainAlign(
                           huxerui::MainAxisAlignment::Start),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Center)),
                huxerui::Text(
                    "默认直接带入直连模式已配置的模型（主模型、三档映射和"
                    "已保存的模型清单）；需要完整菜单时点「获取全量模型」"
                    "重新请求。勾选供应商会把这些模型加入尝试顺序，也可以在"
                    "下方手动增删模型。凭据仍只保存在原供应商条目里。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                hasLegacyEntries
                    ? huxerui::View{
                          huxerui::Text(
                              "这个聚合项含旧版直填 API；保存后会改为下方"
                              "勾选的现有供应商。")
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kChip),
                                  theme.colors.error})}
                    : huxerui::View{huxerui::Row{}},
                std::move(memberList),
                huxerui::Text("模型尝试顺序")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                huxerui::Text(
                    "拖动手柄，或用「上移 / 下移」调整。Agent 侧的模型 ID 是"
                    "「供应商名/模型」；请求精确命中的成员优先，未命中时按"
                    "这里的顺序逐条兜底。")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                std::move(orderList),
                huxerui::Text("手动添加模型")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                candidates.empty()
                    ? huxerui::View{
                          huxerui::Text("先勾选或新增一个现有供应商，才能手动添加模型。")
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kCaption),
                                  theme.colors.on_surface_variant})}
                    : huxerui::View{
                          huxerui::Row {
                              huxerui::Select(
                                  manualProviderNames, manualIndex,
                                  [](const std::string& option) {
                                      return huxerui::Text(option).Key(option);
                                  })
                                  .Label("供应商")
                                  .OnChanged([manualProvider](std::size_t index) {
                                      manualProvider = index;
                                  })
                                  .With(huxerui::Frame{.width = 180.0F}),
                              huxerui::TextField(manualModel.Get())
                                  .Label("模型 ID")
                                  .Variant(huxerui::TextFieldVariant::Outlined)
                                  .OnChanged([manualModel](
                                                 const huxerui::TextEditingValue& v) {
                                      manualModel = v;
                                  })
                                  .With(huxerui::Grow(1.0F)),
                              huxerui::Button("添加")
                                  .OnClick([candidates, ordered, manualProvider,
                                            manualModel, memberProtocols,
                                            toast] {
                                      if (candidates.empty()) return;
                                      const std::size_t index = std::min(
                                          manualProvider.Get(),
                                          candidates.size() - 1);
                                      const std::string model =
                                          manualModel.Get().text;
                                      if (model.empty()) {
                                          toast.Show("模型 ID 不能为空");
                                          return;
                                      }
                                      auto list = ordered.Get();
                                      const MemberInfo& member = candidates[index];
                                      if (HasEntry(list, member.id, model)) {
                                          toast.Show("该模型已在尝试顺序中");
                                          return;
                                      }
                                      const auto protocols =
                                          memberProtocols.Get();
                                      const auto protocolIt =
                                          protocols.find(member.id);
                                      list.push_back(ModelEntry{
                                          .providerId = member.id,
                                          .providerName = member.name,
                                          .model = model,
                                          .apiFormat =
                                              protocolIt != protocols.end()
                                                  ? protocolIt->second
                                                  : member.apiFormat,
                                      });
                                      ordered = std::move(list);
                                      manualModel =
                                          huxerui::TextEditingValue{};
                                  }),
                          }.With(huxerui::Spacing(8.0F),
                                 huxerui::CrossAlign(
                                     huxerui::CrossAxisAlignment::Center))},
                candidates.empty()
                    ? huxerui::View{huxerui::Row{}}
                    : huxerui::View{
                          huxerui::Text(
                              "手动添加的模型填成员原始 ID，不要带「供应商名/」"
                              "前缀；它只影响本聚合项。")
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kChip),
                                  theme.colors.on_surface_variant})},
                std::move(modelPicker),
            }.With(huxerui::Spacing(10.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                .With(huxerui::Grow(1.0F)),
        huxerui::Row {
            huxerui::Button("取消").OnClick([formTarget] { formTarget = ""; }),
            huxerui::Button(isNew ? "添加" : "保存").OnClick(
                [=] {
                    const std::string formName = name.Get().text;
                    if (formName.empty()) {
                        toast.Show("名称不能为空");
                        return;
                    }
                    const std::vector<ModelEntry> currentEntries = ordered.Get();
                    if (currentEntries.empty()) {
                        toast.Show("至少选择一个现有供应商");
                        return;
                    }
                    std::vector<models::ProviderUpstream> upstreams;
                    upstreams.reserve(currentEntries.size());
                    for (const auto& entry : currentEntries) {
                        upstreams.push_back(models::ProviderUpstream{
                            .name = entry.providerName,
                            .providerId = entry.providerId,
                            .model = entry.model,
                            .apiFormat = entry.apiFormat,
                        });
                    }
                    const std::vector<std::string> modelChoices =
                        ModelChoices(currentEntries);
                    std::string defaultModel = chosenModel.Get();
                    if (std::ranges::find(modelChoices, defaultModel) ==
                        modelChoices.end()) {
                        defaultModel = modelChoices.empty()
                                           ? std::string{}
                                           : modelChoices.front();
                    }
                    if (tool == "kimi-code" && defaultModel.empty()) {
                        toast.Show(
                            "Kimi Code 聚合需要默认模型：请先给至少一个成员"
                            "设置模型");
                        return;
                    }
                    models::Provider provider = initial;
                    provider.name = formName;
                    provider.notes = notes.Get().text;
                    provider.model = std::move(defaultModel);
                    provider.models = ModelChoices(currentEntries);
                    provider.baseUrl.clear();
                    provider.modelFetchUrl.clear();
                    provider.apiKey =
                        std::string(models::kAggregateApiKey);
                    provider.fullUrl = true;
                    provider.upstreamFormat = DefaultUpstreamFormat(tool);
                    // 聚合项自身对 Agent 暴露的协议：Claude/Codex 由工具固定，
                    // opencode/pi 等保留原值，不能因为保存成员列表而重置目标协议。
                    provider.apiFormat = initial.apiFormat;
                    provider.upstreams = std::move(upstreams);
                    const auto currentGroup = providerStore().group(tool);
                    const bool wasCurrent =
                        !isNew && currentGroup.mode == "aggregate" &&
                        currentGroup.current == provider.id;
                    try {
                        if (isNew) {
                            providerStore().addProvider(tool,
                                                        std::move(provider));
                        } else {
                            providerStore().updateProvider(tool, provider);
                        }
                        // 当前聚合项改了成员/默认模型时，live 配置里的 model
                        // 也要跟着走；路由本身读 store，不用重启。
                        if (wasCurrent) SwitchProvider(tool, provider.id);
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                        return;
                    }
                    revision = revision.Get() + 1;
                    toast.Show(isNew ? "已添加聚合供应商" : "已保存聚合供应商");
                    formTarget = "";
                }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    const std::string title =
        isNew ? "新增聚合供应商 — " + std::string(ToolName(tool))
              : "编辑聚合供应商 — " + initial.name;
    return PageScaffold(
        title,
        huxerui::Row {
            huxerui::Button("返回").OnClick([formTarget] { formTarget = ""; }),
        },
        std::move(body));
}

} // namespace llmswitch::ui
