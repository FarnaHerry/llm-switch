// hook_manager.cppm — 用户级原生 Agent hook 的幂等安装/卸载接口。
export module llmswitch.agents.hook_manager;

import std;

namespace agents::hooks {

export struct HookStatus {
    bool installed = false;
    bool blockedByUser = false;
    bool operator==(const HookStatus&) const = default;
};

// 仅识别本应用通过 --llmswitch-hook 标记写入的命令，不判断其它 hooks。
export HookStatus Inspect(std::string_view tool);

// 修改只触及本应用拥有的 handler；用户其它字段与 handler 保持原样。
// 安装被 Agent 自身关闭 hooks 时会抛错，不擅自覆盖用户的关闭选择。
export void Configure(std::string_view tool,
                      const std::filesystem::path& helperExecutable,
                      bool enabled);

} // namespace agents::hooks
