#include <huxerui/huxerui.h>
#include <huxerui/live2d.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pet.h"
#include "theme_colors.h"

import llmswitch.config;
import llmswitch.pet_profile;

namespace llmswitch::ui {
namespace {

namespace live2d = huxerui::live2d;

constexpr const char* kBundledEntryKey = "live2d/Hiyori/Hiyori.model3.json";
constexpr std::string_view kBundledFiles[] = {
    "live2d/Hiyori/Hiyori.model3.json",
    "live2d/Hiyori/Hiyori.moc3",
    "live2d/Hiyori/Hiyori.cdi3.json",
    "live2d/Hiyori/Hiyori.physics3.json",
    "live2d/Hiyori/Hiyori.pose3.json",
    "live2d/Hiyori/Hiyori.userdata3.json",
    "live2d/Hiyori/Hiyori.2048/texture_00.png",
    "live2d/Hiyori/Hiyori.2048/texture_01.png",
    "live2d/Hiyori/motions/Hiyori_m01.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m02.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m03.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m04.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m05.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m06.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m07.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m08.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m09.motion3.json",
    "live2d/Hiyori/motions/Hiyori_m10.motion3.json",
};
using BundledResources = std::map<std::string, huxerui::RawAsset>;

std::optional<live2d::MotionGroup> FindMotionGroup(
    const live2d::ModelAsset& asset, const std::string& preferred,
    bool allowFirst = false) {
    std::optional<live2d::MotionGroup> first;
    for (const auto& group : asset.Info().motion_groups) {
        if (group.count <= 0) continue;
        if (group.name == preferred) return group;
        if (!first) first = group;
    }
    return allowFirst ? first : std::nullopt;
}

std::string CurrentState(const std::shared_ptr<PetController>& controller) {
    if (controller->failure.Get()) return "failed";
    if (controller->donePendingAck.Get()) return "completed";
    if (controller->waitingForInput.Get()) return "waiting";
    if (controller->streaming.Get() || controller->agentWorking.Get()) return "working";
    return "idle";
}

const pet_profile::MotionCue& IdleCue(const pet_profile::Live2DProfile& profile) {
    static const pet_profile::MotionCue fallback{.motion = "Idle", .loop = true};
    if (const auto* cue = pet_profile::FindMotionCue(profile, "idle")) return *cue;
    return fallback;
}

const pet_profile::MotionCue& CueForState(
    const pet_profile::Live2DProfile& profile, const std::string& state) {
    if (const auto* cue = pet_profile::FindMotionCue(profile, state)) return *cue;
    return IdleCue(profile);
}

bool PlayCue(const live2d::ModelAsset& asset, const live2d::ModelController& command,
             const pet_profile::MotionCue& cue) {
    bool played = false;
    if (!cue.motion.empty()) {
        if (const auto group = FindMotionGroup(asset, cue.motion, cue.motion == "Idle")) {
            const auto result = command.PlayMotion(
                group->name, 0,
                {.priority = cue.priority, .force = cue.priority > 1, .loop = cue.loop});
            played = result.Succeeded();
        }
    }
    if (!cue.expression.empty()) {
        static_cast<void>(command.SetExpression(cue.expression));
    } else {
        static_cast<void>(command.ClearExpression());
    }
    return played;
}

std::vector<live2d::ParameterInput> ParametersForCue(
    const live2d::ModelAsset& asset, const pet_profile::MotionCue& cue) {
    std::vector<live2d::ParameterInput> result;
    for (const auto& parameter : cue.parameters) {
        const auto declared = std::ranges::find_if(
            asset.Info().parameters, [&](const live2d::ParameterInfo& candidate) {
                return candidate.id == parameter.id;
            });
        if (declared == asset.Info().parameters.end()) continue;
        result.push_back({
            .id = parameter.id,
            .value = parameter.value,
            .blend = parameter.blend == "add" ? live2d::ParameterBlend::Add
                                              : live2d::ParameterBlend::Override,
            .weight = parameter.weight,
        });
    }
    return result;
}

pet_profile::Live2DModel BundledModel() {
    pet_profile::Live2DModel model{.id = "Hiyori"};
    model.profile.displayName = "Hiyori";
    model.profile.states = {
        {"idle", {.motion = "Idle", .loop = true}},
        {"completed", {.motion = "TapBody", .priority = 3}},
        {"failed", {.motion = "TapBody", .priority = 3}},
    };
    return model;
}

huxerui::View LoadingBadge(std::string text, bool error = false) {
    return huxerui::Text(std::move(text))
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            error ? huxerui::Color::Rgb(240, 115, 108)
                  : huxerui::Color::Rgb(224, 242, 254)})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(14.0F, 6.0F)),
              huxerui::Background(huxerui::Color::Rgb(16, 18, 20, 0.86F)),
              huxerui::CornerRadius(12.0F),
              huxerui::Border{huxerui::Color::Rgb(224, 242, 254, 0.14F)},
              huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Center),
              huxerui::Grow(1.0F));
}

} // namespace

bool PetLive2DAvailable() { return true; }

[[huxerui::composable]]
huxerui::View PetLive2DBody(const std::shared_ptr<PetController>& controller) {
    auto model = huxerui::UseState(pet_profile::Live2DModel{});
    auto asset = huxerui::UseState(live2d::ModelAsset{});
    auto controller2d = huxerui::UseState(live2d::ModelController{});
    auto status = huxerui::UseState<std::string>({});
    auto ready = huxerui::UseState(false);
    auto settled = huxerui::UseState(false);
    auto loadGeneration = huxerui::UseState<std::uint64_t>(0);
    auto reaction = huxerui::UseState(false);
    auto reactionEpoch = huxerui::UseState(0);
    const auto tasks = huxerui::UseTaskScope();
    const auto window = huxerui::UseWindow();

    std::shared_ptr<const BundledResources> bundled;
    {
        auto resources = std::make_shared<BundledResources>();
        bool complete = true;
        try {
            for (const std::string_view key : kBundledFiles) {
                (*resources)[std::string(key)] = huxerui::UseRawResource(
                    huxerui::RawResource{"app", std::string("raw/") + std::string(key)});
            }
        } catch (const std::exception&) {
            complete = false;
        }
        if (complete) bundled = std::move(resources);
    }

    huxerui::Lifecycle([tasks, model, asset, controller2d, status, ready, settled,
                        loadGeneration, controller, bundled] {
        const std::uint64_t request = loadGeneration.Get() + 1;
        loadGeneration = request;
        ready = false;
        tasks.Launch([model, asset, controller2d, status, ready, settled,
                      loadGeneration, controller, bundled, request]() -> huxerui::Task<void> {
            pet_profile::Live2DModel selectedModel;
            bool hasCustom = false;
            try {
                selectedModel = co_await huxerui::RunWorker([selected = controller->live2dModel.Get()] {
                    return pet_profile::ResolveLive2DModel(cfg::dataDir() / "live2d", selected);
                });
                hasCustom = true;
            } catch (const std::exception& e) {
                if (!controller->live2dModel.Get().empty() || !bundled) {
                    status = std::format("{}，已显示莲花球", e.what());
                    co_return;
                }
            }
            if (loadGeneration.Get() != request) co_return;
            huxerui::Result<live2d::ModelAsset, live2d::LoadError> loaded =
                co_await [&]() -> huxerui::Task<
                    huxerui::Result<live2d::ModelAsset, live2d::LoadError>> {
                if (hasCustom) {
                    co_return co_await live2d::LoadModelAsync(
                        huxerui::File(selectedModel.entry.string()));
                }
                selectedModel = BundledModel();
                co_return co_await live2d::LoadModelAsync(
                    huxerui::RawResource{"app", std::string("raw/") + kBundledEntryKey},
                    [bundled](std::string path)
                        -> huxerui::Task<huxerui::IoResult<huxerui::AsyncInputStream>> {
                        const auto it = bundled->find(path);
                        if (it == bundled->end()) {
                            co_return huxerui::IoResult<huxerui::AsyncInputStream>::Failure(
                                {huxerui::IoErrorCode::NotFound,
                                 "内置示例资源缺失：" + path});
                        }
                        co_return huxerui::IoResult<huxerui::AsyncInputStream>::Success(
                            co_await it->second.OpenReadAsync());
                    });
            }();
            if (loadGeneration.Get() != request) co_return;
            if (!loaded.Succeeded()) {
                status = std::format("{}，已显示莲花球", loaded.Error().message);
                co_return;
            }
            model = std::move(selectedModel);
            controller2d = live2d::ModelController{};
            asset = std::move(loaded).Value();
            settled = false;
            status = "";
        });
        return [loadGeneration, request] {
            if (loadGeneration.Get() == request) loadGeneration = request + 1;
        };
    });

    const int playEpoch = controller->play.Get().epoch;
    huxerui::Lifecycle([playEpoch, reaction, reactionEpoch, tasks] {
        if (playEpoch <= 0 || reactionEpoch.Get() == playEpoch) return;
        reactionEpoch = playEpoch;
        reaction = true;
        tasks.Launch([playEpoch, reaction, reactionEpoch]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::milliseconds{900});
            if (reactionEpoch.Get() == playEpoch) reaction = false;
        });
    }, playEpoch);

    const live2d::ModelAsset current = asset.Get();
    if (!current.HasValue()) {
        if (!status.Get().empty()) return PetSkinFallback(controller, status.Get());
        return LoadingBadge("加载 Live2D…");
    }

    const live2d::ModelController command = controller2d.Get();
    const pet_profile::Live2DModel currentModel = model.Get();
    const pet_profile::Live2DProfile& profile = currentModel.profile;
    const std::string baseState = CurrentState(controller);
    const std::string state = reaction.Get() ? "completed" : baseState;
    const pet_profile::MotionCue cue = CueForState(profile, state);
    const auto play_cue = [current, command](const pet_profile::MotionCue& selected) {
        return PlayCue(current, command, selected);
    };

    huxerui::Lifecycle([current, command, cue, ready, settled, play_cue] {
        settled = false;
        if (ready.Get()) static_cast<void>(play_cue(cue));
    }, current, state, ready.Get());

    const pet_profile::MotionCue& parameterCue = settled.Get()
        ? (baseState == "completed" || baseState == "failed"
               ? IdleCue(profile)
               : CueForState(profile, baseState))
        : cue;
    huxerui::View body = live2d::ModelView(
        current,
        {
            .controller = command,
            .fit = huxerui::ImageFit::Contain,
            .paused = window.LifecycleState() == huxerui::WindowLifecycleState::Background,
            .playback_rate = baseState == "working" ? 1.15F : 1.0F,
            .parameters = ParametersForCue(current, parameterCue),
        })
        .On<live2d::ModelEvents::Ready>([ready, settled, cue, play_cue] {
            ready = true;
            settled = false;
            static_cast<void>(play_cue(cue));
        })
        .On<live2d::ModelEvents::HitAreaTapped>(
            [profile, play_cue, settled](std::string area) {
                const auto it = profile.hitAreas.find(area);
                if (it != profile.hitAreas.end()) {
                    static_cast<void>(play_cue(it->second));
                } else {
                    static_cast<void>(play_cue(CueForState(profile, "completed")));
                }
                settled = false;
            })
        .On<live2d::ModelEvents::MotionFinished>(
            [profile, baseState, play_cue, settled](live2d::MotionFinished) {
                const std::string restore =
                    baseState == "completed" || baseState == "failed" ? "idle" : baseState;
                static_cast<void>(play_cue(CueForState(profile, restore)));
                settled = true;
            })
        .On<live2d::ModelEvents::PlaybackFailed>(
            [asset, status, ready](live2d::PlaybackError error) {
                ready = false;
                status = std::format("Live2D 渲染失败：{}，已显示莲花球", error.message);
                asset = live2d::ModelAsset{};
            })
        .With(huxerui::Grow(1.0F));
    if (!status.Get().empty()) {
        body = huxerui::Stack {
            std::move(body),
            LoadingBadge(status.Get(), true)
                .With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                                     huxerui::VerticalAlignment::End)),
        }.With(huxerui::Grow(1.0F));
    }
    return body;
}

} // namespace llmswitch::ui
