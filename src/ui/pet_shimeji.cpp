#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pet.h"
#include "theme_colors.h"

import llmswitch.config;
import llmswitch.shimeji;

namespace llmswitch::ui {
namespace {

namespace fs = std::filesystem;
using huxerui::Point;
using huxerui::Rect;

struct ShimejiEvents {
    struct Moved : huxerui::Event<void(Point)> {};
    struct ActionRequested : huxerui::Event<void(std::string)> {};
};

struct Frame {
    huxerui::ImageAsset image;
    shimeji::Pose pose;
};

struct ActionFrames {
    std::vector<Frame> frames;
};

struct Runtime {
    shimeji::Pack pack;
    std::map<std::string, std::shared_ptr<ActionFrames>, std::less<>> cache;
    std::vector<std::string> lru;

    void Touch(const std::string& name) {
        lru.erase(std::ranges::remove(lru, name).begin(), lru.end());
        lru.push_back(name);
        while (lru.size() > 4) {
            const std::string oldest = lru.front();
            lru.erase(lru.begin());
            cache.erase(oldest);
        }
    }
};

bool IsWithin(const fs::path& root, const fs::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

fs::path ResolvePackRoot(std::string selected) {
    const fs::path root = cfg::dataDir() / "shimeji";
    if (selected.empty()) {
        const auto packs = shimeji::ScanPacks(root);
        if (packs.empty()) throw std::runtime_error("未安装 Shimeji 模型包");
        return packs.front();
    }
    const fs::path relative = fs::path(selected).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("Shimeji 模型选择越出数据目录");
    }
    std::error_code ec;
    const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    const fs::path candidate = fs::weakly_canonical(root / relative, ec);
    if (ec || !IsWithin(canonicalRoot, candidate)) {
        throw std::runtime_error("Shimeji 模型选择无效");
    }
    return candidate;
}

std::shared_ptr<ActionFrames> LoadActionFrames(const shimeji::Pack& pack,
                                               const std::string& name) {
    const shimeji::Action* action = shimeji::FindAction(pack, name);
    if (!action) throw std::runtime_error("Shimeji 动作不存在：" + name);
    if (action->poses.size() > 256) throw std::runtime_error("Shimeji 单动作帧数超过限制");
    auto result = std::make_shared<ActionFrames>();
    std::size_t totalBytes = 0;
    for (const shimeji::Pose& pose : action->poses) {
        const fs::path path = shimeji::ImagePath(pack, pose);
        std::error_code ec;
        const std::uintmax_t bytes = fs::file_size(path, ec);
        if (ec || bytes > 8 * 1024 * 1024 || totalBytes + bytes > 32 * 1024 * 1024) {
            throw std::runtime_error("Shimeji 图片大小超过限制");
        }
        totalBytes += static_cast<std::size_t>(bytes);
        huxerui::ImageAsset image = huxerui::ImageAsset::FromFile(path);
        if (image.PixelWidth() == 0 || image.PixelHeight() == 0 ||
            image.PixelWidth() > 2048 || image.PixelHeight() > 2048) {
            throw std::runtime_error("Shimeji 图片尺寸超过限制");
        }
        result->frames.push_back({std::move(image), pose});
    }
    return result;
}

std::shared_ptr<Runtime> LoadRuntime(std::string selected) {
    auto runtime = std::make_shared<Runtime>();
    runtime->pack = shimeji::LoadPack(ResolvePackRoot(std::move(selected)));
    const shimeji::Action* idle = shimeji::ActionForActivity(
        runtime->pack, shimeji::Activity::idle);
    if (!idle) throw std::runtime_error("Shimeji 模型包没有基础动作");
    runtime->cache.emplace(idle->name, LoadActionFrames(runtime->pack, idle->name));
    runtime->Touch(idle->name);
    return runtime;
}

shimeji::Activity CurrentActivity(const std::shared_ptr<PetController>& controller) {
    if (controller->failure.Get()) return shimeji::Activity::failed;
    if (controller->donePendingAck.Get()) return shimeji::Activity::completed;
    if (controller->waitingForInput.Get()) return shimeji::Activity::waiting;
    if (controller->streaming.Get() || controller->agentWorking.Get()) {
        return shimeji::Activity::working;
    }
    if (controller->activityUnknown.Get()) return shimeji::Activity::unknown;
    return shimeji::Activity::idle;
}

std::string ActivityAction(const Runtime& runtime, shimeji::Activity activity) {
    const shimeji::Action* action = shimeji::ActionForActivity(runtime.pack, activity);
    return action ? action->name : std::string{};
}

struct Sprite {
    class Extension;
    std::shared_ptr<Runtime> runtime;
    std::string action;
    bool animate = true;
    bool dragging = false;
    bool operator==(const Sprite&) const = default;
};

class Sprite::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const Sprite& sprite) { Update(node, sprite); }

    void Update(huxerui::ViewNode&, const Sprite& sprite) {
        const bool changed = sprite_ != sprite;
        sprite_ = sprite;
        if (!sprite_.runtime) return;
        if (sprite_.runtime->cache.contains(sprite_.action) &&
            (activeAction_ != sprite_.action || changed)) {
            SetAction(sprite_.action);
        }
        if (changed) InvalidatePaint();
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const ActionFrames* frames = CurrentFrames();
        if (!frames || frames->frames.empty()) return {};
        const bool moving = !frame.reduced_motion && sprite_.animate && !sprite_.dragging && !finished_;
        if (!moving) return {};
        const float delta = static_cast<float>(std::clamp(frame.delta_time, 0.0, 0.1));
        elapsed_ += delta * 60.0F;
        bool changed = false;
        while (!finished_ && elapsed_ >= frames->frames[poseIndex_].pose.durationTicks) {
            elapsed_ -= frames->frames[poseIndex_].pose.durationTicks;
            Advance();
            frames = CurrentFrames();
            if (!frames) return {};
            changed = true;
        }
        const shimeji::Action* action = CurrentAction();
        if (action && action->type == "Move") {
            const shimeji::Pose& pose = frames->frames[poseIndex_].pose;
            pendingMove_.x += pose.velocity.x * delta * 60.0F;
            pendingMove_.y += pose.velocity.y * delta * 60.0F;
            moveElapsed_ += delta;
            if (moveElapsed_ >= 1.0 / 15.0) {
                EmitEvent<ShimejiEvents::Moved>(pendingMove_);
                pendingMove_ = {};
                moveElapsed_ = 0.0F;
            }
        }
        if (changed) InvalidatePaint();
        return {.wake_after = 1.0 / 30.0};
    }

    void PaintAboveContent(const huxerui::ViewNode& node,
                           huxerui::PaintContext& paint) const override {
        const ActionFrames* frames = CurrentFrames();
        if (!frames || frames->frames.empty()) return;
        const Frame& frame = frames->frames[poseIndex_];
        const huxerui::Size intrinsic = frame.image.IntrinsicSize();
        const huxerui::Size available = node.LayoutSize();
        if (intrinsic.width <= 0.0F || intrinsic.height <= 0.0F ||
            available.width <= 0.0F || available.height <= 0.0F) {
            return;
        }
        const float scale = std::min(available.width / intrinsic.width,
                                     available.height / intrinsic.height);
        const float width = intrinsic.width * scale;
        const float height = intrinsic.height * scale;
        const float anchorX = frame.pose.anchor.x == 0.0F
                                  ? intrinsic.width * 0.5F
                                  : frame.pose.anchor.x;
        const float anchorY = frame.pose.anchor.y == 0.0F
                                  ? intrinsic.height
                                  : frame.pose.anchor.y;
        const Rect destination{
            (available.width - width) * 0.5F + (intrinsic.width * 0.5F - anchorX) * scale,
            available.height - anchorY * scale,
            width,
            height,
        };
        const bool flip = frame.pose.velocity.x > 0.0F;
        if (flip) {
            paint.PushTransform({.m11 = -1.0F, .translate_x = available.width});
        }
        paint.DrawImage(frame.image, destination);
        if (flip) paint.PopTransform();
    }

private:
    const shimeji::Action* CurrentAction() const {
        if (!sprite_.runtime) return nullptr;
        return shimeji::FindAction(sprite_.runtime->pack, activeAction_);
    }

    const ActionFrames* CurrentFrames() const {
        if (!sprite_.runtime) return nullptr;
        const auto it = sprite_.runtime->cache.find(activeAction_);
        return it == sprite_.runtime->cache.end() ? nullptr : it->second.get();
    }

    void SetAction(const std::string& name) {
        if (activeAction_ == name) return;
        activeAction_ = name;
        poseIndex_ = 0;
        elapsed_ = 0.0F;
        loops_ = 0;
        finished_ = false;
        pendingMove_ = {};
        InvalidatePaint();
    }

    void Advance() {
        const ActionFrames* frames = CurrentFrames();
        const shimeji::Action* action = CurrentAction();
        if (!frames || !action) return;
        if (poseIndex_ + 1 < frames->frames.size()) {
            ++poseIndex_;
            return;
        }
        if (action->loop) {
            poseIndex_ = 0;
            ++loops_;
            if (loops_ >= 3 && sprite_.runtime) {
                loops_ = 0;
                randomState_ = randomState_ * 6364136223846793005ULL + 1;
                EmitEvent<ShimejiEvents::ActionRequested>(shimeji::NextIdleAction(
                    sprite_.runtime->pack, activeAction_, randomState_));
            }
        } else {
            finished_ = true;
            EmitEvent<ShimejiEvents::ActionRequested>(sprite_.runtime->pack.stateActions.idle);
        }
    }

    Sprite sprite_;
    std::string activeAction_;
    std::size_t poseIndex_ = 0;
    float elapsed_ = 0.0F;
    float moveElapsed_ = 0.0F;
    int loops_ = 0;
    bool finished_ = false;
    Point pendingMove_{};
    std::uint64_t randomState_ = 1;
};

huxerui::View LoadingBadge(std::string text) {
    return huxerui::Text(std::move(text))
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            huxerui::Color::Rgb(224, 242, 254)})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(14.0F, 6.0F)),
              huxerui::Background(huxerui::Color::Rgb(16, 18, 20, 0.86F)),
              huxerui::CornerRadius(12.0F),
              huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Center),
              huxerui::Grow(1.0F));
}

} // namespace

[[huxerui::composable]]
huxerui::View PetShimejiBody(const std::shared_ptr<PetController>& controller) {
    auto runtime = huxerui::UseState<std::shared_ptr<Runtime>>({});
    auto status = huxerui::UseState<std::string>({});
    auto cacheRevision = huxerui::UseState(0);
    auto scheduledAction = huxerui::UseState<std::string>({});
    auto reaction = huxerui::UseState(false);
    auto reactionEpoch = huxerui::UseState(0);
    auto dropping = huxerui::UseState(false);
    auto handledDrop = huxerui::UseState<std::uint64_t>(0);
    const auto tasks = huxerui::UseTaskScope();
    const auto window = huxerui::UseWindow();

    huxerui::Lifecycle([runtime, status, controller, tasks] {
        tasks.Launch([runtime, status, controller]() -> huxerui::Task<void> {
            try {
                auto loaded = co_await huxerui::RunWorker(
                    [selected = controller->shimejiPack.Get()] {
                        return LoadRuntime(selected);
                    });
                runtime = std::move(loaded);
            } catch (const std::exception& e) {
                status = std::format("{}，已显示莲花球", e.what());
            }
        });
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

    const std::uint64_t dropEpoch = controller->dropEpoch.Get();
    huxerui::Lifecycle([dropEpoch, dropping, handledDrop, tasks] {
        if (dropEpoch == 0 || handledDrop.Get() == dropEpoch) return;
        handledDrop = dropEpoch;
        dropping = true;
        tasks.Launch([dropEpoch, dropping, handledDrop]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::milliseconds{600});
            if (handledDrop.Get() == dropEpoch) dropping = false;
        });
    }, dropEpoch);

    const auto current = runtime.Get();
    if (!current) {
        if (!status.Get().empty()) return PetSkinFallback(controller, status.Get());
        return LoadingBadge("加载 Shimeji…");
    }

    const shimeji::Activity activity = CurrentActivity(controller);
    std::string desired = ActivityAction(*current, activity);
    if (reaction.Get()) desired = ActivityAction(*current, shimeji::Activity::completed);
    if (dropping.Get()) desired = current->pack.stateActions.falling;
    if (controller->dragging.Get()) desired = current->pack.stateActions.dragged;
    if (activity == shimeji::Activity::idle && !scheduledAction.Get().empty() &&
        !reaction.Get() && !dropping.Get() && !controller->dragging.Get()) {
        desired = scheduledAction.Get();
    }
    if (!shimeji::FindAction(current->pack, desired)) {
        desired = ActivityAction(*current, shimeji::Activity::idle);
    }

    huxerui::Lifecycle([current, desired, cacheRevision, status, tasks] {
        if (current->cache.contains(desired)) return;
        tasks.Launch([current, desired, cacheRevision, status]() -> huxerui::Task<void> {
            try {
                auto frames = co_await huxerui::RunWorker(
                    [current, desired] { return LoadActionFrames(current->pack, desired); });
                if (current->cache.contains(desired)) co_return;
                current->cache.emplace(desired, std::move(frames));
                current->Touch(desired);
                cacheRevision = cacheRevision.Get() + 1;
                status = "";
            } catch (const std::exception& e) {
                status = e.what();
            }
        });
    }, current, desired);

    static_cast<void>(cacheRevision.Get());
    huxerui::View sprite = huxerui::Stack {}.With(
        Sprite{.runtime = current,
               .action = desired,
               .animate = window.LifecycleState() != huxerui::WindowLifecycleState::Background &&
                          controller->animate.Get(),
               .dragging = controller->dragging.Get()},
        huxerui::Grow(1.0F));
    sprite = std::move(sprite)
        .On<ShimejiEvents::ActionRequested>([scheduledAction](std::string action) {
            scheduledAction = std::move(action);
        })
        .On<ShimejiEvents::Moved>([controller, window](Point delta) {
            const auto position = window.Position();
            const auto work = window.WorkArea();
            if (!position || !work) return;
            const huxerui::Size size = controller->Descriptor().size;
            Point next{
                std::clamp(position->x + delta.x, work->x,
                           work->x + std::max(0.0F, work->width - size.width)),
                std::clamp(position->y + delta.y, work->y,
                           work->y + std::max(0.0F, work->height - size.height)),
            };
            if (window.SetPosition(next) == huxerui::WindowRequestResult::Submitted) {
                controller->saved_position = next;
            }
        });
    if (!status.Get().empty()) {
        return huxerui::Stack {
            std::move(sprite),
            LoadingBadge(status.Get())
                .With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                                     huxerui::VerticalAlignment::End)),
        }.With(huxerui::Grow(1.0F));
    }
    return sprite;
}

} // namespace llmswitch::ui
