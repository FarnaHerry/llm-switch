// router_page.cpp — 本地路由页 + 进程级 LocalRouter 单例（routerInstance()）。
//
// 页面结构（PageScaffold「本地路由」）：
//   状态卡：运行徽章（运行中/已停止）、启用开关（开=start(port)/关=stop()，
//     立即 setRouterEnabled 落盘）、端口输入框+「应用」（运行中改端口 =
//     stop→start 新端口，随后 setRouterPort 落盘；非法端口 toast）、
//     故障转移开关（setFailoverEnabled + setRouterFailover 落盘）。
//   Agent 代理卡：遍历 models::toolRegistry() 提供逐工具开关；修改立即同步到
//     LocalRouter 并通过 ProviderStore 持久化，运行中无需重启。
//   接入地址卡：只展示已启用工具的
//     http://127.0.0.1:<port>/<toolId>（等宽）。
//   最近请求卡：recentLogs(50)（新的在前）：时间/工具/供应商/方法+路径
//     （截断）/状态码（2xx 绿 4xx 黄 5xx 与失败红）/耗时/token（-1 显 "—"）。
//     页面可见期间每 2s 自动刷新（Lifecycle + TaskScope 轮询，卸载自动取消；
//     State 只在 UI 线程写）。
// start/stop 是快操作（后台线程起停 httplib），UI 线程直接调用；失败
// try/catch toast 中文错误。routerEnabled=true 的开机自启由 app.cpp 首组合
// 负责（见 ui.h 标记段注释，任务G 接线）。
#include <huxerui/huxerui.h>

#include <charconv>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ui.h"

import llmswitch.models;
import llmswitch.router;
import llmswitch.store;

namespace llmswitch::ui {

router::LocalRouter& routerInstance() {
    // resolver 读 providerStore() 的组快照（按值拷贝）；空组原样返回，由
    // router 判 current。未注册工具 id / 读取异常 → nullopt（router 走无组
    // 路径）。store 无内部锁、UI 线程独占是全局契约，resolver 在 router 的
    // 后台线程被调时只做这份只读拷贝（与 UI 写操作并发窗口极小，已知的
    // 设计取舍，见 ui.h 注释）。
    static router::LocalRouter instance{
        [](std::string_view tool) -> std::optional<models::ProviderGroup> {
            try {
                return providerStore().group(std::string(tool));
            } catch (...) {
                return std::nullopt;
            }
        }};
    // 首次访问按持久化配置设置故障转移与逐工具代理开关（LocalRouter 不可
    // 移动，只能就地构造后补设）。
    static const bool settingsInit = [] {
        const auto& config = providerStore().config();
        instance.setFailoverEnabled(config.routerFailover);
        for (const auto& spec : models::toolRegistry()) {
            const bool enabled =
                std::ranges::find(config.routerTools, spec.id) !=
                config.routerTools.end();
            instance.setToolEnabled(spec.id, enabled);
        }
        return true;
    }();
    (void)settingsInit;
    return instance;
}

namespace {

void ReplaceLogList(const huxerui::StateList<router::RequestLog>& destination,
                    std::vector<router::RequestLog> values) {
    const std::size_t shared = std::min(destination.Size(), values.size());
    for (std::size_t i = 0; i < shared; ++i) {
        destination.Set(i, std::move(values[i]));
    }
    while (destination.Size() > values.size()) {
        destination.PopBack();
    }
    for (std::size_t i = shared; i < values.size(); ++i) {
        destination.PushBack(std::move(values[i]));
    }
}

std::string FormatClock(std::int64_t ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    if (::localtime_s(&tm, &t) != 0) return "--:--:--";  // MSVC 安全版（参数对调）
#else
    if (::localtime_r(&t, &tm) == nullptr) return "--:--:--";
#endif
    return std::format("{:02d}:{:02d}:{:02d}", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string Truncate(std::string s, std::size_t max) {
    if (s.size() > max) {
        s.resize(max - 1);
        s += "…";
    }
    return s;
}

std::string FormatTokens(std::int64_t prompt, std::int64_t completion) {
    auto one = [](std::int64_t v) {
        return v < 0 ? std::string("—") : std::to_string(v);
    };
    return one(prompt) + "/" + one(completion);
}

huxerui::Color StatusColor(int status, const huxerui::ThemeSpec& theme) {
    if (status >= 200 && status < 300) return huxerui::Color::Rgb(22, 163, 74);
    if (status >= 400 && status < 500) return huxerui::Color::Rgb(202, 138, 4);
    if (status == 0 || status >= 500) return theme.colors.error;  // 0 = 上游失败
    return theme.colors.on_surface_variant;
}

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

[[huxerui::composable]] huxerui::View HintText(const std::string& text) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(text).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption),
        theme.colors.on_surface_variant});
}

// 设置行：左标签+提示，右侧控件（对齐 settings_page 的 SettingRow 范式）。
[[huxerui::composable]] huxerui::View SettingRow(const std::string& label,
                                                 const std::string& hint,
                                                 huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Column {
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
            hint.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{HintText(hint)},
        }.With(huxerui::Spacing(2.0F)),
        huxerui::Spacer(),
        std::move(control),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 接入地址行：工具名 + 等宽地址。
[[huxerui::composable]] huxerui::View EndpointRow(std::string name,
                                                  std::string url) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Text(name)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kChip),
                                      theme.colors.on_surface})
            .With(huxerui::Frame{.width = 110.0F}),
        huxerui::Text(url).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kChip), theme.colors.on_surface}),
    }.With(huxerui::Spacing(8.0F));
}

// 请求日志行：状态码单独着色，方法+路径截断并弹性占宽。
[[huxerui::composable]] huxerui::View LogRow(router::RequestLog log) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::Font mono = huxerui::Font::Monospace(font_size::kCaption);
    return huxerui::Row {
        huxerui::Text(FormatClock(log.tsMillis))
            .Style(huxerui::TextStyle{mono, theme.colors.on_surface_variant})
            .With(huxerui::Frame{.width = 64.0F}),
        huxerui::Text(std::string(ToolName(log.tool)))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface})
            .With(huxerui::Frame{.width = 96.0F}),
        huxerui::Text(Truncate(log.providerName, 14))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant})
            .With(huxerui::Frame{.width = 110.0F}),
        huxerui::Text(Truncate(log.method + " " + log.path, 56))
            .Style(huxerui::TextStyle{mono, theme.colors.on_surface})
            .With(huxerui::Grow(1.0F)),
        huxerui::Text(log.status == 0 ? "失败" : std::to_string(log.status))
            .Style(huxerui::TextStyle{mono, StatusColor(log.status, theme)})
            .With(huxerui::Frame{.width = 40.0F}),
        huxerui::Text(std::format("{}ms", log.latencyMs))
            .Style(huxerui::TextStyle{mono, theme.colors.on_surface_variant})
            .With(huxerui::Frame{.width = 60.0F}),
        huxerui::Text(FormatTokens(log.promptTokens, log.completionTokens))
            .Style(huxerui::TextStyle{mono, theme.colors.on_surface_variant})
            .With(huxerui::Frame{.width = 90.0F}),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace

[[huxerui::composable]] huxerui::View RouterPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    auto running = huxerui::UseState(routerInstance().running());
    auto failover =
        huxerui::UseState(providerStore().config().routerFailover);
    auto enabledTools =
        huxerui::UseState(providerStore().config().routerTools);
    auto portField = huxerui::UseState(huxerui::TextEditingValue{
        std::to_string(providerStore().config().routerPort)});
    auto logs = huxerui::UseStateList<router::RequestLog>();

    // 首组合加载 + 可见期间每 2s 刷新日志与运行状态（TaskScope 随页面卸载
    // 自动取消轮询；State 只在 UI 线程写）。
    huxerui::Lifecycle(
        [=] {
            ReplaceLogList(logs, routerInstance().recentLogs(50));
            running = routerInstance().running();
            tasks.Launch([=]() -> huxerui::Task<void> {
                while (true) {
                    co_await huxerui::Delay(std::chrono::seconds{2});
                    ReplaceLogList(logs, routerInstance().recentLogs(50));
                    running = routerInstance().running();
                }
            });
            return [] {};
        },
        0);

    // 启用开关：start/stop 为快操作，UI 线程直接做；立即落盘 routerEnabled。
    auto setRunning = [=](bool on) {
        try {
            if (on) {
                routerInstance().start(providerStore().config().routerPort);
            } else {
                routerInstance().stop();
            }
            providerStore().setRouterEnabled(on);
            running = routerInstance().running();
            toast.Show(on ? "路由已启动" : "路由已停止");
        } catch (const std::exception& e) {
            toast.Show(e.what());
            running = routerInstance().running();
        }
    };

    // 应用端口：非法输入 toast；运行中先停再以新端口重启，成功后落盘。
    auto applyPort = [=] {
        const std::string& text = portField.Get().text;
        const char* last = text.data() + text.size();
        int port = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), last, port);
        if (ec != std::errc{} || ptr != last || port < 1 || port > 65535) {
            toast.Show("端口无效：请输入 1-65535 的整数");
            return;
        }
        try {
            if (routerInstance().running()) {
                routerInstance().stop();
                routerInstance().start(port);
            }
            providerStore().setRouterPort(port);
            running = routerInstance().running();
            toast.Show(std::format("端口已更新为 {}", port));
        } catch (const std::exception& e) {
            toast.Show(e.what());
            running = routerInstance().running();
        }
    };

    auto setFailover = [=](bool on) {
        routerInstance().setFailoverEnabled(on);
        providerStore().setRouterFailover(on);
        failover = on;
    };

    // 单 Agent 开关：先持久化，再更新线程安全的运行时路由表；运行中的服务
    // 无需重启。失败时保持 UI 与运行时原状态。
    auto setToolEnabled = [=](std::string tool, bool on) {
        try {
            providerStore().setRouterToolEnabled(tool, on);
            routerInstance().setToolEnabled(tool, on);
            enabledTools = providerStore().config().routerTools;
            toast.Show(std::format("{} 代理已{}", ToolName(tool),
                                   on ? "启用" : "关闭"));
        } catch (const std::exception& e) {
            toast.Show(e.what());
            enabledTools = providerStore().config().routerTools;
        }
    };

    // 展示端口：运行中取实际绑定端口，否则取配置值（与启动后一致）。
    const int displayPort = running.Get() ? routerInstance().port()
                                          : providerStore().config().routerPort;

    std::vector<huxerui::View> endpoints;
    std::vector<huxerui::View> toolToggles;
    for (const auto& spec : models::toolRegistry()) {
        const bool enabled =
            std::ranges::find(enabledTools.Get(), spec.id) !=
            enabledTools.Get().end();
        const std::string toolId(spec.id);
        toolToggles.push_back(
            SettingRow(
                std::string(spec.displayName),
                std::format("代理 /{} 路径的请求", spec.id),
                huxerui::View{huxerui::Switch(enabled).OnChanged(
                    [setToolEnabled, toolId](bool on) {
                        setToolEnabled(toolId, on);
                    })})
                .Key(toolId));
        if (enabled) {
            endpoints.push_back(
                EndpointRow(std::string(spec.displayName),
                            std::format("http://127.0.0.1:{}/{}", displayPort,
                                        spec.id)));
        }
    }

    const std::size_t logCount = logs.Size();
    const auto buildLogRow = [logs](std::size_t index) {
        const auto& log = logs.At(index);
        return LogRow(log)
            .Key(std::format("{}:{}:{}:{}", log.tsMillis, log.tool,
                              log.method, log.path));
    };

    const huxerui::View badge =
        running.Get()
            ? huxerui::View{huxerui::Text("运行中").Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  huxerui::Color::White()})}
                  .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 2.0F)),
                        huxerui::Background(huxerui::Color::Rgb(22, 163, 74)),
                        huxerui::CornerRadius(8.0F))
            : huxerui::View{huxerui::Text("已停止").Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})}
                  .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 2.0F)),
                        huxerui::Background(theme.colors.surface_container_highest),
                        huxerui::CornerRadius(8.0F));

    return PageScaffold(
        "本地路由",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    SectionTitle("状态"),
                    huxerui::Row {
                        huxerui::Text("运行状态")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kBody),
                                theme.colors.on_surface}),
                        huxerui::Spacer(),
                        badge,
                    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                    SettingRow("启用本地路由",
                               "把各 CLI 的 base URL 指向本机路由，转发到当前激活供应商",
                               huxerui::View{huxerui::Switch(running.Get())
                                                 .OnChanged([=](bool on) {
                                                     setRunning(on);
                                                 })}),
                    SettingRow(
                        "监听端口", "运行中修改端口会自动重启路由",
                        huxerui::View{huxerui::Row {
                            huxerui::TextField(portField.Get())
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged([=](const huxerui::TextEditingValue& v) {
                                    portField = v;
                                })
                                .With(huxerui::Frame{.width = 110.0F}),
                            huxerui::Button("应用").OnClick([=] { applyPort(); }),
                        }.With(huxerui::Spacing(8.0F))}),
                    SettingRow("故障转移",
                               "上游 429/5xx/连接失败时按组内顺序试下一个供应商",
                               huxerui::View{huxerui::Switch(failover.Get())
                                                 .OnChanged([=](bool on) {
                                                     setFailover(on);
                                                 })}),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("Agent 代理"),
                    HintText("选择允许通过本地端口转发的 Agent；运行中修改即时生效。"),
                    huxerui::Column(std::move(toolToggles))
                        .With(huxerui::Spacing(8.0F),
                              huxerui::CrossAlign(
                                  huxerui::CrossAxisAlignment::Stretch)),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("接入地址"),
                    HintText("把各工具的 base URL 指到对应地址，例如 Claude Code "
                             "设置环境变量 ANTHROPIC_BASE_URL 为下方地址。"),
                    endpoints.empty()
                        ? huxerui::View{HintText("尚未启用任何 Agent 代理")}
                        : huxerui::View{
                              huxerui::Column(std::move(endpoints))
                                  .With(huxerui::Spacing(6.0F),
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::Stretch))},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("最近请求"),
                    logCount == 0
                        ? huxerui::View{HintText("暂无请求")}
                        : huxerui::View{huxerui::VirtualList(logCount, buildLogRow)
                                            .ItemExtent(32.0F)
                                            .With(huxerui::Frame{.height = 320.0F})},
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace llmswitch::ui
