// pet_live2d.cpp — Live2D 桌宠皮肤（LLMSWITCH_LIVE2D=ON 才编译；禁用时由
// pet_live2d_stub.cpp 顶替，PetLive2DAvailable() 恒 false）。
//
// 资产约定：模型包放在 dataDir()/live2d/<目录>/<名>.model3.json，取目录
// 扫描到的第一个（官方 Cubism 示例角色 Hiyori/Haru/Mao 的目录结构直接可
// 用）。模型不随本仓库分发（Cubism SDK 与模型资产各有自己的许可），由用户
// 自行放入；缺模型或加载失败时显示占位提示（不静默回退成莲花球，避免
// 「开关打开了却没有效果」的困惑）。
//
// 渲染：HuxerUI::Live2D 的 ModelView 经 ExternalTexture 发布帧——透明
// Utility 表面支持的外部纹理路径（desktop_pet README 同款机制）。模型自带
// 自动眨眼/呼吸；路由 streaming 时 playback_rate 提到 1.15（「思考变快」，
// 与莲花球的呼吸加速同语义）；Ready 后循环播放 Idle 组；点击命中区播
// TapBody（或首个动作组）。
#include <huxerui/huxerui.h>
#include <huxerui/live2d.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pet.h"
#include "theme_colors.h"

import llmswitch.config;

namespace llmswitch::ui {
namespace {

namespace fs = std::filesystem;
namespace live2d = huxerui::live2d;

// 扫描 dataDir()/live2d 下第一个 *.model3.json（浅层目录即可；文件 IO
// 放 worker 线程执行）。找不到返回 nullopt。
std::optional<std::string> FindModelEntry() {
    std::error_code ec;
    const fs::path root = cfg::dataDir() / "live2d";
    if (!fs::exists(root, ec)) return std::nullopt;
    // 深度限两层：<live2d>/<模型目录>/<名>.model3.json。
    for (const auto& dir : fs::directory_iterator(root, ec)) {
        if (!dir.is_directory()) continue;
        for (const auto& file : fs::directory_iterator(dir.path(), ec)) {
            if (file.is_regular_file(ec) && file.path().extension() == ".model3.json") {
                return file.path().string();
            }
        }
    }
    return std::nullopt;
}

// 在模型动作组里找首选组：Exact 优先，否则首个有动作的组。
std::optional<live2d::MotionGroup> FindMotionGroup(
    const live2d::ModelAsset& asset, const std::string& preferred) {
    std::optional<live2d::MotionGroup> first;
    for (const auto& group : asset.Info().motion_groups) {
        if (group.count <= 0) continue;
        if (group.name == preferred) return group;
        if (!first.has_value()) first = group;
    }
    return first;
}

} // namespace

bool PetLive2DAvailable() { return true; }

[[huxerui::composable]]
huxerui::View PetLive2DBody(const std::shared_ptr<PetController>& controller) {
    auto asset = huxerui::UseState(live2d::ModelAsset{});
    auto controller2d = huxerui::UseState(live2d::ModelController{});
    auto status = huxerui::UseState<std::string>({});
    const auto tasks = huxerui::UseTaskScope();

    // 首次挂载触发一次加载（重组不重载；窗口退役随作用域取消任务）。
    huxerui::Lifecycle([tasks, asset, controller2d, status] {
        tasks.Launch([asset, controller2d, status]() -> huxerui::Task<void> {
            const std::optional<std::string> entry =
                co_await huxerui::RunWorker([] { return FindModelEntry(); });
            if (!entry.has_value()) {
                status = std::string("未找到模型：把 Live2D 模型目录放到 数据目录/live2d/ 下");
                co_return;
            }
            auto loaded = co_await live2d::LoadModelAsync(huxerui::File(*entry));
            if (loaded.Succeeded()) {
                controller2d = live2d::ModelController{};
                asset = std::move(loaded).Value();
            } else {
                status = std::string("模型加载失败：") + loaded.Error().message;
            }
        });
    });

    const live2d::ModelAsset current = asset.Get();
    if (!current.HasValue()) {
        // 占位：深色玻璃药丸（与莲花球浮标同族样式），显示加载态或错误。
        return huxerui::Text(status.Get().empty() ? "加载模型…" : status.Get())
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      huxerui::Color::Rgb(224, 242, 254)})
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(14.0F, 6.0F)),
                  huxerui::Background(huxerui::Color::Rgb(16, 18, 20, 0.86F)),
                  huxerui::CornerRadius(12.0F),
                  huxerui::Border{huxerui::Color::Rgb(224, 242, 254, 0.14F)},
                  huxerui::Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center),
                  huxerui::Grow(1.0F));
    }

    const live2d::ModelController command = controller2d.Get();
    const auto play_idle = [current, command] {
        if (const auto group = FindMotionGroup(current, "Idle")) {
            static_cast<void>(command.PlayMotion(group->name, 0,
                                                 {.priority = 1, .looping = true}));
        }
    };
    const auto play_tap = [current, command] {
        if (const auto group = FindMotionGroup(current, "TapBody")) {
            static_cast<void>(command.PlayMotion(group->name, 0, {.priority = 3, .force = true}));
        } else {
            play_idle();
        }
    };

    return live2d::ModelView(current,
                             {
                                 .controller = command,
                                 .fit = huxerui::ImageFit::Contain,
                                 // streaming 时播放速率 +15%：与莲花球
                                 // 「呼吸加速」同语义的思考态。
                                 .playback_rate = controller->streaming.Get() ? 1.15F : 1.0F,
                             })
        .On<live2d::ModelEvents::Ready>([play_idle] { play_idle(); })
        .On<live2d::ModelEvents::HitAreaTapped>([play_tap](std::string) { play_tap(); })
        .On<live2d::ModelEvents::PlaybackFailed>(
            [status](live2d::PlaybackError error) {
                status = std::string("渲染失败：") + error.message;
            })
        .With(huxerui::Grow(1.0F));
}

} // namespace llmswitch::ui
