// activity.cppm — llmswitch.agents：原生 Agent 活动的统一事件契约与聚合器。
//
// 各 Agent 的 hook/plugin 适配器只产出白名单元数据；本模块只在内存里按
// (tool, session key) 聚合，不提供正文、工具参数或 transcript 的存储入口。
export module llmswitch.agents;

import std;

namespace agents {

export enum class ActivityState {
    unknown,
    idle,
    working,
    waiting,
    completed,
    failed,
    cancelled,
    sessionEnded,
};

export enum class SourceHealth {
    disabled,
    notInstalled,
    needsTrust,
    awaitingEvent,
    observing,
    unavailable,
};

// 适配器的短期事件。sessionKey 只用于当前进程内去重，不能持久化。
export struct ActivityEvent {
    std::string tool;
    std::string sessionKey;
    ActivityState state = ActivityState::unknown;
    std::int64_t occurredAtMillis = 0;
};

export struct AgentActivitySnapshot {
    std::string tool;
    SourceHealth health = SourceHealth::awaitingEvent;
    ActivityState state = ActivityState::unknown;
    std::size_t activeSessions = 0;
    std::size_t waitingSessions = 0;
    std::int64_t lastEventMillis = 0;
    bool unknown = true;
    bool operator==(const AgentActivitySnapshot&) const = default;
};

// 最近终态仅暴露 Agent、结果和时间；session key 继续留在聚合器内部。
export struct RecentActivityOutcome {
    std::string tool;
    ActivityState state = ActivityState::unknown;
    std::int64_t occurredAtMillis = 0;
    bool operator==(const RecentActivityOutcome&) const = default;
};

export struct ActivitySnapshot {
    ActivityState state = ActivityState::unknown;
    bool unknown = true;  // true 表示至少有一个启用来源未知，或没有启用来源
    std::vector<std::string> activeTools;
    std::vector<AgentActivitySnapshot> sources;
    std::vector<RecentActivityOutcome> recentOutcomes;
    bool operator==(const ActivitySnapshot&) const = default;
};

export class ActivityTracker {
public:
    // 配置来源健康状态。disabled 会清除该 Agent 的内存会话。
    void SetSourceHealth(std::string tool, SourceHealth health);

    // 应用一个归一事件；来源未配置/已停用、字段越界或乱序事件返回 false。
    bool Apply(const ActivityEvent& event);

    // 活动信任窗口：working/waiting 超过 5 分钟转 unknown；终态提示保留 8 秒；
    // 没有活动的来源在最后一次观察 60 秒后转 unknown。
    [[nodiscard]] ActivitySnapshot SnapshotAt(std::int64_t nowMillis) const;

    void Clear();

private:
    struct RuntimeSource {
        SourceHealth health = SourceHealth::awaitingEvent;
        bool observedThisRun = false;
        std::int64_t lastEventMillis = 0;
    };

    struct RuntimeSession {
        ActivityState state = ActivityState::unknown;
        std::int64_t lastEventMillis = 0;
    };

    std::map<std::string, RuntimeSource, std::less<>> sources_;
    std::map<std::pair<std::string, std::string>, RuntimeSession> sessions_;
};

} // namespace agents
