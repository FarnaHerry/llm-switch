// activity.cpp — llmswitch.agents 聚合器实现。
module llmswitch.agents;

import std;

namespace agents {
namespace {

constexpr std::int64_t kActiveFreshMillis = 5 * 60 * 1000;
constexpr std::int64_t kOutcomeFreshMillis = 8 * 1000;
constexpr std::int64_t kIdleFreshMillis = 60 * 1000;
constexpr std::size_t kMaxToolIdBytes = 64;
constexpr std::size_t kMaxSessionKeyBytes = 256;

std::int64_t Age(std::int64_t nowMillis, std::int64_t eventMillis) {
    if (nowMillis < eventMillis) return std::numeric_limits<std::int64_t>::max();
    return nowMillis - eventMillis;
}

bool IsActive(ActivityState state) {
    return state == ActivityState::working || state == ActivityState::waiting;
}

bool IsOutcome(ActivityState state) {
    return state == ActivityState::completed || state == ActivityState::failed ||
           state == ActivityState::cancelled;
}

int Priority(ActivityState state) {
    switch (state) {
        case ActivityState::waiting: return 5;
        case ActivityState::failed: return 4;
        case ActivityState::working: return 3;
        case ActivityState::completed: return 2;
        case ActivityState::cancelled: return 1;
        case ActivityState::idle: return 0;
        case ActivityState::unknown: return -1;
        case ActivityState::sessionEnded: return -1;
    }
    return -1;
}

} // namespace

void ActivityTracker::SetSourceHealth(std::string tool, SourceHealth health) {
    if (tool.empty() || tool.size() > kMaxToolIdBytes) {
        throw std::invalid_argument("invalid Agent tool id");
    }

    RuntimeSource& source = sources_[tool];
    source.health = health;
    if (health != SourceHealth::observing) {
        source.observedThisRun = false;
        source.lastEventMillis = 0;
        std::erase_if(sessions_, [&tool](const auto& item) {
            return item.first.first == tool;
        });
    }
}

bool ActivityTracker::Apply(const ActivityEvent& event) {
    if (event.tool.empty() || event.tool.size() > kMaxToolIdBytes ||
        event.sessionKey.empty() || event.sessionKey.size() > kMaxSessionKeyBytes ||
        event.occurredAtMillis < 0 || event.state == ActivityState::unknown) {
        return false;
    }

    const auto sourceIt = sources_.find(event.tool);
    if (sourceIt == sources_.end() ||
        sourceIt->second.health == SourceHealth::disabled) {
        return false;
    }

    const auto key = std::pair{event.tool, event.sessionKey};
    const auto sessionIt = sessions_.find(key);
    if (sessionIt != sessions_.end() &&
        event.occurredAtMillis < sessionIt->second.lastEventMillis) {
        return false;
    }

    if (event.state == ActivityState::sessionEnded) {
        sessions_.erase(key);
    } else {
        sessions_[key] = RuntimeSession{event.state, event.occurredAtMillis};
    }

    RuntimeSource& source = sourceIt->second;
    source.health = SourceHealth::observing;
    source.observedThisRun = true;
    source.lastEventMillis = std::max(source.lastEventMillis,
                                      event.occurredAtMillis);
    return true;
}

ActivitySnapshot ActivityTracker::SnapshotAt(std::int64_t nowMillis) const {
    ActivitySnapshot result;
    result.state = ActivityState::unknown;
    result.unknown = false;
    int resultPriority = -1;
    std::size_t enabledSources = 0;

    for (const auto& [tool, source] : sources_) {
        AgentActivitySnapshot row;
        row.tool = tool;
        row.health = source.health;
        row.lastEventMillis = source.lastEventMillis;
        row.unknown = false;

        if (source.health == SourceHealth::disabled) {
            row.state = ActivityState::unknown;
            row.unknown = false;
            result.sources.push_back(std::move(row));
            continue;
        }
        ++enabledSources;

        bool staleActiveSession = false;
        std::size_t failedSessions = 0;
        std::size_t completedSessions = 0;
        std::size_t cancelledSessions = 0;
        for (const auto& [key, session] : sessions_) {
            if (key.first != tool) continue;
            const std::int64_t age = Age(nowMillis, session.lastEventMillis);
            if (IsActive(session.state)) {
                if (age <= kActiveFreshMillis) {
                    ++row.activeSessions;
                    if (session.state == ActivityState::waiting) {
                        ++row.waitingSessions;
                    }
                } else {
                    staleActiveSession = true;
                }
            } else if (IsOutcome(session.state) && age <= kOutcomeFreshMillis) {
                if (session.state == ActivityState::failed) ++failedSessions;
                else if (session.state == ActivityState::completed) ++completedSessions;
                else ++cancelledSessions;
            }
        }

        if (source.health != SourceHealth::observing || !source.observedThisRun ||
            staleActiveSession) {
            row.state = ActivityState::unknown;
            row.unknown = true;
        } else if (row.waitingSessions > 0) {
            row.state = ActivityState::waiting;
        } else if (failedSessions > 0) {
            row.state = ActivityState::failed;
        } else if (row.activeSessions > 0) {
            row.state = ActivityState::working;
        } else if (completedSessions > 0) {
            row.state = ActivityState::completed;
        } else if (cancelledSessions > 0) {
            row.state = ActivityState::cancelled;
        } else if (Age(nowMillis, source.lastEventMillis) <= kIdleFreshMillis) {
            row.state = ActivityState::idle;
        } else {
            row.state = ActivityState::unknown;
            row.unknown = true;
        }

        if (row.state == ActivityState::working ||
            row.state == ActivityState::waiting) {
            result.activeTools.push_back(tool);
        }
        if (row.unknown) result.unknown = true;
        if (Priority(row.state) > resultPriority) {
            result.state = row.state;
            resultPriority = Priority(row.state);
        }
        result.sources.push_back(std::move(row));
    }

    if (enabledSources == 0) result.unknown = true;
    if (resultPriority < 0 || (result.unknown && resultPriority == 0)) {
        result.state = ActivityState::unknown;
    }
    return result;
}

void ActivityTracker::Clear() {
    sessions_.clear();
    for (auto& [tool, source] : sources_) {
        (void)tool;
        source.observedThisRun = false;
        source.lastEventMillis = 0;
        if (source.health != SourceHealth::disabled) {
            source.health = SourceHealth::awaitingEvent;
        }
    }
}

} // namespace agents
