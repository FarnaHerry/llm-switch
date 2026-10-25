// pet.h — AI 桌宠（透明工具窗口小圆球）。
//
// 框架 501e8ca 起支持运行时多窗口与透明 Utility 窗口（examples/desktop_pet
// 是同一套能力的官方演示）；llm-switch 在其上做一个 AI 语义的桌宠：
//   * 视觉：石墨玻璃球 + 居中呼吸的品牌莲花（lotus_bloom_raster，冷白
//     alpha-mask，固定深色玻璃——桌面饰品在深浅桌面下都可读）；
//   * 状态（每秒轮询本地路由；路由未运行时显示未知）：streaming（在途请求
//     > 0，呼吸加深加快）、failure（最近一次请求失败的红脉冲窗）、
//     donePendingAck（✓ 完成提醒，点击/右键/新任务消解，30 秒后自动消退）、
//     activeTools（最近 60s 有请求的活跃 Agent，≤2 个，顶部浮标）、
//     todayRequests（今日请求数，底部角标）；
//   * 交互：点击打开主窗口，拖拽移动，右键菜单（路由开关 / 隐藏 / 退出），
//     菜单经 PetThemeDefinition 套主应用同款主题。
// 控制器由 InstallApplication 注册为应用级服务，设置页与托盘共用同一实例。
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/huxerui.h>

namespace llmswitch::ui {

// 桌宠窗口的 WindowOptions::key；AppRoot 按 key 把组合分发到 PetContent。
inline constexpr std::string_view kPetWindowKey = "pet";
// 桌宠信息卡窗口的 key；AppRoot 按它分发到 PetCardContent。
inline constexpr std::string_view kPetCardWindowKey = "pet-card";

// 桌宠窗口参数：透明、无边框 Utility，创建时请求置顶与被动显示
// （Wayland 拒绝时由 Show 回退到合成器默认策略）。compact 缩为胶囊，
// live2d 用更高的窗口容纳立绘。
inline huxerui::WindowOptions PetOptions(bool topmost, bool compact, bool live2d) {
    huxerui::Size size{200.0F, 200.0F};
    if (compact) size = {150.0F, 44.0F};
    if (live2d) size = {300.0F, 420.0F};
    return {
        .title = "llm-switch 桌宠",
        .key = std::string(kPetWindowKey),
        .initial_size = size,
        .presentation = huxerui::WindowPresentation::Utility,
        .transparent_surface = true,
        .show_without_activation = true,
        .topmost = topmost,
    };
}

// 信息卡的每 Agent 今日行（Poll 聚合，进 State 需 operator==）。
struct PetAgentRow {
    std::string tool;
    std::int64_t requests = 0;
    std::int64_t promptTokens = 0;     // -1 计数不计入
    std::int64_t completionTokens = 0;
    std::int64_t errors = 0;
    std::string activityLabel;
    bool operator==(const PetAgentRow&) const = default;
};

enum class PetRecentOutcome { completed, failed, cancelled };

// 信息卡最近结果只保留状态、来源、供应商与可用 token 元数据，不含请求正文。
struct PetRecentResult {
    std::string tool;
    std::string providerName;
    std::string sourceLabel;
    PetRecentOutcome outcome = PetRecentOutcome::completed;
    std::int64_t timestampMillis = 0;
    int statusCode = 0;  // 路由失败状态；0 表示连接失败，原生事件不适用。
    std::int64_t promptTokens = -1;
    std::int64_t completionTokens = -1;
    bool operator==(const PetRecentResult&) const = default;
};

// 信息卡快照：今日总数、按 tool 的今日聚合（注册表序）和最近结果。
struct PetCardSnapshot {
    std::int64_t todayRequests = 0;
    std::vector<PetAgentRow> rows;
    std::vector<PetRecentResult> recentResults;
    bool operator==(const PetCardSnapshot&) const = default;
};

enum class PetNativeState { unknown, idle, working, waiting, completed, failed, cancelled };

struct PetNativeSourceSnapshot {
    std::string tool;
    PetNativeState state = PetNativeState::unknown;
    std::int64_t lastEventMillis = 0;
    bool enabled = false;
    bool unknown = true;
};

struct PetNativeOutcomeSnapshot {
    std::string tool;
    PetNativeState state = PetNativeState::unknown;
    std::int64_t occurredAtMillis = 0;
};

struct PetNativeActivitySnapshot {
    std::vector<std::string> activeTools;
    std::vector<PetNativeSourceSnapshot> sources;
    std::vector<PetNativeOutcomeSnapshot> recentOutcomes;
};

// 桌宠控制器：持有桌宠窗口句柄与 AI 活动状态。线程约定同框架示例——
// 句柄管理与状态读写都发生在 UI 线程（组合 / 事件 / Task 续体），不加锁。
struct PetController {
    std::optional<huxerui::WindowHandle> pet;
    std::optional<huxerui::Point> saved_position;
    std::uint64_t generation = 0;
    huxerui::State<bool> topmost{true};
    huxerui::State<bool> animate{true};
    // 轻量级模式（对齐 Clash-Flux 的轻量级托盘）：隐藏时退役窗口、释放
    // UI 树与渲染资源（应用状态/托盘/进程继续运行），唤出时按进程内保留的
    // 位置与偏好重建，波纹相位从头开始。
    huxerui::State<bool> lightweight{false};
    // ---- AI 活动状态（Poll 每秒写、组合读；State 变更驱动桌宠重组）----
    // LocalRouter 未运行或本次运行尚未观测到路由请求时，活动状态未知。
    huxerui::State<bool> activityUnknown{true};
    // streaming：路由在途请求 > 0（真状态，来自 LocalRouter::inflight）。
    huxerui::State<bool> streaming{false};
    // working 表示原生 Agent hooks 观察到的工作中/等待中；不是 token stream。
    huxerui::State<bool> agentWorking{false};
    huxerui::State<bool> waitingForInput{false};
    // failure：最近一次路由请求失败（传输失败或 ≥400）后的红脉冲窗内。
    huxerui::State<bool> failure{false};
    // donePendingAck：完成提醒，点击 / 右键 / 新任务可消解，30 秒后自动消失。
    huxerui::State<bool> donePendingAck{false};
    std::int64_t doneExpiresAtMillis = 0;
    // 活跃 Agent（tool id，最多 2 个）：来自路由近期请求与原生 hook 会话，
    // 桌宠顶部浮标展示。
    // 注意 State 必须带初值构造：默认构造是空句柄，Get() 抛
    // logic_error("HuxerUI state is empty")。
    huxerui::State<std::vector<std::string>> activeTools{std::vector<std::string>{}};
    huxerui::State<std::int64_t> todayRequests{0};  // 今日路由请求数（角标）
    // 信息卡快照（每 Agent 今日请求 / tokens / 失败数，Poll 聚合）。
    huxerui::State<PetCardSnapshot> cardSnapshot{PetCardSnapshot{}};
    // 胶囊模式：桌宠缩为「状态点 + 今日次数」的小胶囊（同一窗口的另一种
    // 组合，切换经 Retire + 按新尺寸重建）。
    huxerui::State<bool> compact{false};
    // Live2D 皮肤（config.petLive2D 的运行时镜像）：开启且编译可用
    // （LLMSWITCH_LIVE2D）且模型存在时，桌宠用 Live2D 模型渲染；窗口更高。
    huxerui::State<bool> live2d{false};
    // 系统托盘显示偏好；关闭时不能隐藏最后一个可恢复入口。
    huxerui::State<bool> trayEnabled{true};
    // Tray presentation is application-owned; pet window creation/retirement
    // refreshes its menu even when the control center has never been opened.
    std::function<void()> refreshTray;
    // 窗口存活性代次：Show/Retire 自增，托盘菜单以它为依赖跟随重建勾选态
    // （轻量级模式下隐藏即退役，托盘勾选必须随之消失）。
    huxerui::State<std::uint64_t> epoch{0};
    // 深浅主题（与应用同一规则：themeMode + 系统偏好），Poll 每秒跟随——
    // 右键菜单与桌面不变时切换主题，桌宠菜单在一秒内跟上。
    huxerui::State<bool> darkTheme{true};

    // ---- 批次状态机（Poll 私有，UI 线程独占，不进 State）----
    // 首个 Poll 只对齐日志水位不算活动（启动时的历史日志不触发完成提醒）。
    bool firstPoll = true;
    bool localRouterObservedThisSession = false;
    std::int64_t lastSeenLogTs = 0;  // 已见过的最新日志时间戳
    std::int64_t lastActivityMs = 0;
    bool batchActive = false;
    bool batchAnySuccess = false;
    std::function<PetNativeActivitySnapshot(std::int64_t)> readNativeActivity;
    std::map<std::string, std::int64_t, std::less<>> lastNativeOutcomeMillis;

    // 每秒由 PetContent 的 Delay 循环调用：读路由与原生 Agent 快照，推进
    // streaming / failure / donePendingAck / activeTools / todayRequests /
    // cardSnapshot。
    void Poll();
    // 消解完成提醒（点击桌宠或右键菜单入口）。
    void AckDone() {
        donePendingAck = false;
        doneExpiresAtMillis = 0;
    }
    // 切换信息卡（点击桌宠 / 右键菜单）：在桌宠旁开锚定卡片窗口（owner =
    // 桌宠，随桌宠退役），再点收起。配置持久化由调用方负责。
    void ToggleCard(const huxerui::ApplicationHandle& application);
    // 胶囊模式切换（配置持久化由调用方负责）：存活时按新尺寸重建窗口，
    // 位置保留；信息卡随 owner 重建自动退役，不搬旧卡。
    void SetCompact(bool value, const huxerui::ApplicationHandle& application);
    // Live2D 皮肤切换（配置持久化由调用方负责）：与胶囊切换同一套重建
    // 语义（live2d 优先于 compact 决定窗口尺寸）。
    void SetLive2D(bool value, const huxerui::ApplicationHandle& application);

    std::optional<huxerui::WindowHandle> card;
    std::uint64_t cardGeneration = 0;

    void Show(const huxerui::ApplicationHandle& application);
    void Retire();
    void Hide(bool trayAvailable);
    void SetTopmost(bool value);
    // 轻量级开关：对已经藏起的桌宠启用时立即退役（释放资源）；托盘不可用
    // 时无唤出入口，拒绝开启。
    void SetLightweight(bool value, bool trayAvailable);
    void RecoverPosition();
};

// 桌宠窗口内容组合与窗口句柄绑定（生命周期挂载 / 卸载）。
[[huxerui::composable]] huxerui::View PetContent();

// 信息卡窗口内容组合（owner 锚定在桌宠旁的小窗）。
[[huxerui::composable]] huxerui::View PetCardContent();

// ---- Live2D 皮肤（pet_live2d.cpp / pet_live2d_stub.cpp 二选一编译）----
// 构译期可用性：LLMSWITCH_LIVE2D=ON 时 true。禁用时配置里的 petLive2D
// 不生效，桌宠回退莲花球。
bool PetLive2DAvailable();
// Live2D 皮肤主体（组合内调用）：扫描 dataDir()/live2d/ 下的第一个
// *.model3.json 异步加载渲染；无模型、加载或播放失败时用莲花球回退。
[[huxerui::composable]] huxerui::View PetLive2DBody(
    const std::shared_ptr<PetController>& controller);
[[huxerui::composable]] huxerui::View PetLotusBody(
    const std::shared_ptr<PetController>& controller);
[[huxerui::composable]] huxerui::View PetLive2DFallback(
    const std::shared_ptr<PetController>& controller, std::string status);

inline void UsePetAttachment(const std::shared_ptr<PetController>& controller) {
    const auto window = huxerui::UseWindow();
    huxerui::Lifecycle([controller, window] {
        const auto current = ++controller->generation;
        controller->pet = window;
        // 创建时请求置顶失败（Wayland）则记住合成器策略，不再反复请求。
        if (controller->topmost.Get() &&
            window.SetTopmost(true) != huxerui::WindowRequestResult::Submitted) {
            controller->topmost = false;
        }
        if (!controller->saved_position) controller->RecoverPosition();
        return [controller, current] {
            // 退役先于新窗口挂载时（Retire++generation 已使代次失配），
            // 旧附件的清理不得清掉新窗口的句柄。
            if (controller->generation == current) controller->pet.reset();
        };
    });
}

inline void UseCardAttachment(const std::shared_ptr<PetController>& controller) {
    const auto window = huxerui::UseWindow();
    huxerui::Lifecycle([controller, window] {
        const auto current = ++controller->cardGeneration;
        controller->card = window;
        return [controller, current] {
            if (controller->cardGeneration == current) controller->card.reset();
        };
    });
}

} // namespace llmswitch::ui
