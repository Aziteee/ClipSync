# Module Status Display via KSU `override.description`

**Date:** 2026-07-03

## Goal

Display the `clipsyncd` daemon's runtime status (run state + Zygisk bridge health) in the KernelSU Manager app's module description, using KSU's `override.description` config key.

## Background

KernelSU supports `ksud module config set override.description "text"` which dynamically replaces the static `description` field from `module.prop` when shown in the KSU Manager. Currently the module description is static: `"ClipSync clipboard sync daemon with Zygisk bridge"`.

## Design

### Status string format

```
{RunState} | Bridge: {BridgeStatus} | Port: {Port}
```

| Field | Values |
|---|---|
| `RunState` | `"Running"`, `"Starting"`, `"Stopped"` |
| `BridgeStatus` | `"OK"`, `"ERR"`, `"-"` (unknown/starting) |
| `Port` | The configured listen port number |

Examples:
- `"Running | Bridge: OK | Port: 5287"`
- `"Running | Bridge: ERR | Port: 5287"`
- `"Starting | Bridge: - | Port: 5287"`
- `"Stopped | Bridge: -"`

### Bridge health detection

A global flag `g_bridge_healthy` tracks Zygisk bridge status:
- Set to `1` during init when `binder_clip_init()` returns 0
- Set to `1` whenever `binder_clip_get_text()` returns non-NULL
- Set to `0` when `binder_clip_get_text()` returns NULL (bridge may have failed)

### Update mechanism

A new function `update_module_status()` in `clipsyncd.c`:
1. Constructs the status string with `snprintf`
2. Builds a shell command: `ksud module config set override.description "..."` 
3. Calls `system()` to execute it
4. Logs errors from `system()` to stderr

Called at three points:
- **Startup**: once after all subsystems init (line ~97 in current code)
- **Periodically**: every 100 event loop iterations (100 × 50ms = ~5 seconds)
- **Shutdown**: once before exiting (line ~110)

### Implementation scope

| File | Change |
|---|---|
| `clipsyncd.c` | Add `g_bridge_healthy` flag, `update_module_status()` function, periodic invocation in event loop, startup/shutdown calls |

### Error handling

- `system()` failures logged but daemon continues — status display is cosmetic
- Command injection risk is low: all values in the status string are programmatic (strings/ints), not user-sourced

## Non-goals

- Does NOT show connected client count or auth status (can be added later if needed)
- Does NOT add external dependencies

## Verification

After deployment:
1. Open KSU Manager, check `clipsyncd` module — description should show `"Running | Bridge: OK | Port: 5287"`
2. Kill daemon, refresh KSU Manager — description should revert to static module.prop text
3. Restart daemon, check description updates back to running state
