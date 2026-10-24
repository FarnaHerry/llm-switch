// pet_live2d.cpp — Live2D 桌宠皮肤（LLMSWITCH_LIVE2D=ON 才编译；禁用时由
// pet_live2d_stub.cpp 顶替，PetLive2DAvailable() 恒 false）。
//
// 模型解析优先级：① dataDir()/live2d/<目录>/<名>.model3.json（用户自放，
// 扫描到的第一个，File 重载）；② 内置示例 Hiyori（构建期从 Lib-Live2D
// preview 暂存进资源包 resources/raw/live2d/，含 model_licenses 许可声明；
// RawResource 重载 + 组合期解析的 map 开启器——UseRawResource 是组合期
// API，不能在加载任务的开启回调里调用，故组合期把 18 个文件全部解析成
// RawAsset 捕获，与 Lib-Live2D preview 的模式一致）。模型与 Cubism SDK
// 不随本仓库分发；缺模型/失败显示占位提示，不静默回退。
//
// 渲染：HuxerUI::Live2D 的 ModelView 经 ExternalTexture 发布帧——透明
// Utility 表面支持的外部纹理路径。模型自带自动眨眼/呼吸；路由 streaming
// 时 playback_rate 提到 1.15（「思考变快」）；Ready 后循环 Idle 组；点击
// 命中区播 TapBody。
#include <huxerui/huxerui.h>
#include <huxerui/live2d.h>

#include <cstdio>  // stderr（std 模块不导出 stdout/stderr 宏）
#include <filesystem>
#include <map>
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

// 内置示例（Lib-Live2D preview 的 Hiyori，钉版文件清单——Lib-Live2D
// 钉 commit，文件集固定）。key 相对资源包 raw 根，与开启器收到的路径同构。
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

// 组合期解析出的内置示例资源（key → RawAsset）。解析失败（资源缺失等）
// 为空，加载任务据此跳过内置回退。
using BundledResources = std::map<std::string, huxerui::RawAsset>;

// 扫描 dataDir()/live2d 下第一个 *.model3.json（文件 IO 放 worker 线程）。
// 注意 extension() 只返回最后一个后缀（".json"），必须按完整文件名后缀判。
std::optional<std::string> FindModelEntry() {
    std::error_code ec;
    const fs::path root = cfg::dataDir() / "live2d";
    if (!fs::exists(root, ec)) return std::nullopt;
    // 深度限两层：<live2d>/<模型目录>/<名>.model3.json。
    for (const auto& dir : fs::directory_iterator(root, ec)) {
        if (!dir.is_directory()) continue;
        for (const auto& file : fs::directory_iterator(dir.path(), ec)) {
            if (file.is_regular_file(ec) &&
                file.path().filename().string().ends_with(".model3.json")) {
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

    // 内置示例在组合期解析（UseRawResource 是组合期 API，开启回调不能调）。
    // RawResource 的 key 带 raw/ 前缀（生成标识符 app::raw::<key>）；开启器
    // 收到的路径则不带前缀（包相对）。任一文件缺失则整体放弃内置（用户仍可
    // 自放模型）。
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

    // 首次挂载触发一次加载（重组不重载；窗口退役随作用域取消任务）。
    huxerui::Lifecycle([tasks, asset, controller2d, status, bundled] {
        tasks.Launch([asset, controller2d, status,
                      bundled]() -> huxerui::Task<void> {
            // ① 用户自放模型（dataDir，File 重载）。
            const std::optional<std::string> entry =
                co_await huxerui::RunWorker([] { return FindModelEntry(); });
            if (entry.has_value()) {
                auto loaded = co_await live2d::LoadModelAsync(huxerui::File(*entry));
                if (loaded.Succeeded()) {
                    controller2d = live2d::ModelController{};
                    asset = std::move(loaded).Value();
                    co_return;
                }
                status = std::string("模型加载失败：") + loaded.Error().message;
                co_return;
            }
            // ② 内置示例（RawResource 重载 + 捕获的解析结果；entry 同样带
            // raw/ 前缀，开启器收到的依赖路径不带）。
            if (bundled) {
                auto loaded = co_await live2d::LoadModelAsync(
                    huxerui::RawResource{"app",
                                         std::string("raw/") + kBundledEntryKey},
                    [bundled](std::string path)
                        -> huxerui::Task<
                            huxerui::IoResult<huxerui::AsyncInputStream>> {
                        const auto it = bundled->find(path);
                        if (it == bundled->end()) {
                            co_return huxerui::IoResult<
                                huxerui::AsyncInputStream>::
                                Failure({huxerui::IoErrorCode::NotFound,
                                         "内置示例资源缺失：" + path});
                        }
                        co_return huxerui::IoResult<
                            huxerui::AsyncInputStream>::
                            Success(co_await it->second.OpenReadAsync());
                    });
                if (loaded.Succeeded()) {
                    controller2d = live2d::ModelController{};
                    asset = std::move(loaded).Value();
                    co_return;
                }
                status = std::string("内置示例加载失败：") + loaded.Error().message;
                co_return;
            }
            status = std::string(
                "未找到模型：把 Live2D 模型目录放到 数据目录/live2d/ 下");
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
                                                 {.priority = 1, .loop = true}));
        }
    };
    const auto play_tap = [current, command, play_idle] {
        if (const auto group = FindMotionGroup(current, "TapBody")) {
            static_cast<void>(command.PlayMotion(group->name, 0, {.priority = 3, .force = true}));
        } else {
            play_idle();
        }
    };

    // 诊断（临时）：Live2D 栈在 Linux 透明表面属首次验证，Ready/失败
    // 都打到 stderr 并在底部徽标显示状态，便于定位「纯透明」类问题。
    huxerui::View body =
        live2d::ModelView(current,
                          {
                              .controller = command,
                              .fit = huxerui::ImageFit::Contain,
                              // streaming 时播放速率 +15%：与莲花球
                              // 「呼吸加速」同语义的思考态。
                              .playback_rate =
                                  controller->streaming.Get() ? 1.15F : 1.0F,
                          })
            .On<live2d::ModelEvents::Ready>([play_idle] {
                std::fprintf(stderr, "live2d: Ready\n");
                play_idle();
            })
            .On<live2d::ModelEvents::HitAreaTapped>([play_tap](std::string) { play_tap(); })
            .On<live2d::ModelEvents::PlaybackFailed>(
                [status](live2d::PlaybackError error) {
                    std::fprintf(stderr, "live2d: PlaybackFailed: %s\n",
                                 error.message.c_str());
                    status = std::string("渲染失败：") + error.message;
                })
            .With(huxerui::Grow(1.0F));
    if (!status.Get().empty()) {
        body = huxerui::Stack {
            std::move(body),
            huxerui::Text(status.Get())
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                          huxerui::Color::Rgb(240, 115, 108)})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 4.0F)),
                      huxerui::Background(huxerui::Color::Rgb(16, 18, 20, 0.86F)),
                      huxerui::CornerRadius(9.0F),
                      huxerui::Align(huxerui::HorizontalAlignment::Center,
                                     huxerui::VerticalAlignment::End),
                      huxerui::Padding(huxerui::EdgeInsets{.bottom = 6.0F})),
        }.With(huxerui::Grow(1.0F));
    }
    return body;
}

} // namespace llmswitch::ui
