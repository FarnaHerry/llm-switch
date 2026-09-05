// mcp_page.cpp — MCP 服务器管理页：统一清单（mcp::McpStore，SSOT 在
// dataDir()/mcp.json）的卡片列表。每卡：名称 + 类型徽章（stdio/sse/http）+
// 摘要行（stdio 显示 command+args；sse/http 显示 url）+ 每工具启用开关
// （claude-code / codex / opencode 三个 Switch → setEnabled，不支持的工具
// 由 store 抛中文错 toast 出来）+ 编辑/删除（删除走内置确认框）。
// 顶部：新增 MCP 服务器（slug 校验 + 类型 SegmentedButton 条件表单）+
// 从工具收编（importFromTool 回读 live 配置）。
//
// 数据流对齐 providers 页：store 为页面级函数内 static（UI 线程独占；live
// 文件读写是微秒级本地 IO，不经任务线程）；写操作后 revision+1 驱动重读。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui.h"

import llmswitch.mcp;
import llmswitch.models;

namespace llmswitch::ui {
namespace {

// 页面级 store 持有点（对齐 common.cpp providerStore() 的模式；仅本页使用，
// 不放公共头）。首次访问即加载，进程内唯一实例，UI 线程独占。
mcp::McpStore& mcpStore() {
    static mcp::McpStore store = mcp::McpStore::load();
    return store;
}

// 可启停的工具集合（claude / pi 由 store 层报「暂不支持」，不进开关行）。
constexpr std::string_view kMcpTools[] = {"claude-code", "codex", "opencode"};

// 收编来源（codex 的 TOML 回读 store 层不做、返回 0，不列）。
constexpr std::string_view kImportTools[] = {"claude-code", "opencode"};

constexpr std::string_view kTypeNames[] = {"stdio", "sse", "http"};

// slug 校验：字母数字连字符。
bool IsValidSlug(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    }
    return true;
}

std::string JoinArgs(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
        if (!out.empty()) out += ' ';
        out += a;
    }
    return out;
}

std::vector<std::string> SplitArgs(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

// 多行 KEY=VALUE（env）/ Key: Value（headers）解析；无分隔符或空键的行跳过。
std::map<std::string, std::string> ParseLines(const std::string& text, char delim) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto pos = line.find(delim);
        if (pos == std::string::npos || pos == 0) continue;
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        const auto trim = [](std::string& s) {
            const auto b = s.find_first_not_of(" \t");
            const auto e = s.find_last_not_of(" \t");
            s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
        };
        trim(key);
        trim(value);
        if (!key.empty()) out[key] = value;
    }
    return out;
}

std::string JoinLines(const std::map<std::string, std::string>& kv, char delim) {
    std::string out;
    for (const auto& [k, v] : kv) {
        out += k;
        out += delim == '=' ? "=" : ": ";
        out += v;
        out += '\n';
    }
    return out;
}

std::string TypeOf(const mcp::McpServer& srv) {
    return srv.type.empty() ? "stdio" : srv.type;
}

// 摘要行：stdio 显示 command+args 拼接；sse/http 显示 url。
std::string SummaryOf(const mcp::McpServer& srv) {
    if (TypeOf(srv) == "stdio") {
        const std::string args = JoinArgs(srv.args);
        return args.empty() ? srv.command : srv.command + " " + args;
    }
    return srv.url;
}

// 新增/编辑共用表单字段（State 是可拷贝句柄，归打开弹窗的组合作用域所有）。
struct McpFormStates {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<int> type;  // 0=stdio 1=sse 2=http
    huxerui::State<huxerui::TextEditingValue> command;
    huxerui::State<huxerui::TextEditingValue> args;
    huxerui::State<huxerui::TextEditingValue> env;
    huxerui::State<huxerui::TextEditingValue> url;
    huxerui::State<huxerui::TextEditingValue> headers;
};

// 注意：hcg 限制 UseState 只能在 composable 体内调，无法抽「构造表单 States」
// 辅助函数；各持有点（卡片作用域 / 页面作用域）就地聚合初始化。

void FillMcpForm(const McpFormStates& fs, const mcp::McpServer& srv) {
    fs.name = huxerui::TextEditingValue{srv.name};
    const std::string t = TypeOf(srv);
    fs.type = t == "sse" ? 1 : t == "http" ? 2 : 0;
    fs.command = huxerui::TextEditingValue{srv.command};
    fs.args = huxerui::TextEditingValue{JoinArgs(srv.args)};
    fs.env = huxerui::TextEditingValue{JoinLines(srv.env, '=')};
    fs.url = huxerui::TextEditingValue{srv.url};
    fs.headers = huxerui::TextEditingValue{JoinLines(srv.headers, ':')};
}

// 新增/编辑弹窗内容（composable）。editingName 为空 = 新增。校验：名称 slug
// （字母数字连字符）；stdio 命令必填；sse/http URL 必填。编辑时保留原
// enabledTools；改名 = 先 remove 旧键（连带清 live）再 upsert 新键回写。
[[huxerui::composable]] huxerui::View McpFormContent(
    std::string title, McpFormStates fs, std::string editingName,
    huxerui::DialogContext ctx, huxerui::ToastHandle toast,
    huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool isStdio = fs.type.Get() == 0;
    std::vector<huxerui::View> fields;
    fields.push_back(huxerui::Text(title, huxerui::TextRole::Title));
    fields.push_back(huxerui::TextField(fs.name.Get())
        .Label("名称（字母 / 数字 / 连字符）")
        .Placeholder("my-server")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.name = v; }));
    fields.push_back(huxerui::Text("类型")
        .Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}));
    fields.push_back(
        huxerui::SegmentedButton({"stdio", "sse", "http"},
                                 static_cast<std::size_t>(fs.type.Get()))
            .OnChanged([fs](std::size_t index) {
                fs.type = static_cast<int>(index);
            }));
    if (isStdio) {
        fields.push_back(huxerui::TextField(fs.command.Get())
            .Label("命令")
            .Placeholder("npx")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.command = v; }));
        fields.push_back(huxerui::TextField(fs.args.Get())
            .Label("参数（空格分隔，可选）")
            .Placeholder("-y @modelcontextprotocol/server-filesystem")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.args = v; }));
        fields.push_back(
            huxerui::Text("环境变量（每行 KEY=VALUE，可选）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
        fields.push_back(huxerui::TextField(fs.env.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(3, 8))
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.env = v; }));
    } else {
        fields.push_back(huxerui::TextField(fs.url.Get())
            .Label("URL")
            .Placeholder("https://...")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.url = v; }));
        fields.push_back(
            huxerui::Text("请求头（每行 Key: Value，可选）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
        fields.push_back(huxerui::TextField(fs.headers.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(3, 8))
            .OnChanged([fs](const huxerui::TextEditingValue& v) { fs.headers = v; }));
    }
    fields.push_back(huxerui::Row {
        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        huxerui::Button(editingName.empty() ? "添加" : "保存")
            .OnClick([=] {
                const std::string name = fs.name.Get().text;
                if (!IsValidSlug(name)) {
                    toast.Show("名称只能包含字母、数字、连字符，且不能为空");
                    return;
                }
                mcp::McpServer srv;
                srv.name = name;
                srv.type = std::string(kTypeNames[fs.type.Get()]);
                if (isStdio) {
                    srv.command = fs.command.Get().text;
                    srv.args = SplitArgs(fs.args.Get().text);
                    srv.env = ParseLines(fs.env.Get().text, '=');
                    if (srv.command.empty()) {
                        toast.Show("命令不能为空");
                        return;
                    }
                } else {
                    srv.url = fs.url.Get().text;
                    srv.headers = ParseLines(fs.headers.Get().text, ':');
                    if (srv.url.empty()) {
                        toast.Show("URL 不能为空");
                        return;
                    }
                }
                try {
                    if (!editingName.empty()) {
                        // 编辑保留原启用清单；改名先删旧键再 upsert。
                        for (const auto& cur : mcpStore().servers()) {
                            if (cur.name == editingName) {
                                srv.enabledTools = cur.enabledTools;
                                break;
                            }
                        }
                        if (editingName != name) mcpStore().remove(editingName);
                    }
                    mcpStore().upsert(std::move(srv));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                    return;
                }
                ctx.Dismiss();
                revision = revision.Get() + 1;
                toast.Show(editingName.empty() ? "已添加" : "已保存");
            }),
    }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)));
    return DialogCard(huxerui::ScrollView(huxerui::Column(std::move(fields))
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .With(huxerui::Frame{.width = 420.0F}));
}

// 打开新增/编辑弹窗（普通函数：dialog.Show 的工厂转发到 composable 内容）。
void ShowMcpForm(huxerui::DialogHandle dialog, huxerui::ToastHandle toast,
                 const std::string& title, const McpFormStates& fs,
                 const std::string& editingName, huxerui::State<int> revision) {
    dialog.Show(
        [=](huxerui::DialogContext ctx) -> huxerui::View {
            return McpFormContent(title, fs, editingName, ctx, toast, revision);
        },
        huxerui::DialogOptions{});
}

[[huxerui::composable]] huxerui::View McpServerCard(
    const mcp::McpServer& server, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto dialog = huxerui::UseDialog();
    const McpFormStates fs{huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(0),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""})};
    const std::string name = server.name;

    auto bump = [revision] { revision = revision.Get() + 1; };

    // 编辑：预填表单后开弹窗（弹窗会卸载点击路径上的节点：推迟出指针事件路径）。
    auto showEdit = [dialog, tasks, toast, fs, server, name, revision] {
        FillMcpForm(fs, server);
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            ShowMcpForm(dialog, toast, "编辑 MCP 服务器 — " + server.name, fs, name,
                        revision);
        });
    };

    auto showDeleteConfirm = [dialog, toast, name, bump] {
        dialog.Show(
            "删除 MCP 服务器",
            std::format("确定删除「{}」？将从各工具的启用配置中一并移除。", name),
            "删除", "取消",
            [toast, name, bump] {
                try {
                    mcpStore().remove(name);
                    toast.Show(std::format("已删除 {}", name));
                } catch (const std::exception& e) {
                    toast.Show(e.what());
                }
                bump();
            },
            {});
    };

    // 每工具启用开关：点动即写对应工具 live 配置；失败 toast 中文错
    // （如「该工具暂不支持 MCP 管理」）。
    std::vector<huxerui::View> toggles;
    for (const std::string_view toolId : kMcpTools) {
        const bool enabled =
            std::find(server.enabledTools.begin(), server.enabledTools.end(),
                      toolId) != server.enabledTools.end();
        toggles.push_back(
            huxerui::Switch(std::string(ToolName(toolId)), enabled)
                .OnChanged([toast, name, toolId = std::string(toolId), bump](bool on) {
                    try {
                        mcpStore().setEnabled(name, toolId, on);
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    bump();
                }));
    }

    return Card(huxerui::Column {
        huxerui::Row {
            huxerui::Text(name).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
            huxerui::Text(TypeOf(server)).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_primary})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 2.0F)),
                      huxerui::Background(theme.colors.primary),
                      huxerui::CornerRadius(islands.nested_radius)),
            huxerui::Spacer(),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        SummaryOf(server).empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(SummaryOf(server))
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::Monospace(font_size::kChip),
                                    theme.colors.on_surface_variant})},
        huxerui::Row(std::move(toggles))
            .With(huxerui::Spacing(16.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Button("编辑").OnClick([showEdit] { showEdit(); }),
            huxerui::Button("删除").OnClick([tasks, showDeleteConfirm] {
                // 弹窗会卸载点击路径上的节点：推迟出指针事件路径。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    showDeleteConfirm();
                });
            }),
        }.With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .Key(name);
}

// 「从工具收编」弹窗内容：选工具 → importFromTool → toast 收编数量。
[[huxerui::composable]] huxerui::View McpImportContent(huxerui::DialogContext ctx,
                                                       huxerui::ToastHandle toast,
                                                       huxerui::State<int> revision) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> items;
    for (const std::string_view toolId : kImportTools) {
        const std::string tool = std::string(toolId);
        items.push_back(
            huxerui::Text(std::string(ToolName(toolId)))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface})
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                      huxerui::CornerRadius(8.0F))
                .OnClick([ctx, toast, tool, revision] {
                    try {
                        const std::size_t n = mcpStore().importFromTool(tool);
                        toast.Show(n == 0 ? "没有新的可收编条目"
                                          : std::format("已收编 {} 个 MCP 服务器", n));
                    } catch (const std::exception& e) {
                        toast.Show(e.what());
                    }
                    revision = revision.Get() + 1;
                    ctx.Dismiss();
                }));
    }
    return DialogCard(huxerui::Column {
        huxerui::Text("从工具收编", huxerui::TextRole::Title),
        huxerui::Text("读取该工具现有 MCP 配置并入统一清单；同名条目只补启用标记。")
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        huxerui::Column(std::move(items))
            .With(huxerui::Spacing(4.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Row {
            huxerui::Spacer(),
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        },
    }.With(huxerui::Spacing(12.0F),
           huxerui::Frame{.width = 380.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace

[[huxerui::composable]] huxerui::View McpPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    // 页面内变更计数：任何写库操作后 +1，驱动本页重读。
    auto revision = huxerui::UseState(0);
    (void)revision.Get();
    const McpFormStates fs{huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(0),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""}),
                           huxerui::UseState(huxerui::TextEditingValue{""})};

    auto showCreateDialog = [=] {
        FillMcpForm(fs, mcp::McpServer{});
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            ShowMcpForm(dialog, toast, "新增 MCP 服务器", fs, "", revision);
        });
    };

    auto showImportDialog = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return McpImportContent(ctx, toast, revision);
                },
                huxerui::DialogOptions{});
        });
    };

    std::vector<huxerui::View> cards;
    for (const auto& srv : mcpStore().servers()) {
        cards.push_back(McpServerCard(srv, tasks, toast, revision));
    }

    return PageScaffold(
        "MCP 服务器",
        huxerui::Row {
            huxerui::Button("从工具收编").OnClick([showImportDialog] {
                showImportDialog();
            }),
            huxerui::Button("新增 MCP 服务器").OnClick([showCreateDialog] {
                showCreateDialog();
            }),
        }.With(huxerui::Spacing(8.0F)),
        cards.empty()
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("还没有 MCP 服务器。点击右上角「新增 MCP 服务器」"
                                    "或「从工具收编」开始。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : huxerui::View{huxerui::ScrollView(
                                huxerui::Column(std::move(cards))
                                    .With(huxerui::Spacing(10.0F),
                                          huxerui::CrossAlign(
                                              huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F))});
}

} // namespace llmswitch::ui
