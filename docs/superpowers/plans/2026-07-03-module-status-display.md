# Module Status Display Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show clipsyncd runtime status (run state + bridge health) in KSU Manager via `override.description` config key.

**Architecture:** Single new function `update_module_status()` in `clipsyncd.c` builds status string and calls `ksud module config set override.description` via `system()`. Called at startup, every 5s in event loop, and at shutdown.

**Tech Stack:** C (clipsyncd), KernelSU `ksud` CLI

## Global Constraints

- All changes are in `clipsync-daemon/clipsyncd.c` only
- Must compile with existing Makefile (NDK aarch64-linux-android33-clang)
- `system()` failures must not crash daemon

---

### Task 1: Add status display to clipsyncd

**Files:**
- Modify: `clipsync-daemon/clipsyncd.c`

**Interfaces:**
- Produces: `update_module_status(clipsync_daemon_config *cfg)` — static function, no external consumers
- Produces: `g_bridge_healthy` — `static int`, tracked by `poll_clipboard_change()` and binder init

- [ ] **Step 1: Add global state and status update function**

Add after line 16 (`static char g_last_text[65536] = {0};`):

```c
static int g_bridge_healthy = 0;

static void update_module_status(clipsync_daemon_config *cfg) {
    const char *run_state;
    const char *bridge_state;
    char cmd[512];

    if (!cfg || !running) {
        run_state = "Stopped";
        bridge_state = "-";
    } else {
        run_state = "Running";
        bridge_state = g_bridge_healthy ? "OK" : "ERR";
    }

    snprintf(cmd, sizeof(cmd),
        "ksud module config set override.description \"%s | Bridge: %s | Port: %d\"",
        run_state, bridge_state, cfg ? cfg->port : 0);

    if (system(cmd) != 0) {
        fprintf(stderr, "[clipsyncd] failed to update module description\n");
    }
}
```

- [ ] **Step 2: Track bridge health in poll_clipboard_change()**

Replace the existing `poll_clipboard_change()` function (lines 42-47):

```c
static void poll_clipboard_change(void) {
    char *text = binder_clip_get_text();
    if (!text) {
        g_bridge_healthy = 0;
        return;
    }
    g_bridge_healthy = 1;
    on_clip_change(text);
    free(text);
}
```

- [ ] **Step 3: Set bridge healthy flag after binder init**

After line 84 (`if (binder_clip_init() != 0) { ... return 1; }`), add `g_bridge_healthy = 1;` on the line after the closing `}`:

The block becomes (lines 84-88):
```c
    if (binder_clip_init() != 0) {
        fprintf(stderr, "[clipsyncd] binder_clip_init failed\n");
        return 1;
    }
    g_bridge_healthy = 1;
    binder_clip_set_callback(on_clip_change);
```

- [ ] **Step 4: Call update_module_status after subsystems init**

After line 97 (`printf("[clipsyncd] running. Waiting...\n");`), add:

```c
    update_module_status(&cfg);
```

- [ ] **Step 5: Add periodic status update to event loop**

Replace the event loop (lines 100-108):

```c
    int clipboard_poll_ticks = 0;
    int status_update_ticks = 0;
    while (running) {
        ws_server_poll(50);
        if (++status_update_ticks >= 100) {
            status_update_ticks = 0;
            update_module_status(&cfg);
        }
        int poll_every_ticks = clipsync_clipboard_poll_ticks_for_clients(ws_server_authenticated_count());
        if (++clipboard_poll_ticks >= poll_every_ticks) {
            clipboard_poll_ticks = 0;
            poll_clipboard_change();
        }
    }
```

- [ ] **Step 6: Call update_module_status on shutdown**

After line 110 (`printf("[clipsyncd] shutting down.\n");`), add shutdown status update:

```c
    printf("[clipsyncd] shutting down.\n");
    update_module_status(NULL);
    return 0;
```

- [ ] **Step 7: Build and verify**

Run: `make` in `clipsync-daemon/` directory with NDK environment set.

Expected: compilation succeeds with no warnings.

- [ ] **Step 8: Commit**

```bash
git add clipsync-daemon/clipsyncd.c
git commit -m "feat: add module status display via KSU override.description"
```
