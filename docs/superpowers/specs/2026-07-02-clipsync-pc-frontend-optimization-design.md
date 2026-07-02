# ClipSync PC Frontend Optimization Design

## Date: 2026-07-02

## Overview

Two changes to the clipsync-pc Windows tray application:

1. **Hide console window** — add `#![windows_subsystem = "windows"]` so the binary launches as a GUI app with no foreground terminal.
2. **Pause/Resume sync with toggle** — the existing "Pause Sync" tray menu item currently does nothing. Wire it up with an `AtomicBool` shared between the event loop and sync loop. Show orange tray icon + "Paused" tooltip when paused.

## Changes

### 1. `src/main.rs`

- Add `#![windows_subsystem = "windows"]` at line 1.
- Import `std::sync::atomic::{AtomicBool, Ordering}`.
- `App` struct gains `paused: Arc<AtomicBool>`.
- `App::new` takes `paused: Arc<AtomicBool>`.
- `about_to_wait`: intercept `TrayAction::TogglePause`, flip `AtomicBool` via `fetch_xor`, call `tray.set_paused(!was_paused)`, do not forward to sync loop.
- `main()` creates `Arc<AtomicBool>::new(false)`, clones one for `run_sync_loop`, one for `App`.
- `run_sync_loop` signature adds `paused: Arc<AtomicBool>`.
- In the connected `tokio::select!` loop, each clipboard-operation branch checks `paused.load(Ordering::Relaxed)` and skips if true. Keepalive (Ping/Pong) always runs.
- In the disconnected clip-buffering branch, skip `store_pending` when paused.

### 2. `src/tray.rs`

- Add `use std::sync::Arc;` and `use std::sync::atomic::{AtomicBool, Ordering};` (not needed actually — tray doesn't own the atomic, main.rs owns it).
- `Tray` struct adds fields: `conn_state: ConnState`, `paused: bool`.
- `new()` initializes `conn_state: ConnState::Disconnected, paused: false`.
- `update_state(&mut self, state: ConnState)` stores state then calls `self.refresh()`.
- New method `set_paused(&mut self, paused: bool)` stores flag then calls `self.refresh()`.
- Private `refresh(&mut self)`: if `self.paused` → orange icon (255, 165, 0) + "ClipSync · Paused" tooltip; else → delegate to existing `icon_for_state`/tooltip logic.

### 3. Paused behavior

| When paused | Behavior |
|---|---|
| Local clipboard change | Dropped (not sent, not queued) |
| Remote clipboard push | Dropped (not written) |
| Ping/Pong keepalive | Still processed |
| Connection state changes | Still reflected in tray (but icon is orange when paused) |
| Disconnected + paused | Still try to reconnect; do not buffer local clips |

## Data Flow

```
Tray menu "Pause Sync" clicked
  → MenuEvent thread → mpsc → TrayAction::TogglePause
  → about_to_wait flips AtomicBool, tray.set_paused(new_state)
  → Sync loop reads AtomicBool on each iteration, skips clipboard I/O
```
