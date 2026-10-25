# Native Agent activity integrations

This folder owns native Agent activity input. Keep it separate from `src/ui/` and
`llmswitch.router`: adapters observe lifecycle metadata, while the UI renders
snapshots and LocalRouter remains an independent activity source.

## Layout

- `activity.cppm` / `activity.cpp`: shared event contract, source health, and
  in-memory multi-session aggregation.
- `adapters/hook_input.*`: bounded JSON input parser shared by command hooks.
- `adapters/claude_code.*` and `adapters/codex.*`: one adapter module per Agent.
  Add later integrations as their own `<agent>.cppm` / `<agent>.cpp` pair here.

## Adapter boundary

Adapters may return only the registered tool id, a short-lived session key, a
normalized lifecycle state, and the local arrival time. Do not add prompt or
reply text, tool arguments/results, transcript paths, credentials, screen data,
or clipboard data to the event contract. Session keys stay in memory and are
never written to the app data directory. Unknown, untrusted, disabled, or stale
sources must stay unknown; they must not be converted to idle.

This directory currently provides the event model and parser adapters. Hook
installation, local event transport, per-Agent settings, and PetController
integration are separate follow-up steps; the adapters alone do not mean that
an Agent is connected.
