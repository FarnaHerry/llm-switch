# Native Agent activity integrations

This folder owns native Agent activity input. Keep it separate from `src/ui/` and
`llmswitch.router`: adapters observe lifecycle metadata, while the UI renders
snapshots and LocalRouter remains an independent activity source.

## Layout

- `activity.cppm` / `activity.cpp`: shared event contract, source health, and
  in-memory multi-session aggregation.
- `activity_bridge.cppm` / `activity_bridge.cpp`: app-lifetime authenticated
  loopback receiver. Its short-lived descriptor contains only a loopback port
  and random token; the event contract remains in memory.
- `hook_manager.cppm` / `hook_manager.cpp`: idempotent user-level Claude Code
  and Codex hook installation/removal. It edits only handlers marked as
  llm-switch-owned, backs up changed configuration, and preserves unrelated
  fields and hooks.
- `adapters/hook_input.*`: bounded JSON input parser shared by command hooks.
- `adapters/claude_code.*` and `adapters/codex.*`: one adapter module per Agent.
  Add later integrations as their own `<agent>.cppm` / `<agent>.cpp` pair here.
- `platform/agent_hook/main.cpp`: standalone, fail-open hook process. It reduces
  input through the adapter before sending it to the local bridge and never
  writes hook input to stdout or logs. Codex `Stop` / `SubagentStop` receive only
  the fixed `{"continue":true}` success response required by their contract.

## Adapter boundary

Adapters may return only the registered tool id, a short-lived session key, a
normalized lifecycle state, and the local arrival time. Do not add prompt or
reply text, tool arguments/results, transcript paths, credentials, screen data,
or clipboard data to the event contract. Session keys stay in memory and are
never written to the app data directory. Unknown, untrusted, disabled, or stale
sources must stay unknown; they must not be converted to idle.

The application starts the bridge with its application lifetime, merges native
source snapshots into the pet state, and exposes independent opt-in switches in
first-run setup and Settings. Codex hooks still require review and trust in
Codex's `/hooks` UI; until a trusted event arrives, the source remains unknown.
Claude Code hooks can also be disabled by its own `disableAllHooks` setting.
The bridge/helper/configuration path is covered by local tests. A controlled
real-session spike on 2026-10-25 confirmed helper execution during a Codex TUI
task and shutdown, and during a failed Claude Code request. The pet's visible
state has not been verified end to end; Codex hooks also lack a dedicated turn
failure event, so do not claim full lifecycle coverage yet.

## Data and lifecycle

- Hook input is parsed with a size limit and reduced to event name, session key,
  normalized state, and local arrival time. Prompt/reply text, tool contents,
  transcript paths, credentials, screen data, and clipboard data are discarded.
- Events are sent only to `127.0.0.1` using a per-run random token. The bridge
  descriptor is owner-only on POSIX and removed when the application exits.
- Hook handlers request background execution and the helper always exits
  successfully. Codex may run `SessionEnd` synchronously despite `async`; in that
  case the helper only performs bounded local parsing and loopback delivery.
  Configuration changes are made on a worker thread and retain unrelated hooks.
- When a source is disabled, untrusted, unavailable, or stale, the pet reports
  unknown/未接入 instead of idle. Native activity does not contribute router
  request or token statistics.
