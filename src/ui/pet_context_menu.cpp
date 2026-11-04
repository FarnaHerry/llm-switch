#include <huxerui/huxerui.h>

#include <functional>
#include <utility>
#include <vector>

#include "app.h"
#include "pet.h"
#include "ui.h"

namespace llmswitch::ui {

void PetController::CloseContextMenu() {
    ++contextMenuGeneration;
    auto closing = std::exchange(contextMenu, std::nullopt);
    if (closing) closing->Close();
}

void PetController::ShowContextMenu(
    const huxerui::ApplicationHandle& application, huxerui::Point anchor) {
    if (!pet || !pet->IsOpen()) return;
    if (!application.SupportsMultipleWindows()) {
        throw std::runtime_error("当前平台不支持独立桌宠菜单窗口");
    }
    if (contextMenu && contextMenu->IsOpen()) {
        contextMenu->Activate();
        return;
    }
    CloseContextMenu();
    contextMenu = application.OpenWindow(
        [] { return PetContextMenuContent(); },
        PetContextMenuOptions(pet->Position(), anchor, pet->WorkArea()), {.owner = pet});
    contextMenu->Activate();
}

[[huxerui::composable]] huxerui::View PetContextMenuContent() {
    const auto application = huxerui::UseApplication();
    const auto controller = huxerui::UseService<PetController>();
    const auto activation = huxerui::UseService<MainWindowController>();
    const auto nav = huxerui::UseService<MainNavTarget>();
    const bool systemDark = huxerui::UseSystemThemeBrightness() == huxerui::ThemeBrightness::Dark;
    const bool dark = controller->themeMode.Get() == 1 ||
                      (controller->themeMode.Get() == 0 && systemDark);
    const auto spec = dark ? AppDarkThemeSpec() : AppLightThemeSpec();
    return huxerui::Theme(AppThemeDefinition(spec), huxerui::Scope(
        [application, controller, activation, nav, spec]() -> huxerui::View {
            const auto window = huxerui::UseWindow();
            const auto menu = huxerui::UseMenu();
            const auto tasks = huxerui::UseApplicationTaskScope();
            const auto generation = controller->contextMenuGeneration;
            auto activated = huxerui::UseState(
                window.LifecycleState() == huxerui::WindowLifecycleState::Active);
            const auto dismiss = [controller, tasks, generation] {
                tasks.Post([controller, generation] {
                    if (controller->contextMenuGeneration == generation) controller->CloseContextMenu();
                });
            };
            // 菜单选择可能关闭当前窗口或重建桌宠，统一等指针派发结束后执行。
            const auto item = [controller, tasks, generation](
                std::string label, std::function<void()> action) {
                return huxerui::MenuItem(std::move(label), [controller, tasks, generation, action] {
                    if (controller->contextMenuGeneration != generation) return;
                    tasks.Post([controller, generation, action] {
                        // 原生菜单可能先请求 dismiss；已经选中的操作仍须执行。
                        if (controller->contextMenuGeneration == generation) controller->CloseContextMenu();
                        action();
                    });
                });
            };
            std::vector<huxerui::MenuEntry> entries{
                item("信息卡", [controller, application] { controller->ToggleCard(application); })
                    .Checked(controller->card.has_value()),
                item("胶囊模式", [controller, application] {
                    const bool next = !controller->compact.Get();
                    try {
                        providerStore().setPetCompact(next);
                        controller->SetCompact(next, application);
                    } catch (const std::exception&) {
                    }
                }).Checked(controller->compact.Get()),
                item("置顶", [controller] {
                    const bool next = !controller->topmost.Get();
                    providerStore().setPetTopmost(next);
                    controller->SetTopmost(next);
                }).Checked(controller->topmost.Get()),
                item("本地路由", [nav] {
                    try {
                        if (routerInstance().running()) {
                            routerInstance().stop();
                            providerStore().setRouterEnabled(false);
                        } else {
                            routerInstance().start(providerStore().config().routerPort);
                            providerStore().setRouterEnabled(true);
                        }
                    } catch (const std::exception&) {
                    }
                    if (const auto revision = nav->RevisionState()) *revision = revision->Get() + 1;
                }).Checked(routerInstance().running()),
                item("打开主窗口", [activation] { activation->Activate(); }),
                item("退出", [application] { application.Quit(); }),
            };
            huxerui::Lifecycle([controller, window, generation] {
                controller->contextMenu = window;
                return [controller, generation] {
                    if (controller->contextMenuGeneration == generation) controller->contextMenu.reset();
                };
            });
            window.OnCloseRequest([controller, generation, dismiss] {
                if (controller->contextMenuGeneration != generation) return false;
                dismiss();
                return true;
            });
            window.OnLifecycleChanged([activated, dismiss](huxerui::WindowLifecycleState state) {
                if (state == huxerui::WindowLifecycleState::Active) activated = true;
                else if (activated.Get()) dismiss();
            });
            huxerui::Lifecycle([menu, entries, dismiss] {
                menu.ShowAt({8.0F, 8.0F}, entries, {
                    .gap = 0.0F,
                    .viewport_margin = 8.0F,
                    .width = 240.0F,
                    .on_dismiss_request = dismiss,
                });
            });
            return huxerui::Column{}.With(
                huxerui::Grow(1.0F), huxerui::Background(spec.colors.background));
        }));
}

} // namespace llmswitch::ui
