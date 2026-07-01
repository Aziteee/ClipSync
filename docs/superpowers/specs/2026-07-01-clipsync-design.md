# ClipSync Design Spec

## Overview

Cross-device clipboard sync between Android (KernelSU module) and PC (Rust client). The Android daemon uses Binder IPC to monitor system clipboard changes via `IOnPrimaryClipChangedListener`, and a WebSocket server to relay content to/from the PC. The PC client discovers the phone via mDNS, connects over WebSocket with HMAC authentication, and syncs clipboard bidirectionally.

## Repository Structure (Monorepo)

```
ClipSync/
├── clipsync-pc/              # Rust PC client (bin crate)
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs           # Entry point, tokio + winit event loop
│       ├── config.rs         # clipsync.toml parsing
│       ├── clip.rs           # Win32 clipboard read/write/listen
│       ├── ws.rs             # WebSocket client + reconnect + HMAC handshake
│       ├── mdns.rs           # mDNS discovery (_clipsync._tcp)
│       ├── tray.rs           # System tray + context menu + status
│       ├── sync.rs           # Sync engine: dedup, state machine, direction control
│       └── protocol.rs       # JSON protocol ser/de (push/set/ping/pong)
│
├── clipsync-daemon/          # Android daemon (C, NDK)
│   ├── Makefile
│   ├── clipsyncd.c           # Main: Binder listener + WS server + event loop
│   ├── binder_clip.c/h       # Binder IPC: read/write/listen IClipboard
│   ├── ws_server.c/h         # WebSocket server (mongoose embedded)
│   ├── mdns_publish.c/h      # mDNS publish _clipsync._tcp
│   └── protocol.h            # JSON protocol constants (shared spec)
│
├── docs/
│   └── protocol.md           # Wire protocol specification
│
└── clipsync.toml.example     # Config template
```

**Boundary principle:** PC and daemon are coupled only by the JSON protocol specification. No shared code. The protocol text itself is the interface contract.

## PC Client Design

### Tech Stack

| Component | Crate / API |
|-----------|-------------|
| Async runtime | `tokio` (multi-threaded) |
| WebSocket | `tokio-tungstenite` |
| mDNS discovery | `mdns-sd` |
| System tray | `tray-icon` + `winit` |
| Clipboard | Win32 raw FFI (`GetClipboardData`, `SetClipboardData`, `AddClipboardFormatListener`) |
| Config | `toml` + `serde` |
| Hashing (dedup) | `blake3` |
| HMAC auth | `hmac` + `sha2` |

### Data Flow

```
Launch
  │
  ▼
Load config (clipsync.toml)
  │
  ▼
mDNS scan ──found──▶ Resolve phone IP:5287 ──▶ WebSocket connect + HMAC handshake
  │                     ▲                        │
  │ not found           │ timeout/fail           │ handshake OK
  ▼                     │                        ▼
Re-scan every 5s   Exponential backoff      Start clipboard listener
                                               │
                                    ┌──────────┴──────────┐
                                    ▼                      ▼
                            Local copy event       Incoming WS push
                                    │                      │
                                    ▼                      ▼
                            hash + debounce         Write to local clipboard
                                    │               (skip self-triggered echo)
                                    ▼
                            WS send "set" message
```

### Echo Prevention

Before writing to local clipboard, set a flag `self_writing = true`. The clipboard listener checks this flag and skips the event. Clear the flag after `SetClipboardData` returns.

### Pending Buffer (Offline Queue)

When disconnected (tray grey):

- Continue listening to local clipboard changes.
- Store the **most recent** change in a pending buffer (not a queue; clipboard is always "current content", intermediate states are irrelevant).
- On successful reconnect + HMAC handshake, flush the pending buffer to the phone.

### Reconnect Strategy

On disconnect: exponential backoff 1s → 2s → 4s → 8s … capped at 60s. Before each reconnect attempt, re-run mDNS scan (phone IP may have changed). Tray icon reflects state: green=connected, yellow=connecting, grey=disconnected.

### Clipboard Module (Win32)

**Listening:** Create a hidden `HWND`, register as Clipboard Format Listener via `AddClipboardFormatListener`. `WM_CLIPBOARDUPDATE` triggers read.

**Read:** `OpenClipboard` → `GetClipboardData(CF_UNICODETEXT)` → `CloseClipboard`. Compute blake3 hash for dedup.

**Write:** `OpenClipboard` → `EmptyClipboard` → `SetClipboardData(CF_UNICODETEXT, ...)` → `CloseClipboard`.

**Debounce:** On `WM_CLIPBOARDUPDATE`, start a 300ms timer. When timer fires, compare the latest hash against the last-sent hash. Subsequent events within 300ms reset the timer.

### System Tray

| State | Icon | Tooltip |
|-------|------|---------|
| Connected | green dot | ClipSync · Connected 192.168.1.x |
| Connecting | yellow dot | ClipSync · Connecting… |
| Disconnected | grey dot | ClipSync · Not connected |

**Right-click menu:**
- Reconnect (enabled when disconnected)
- Pause sync / Resume sync (toggle)
- ---
- Quit

**Event loop:** `winit` `ControlFlow::Poll` at 1s interval drives tray events and connection state display. `tokio` runtime runs in a separate thread, communicating state changes via `tokio::sync::mpsc`.

### Network Layer

**mDNS discovery (PC side, `mdns-sd` crate):**
- Query `_clipsync._tcp.local.` on startup.
- Use the first discovered instance.
- Background re-probe every 30s to handle phone IP changes.

**HMAC handshake:**
```
PC                              Daemon
 │                                  │
 │─── WS connect ─────────────────▶│
 │                                  │
 │◀── {"type":"hello","challenge":"<random-32-byte-hex>"} ──│
 │                                  │
 │─── {"type":"auth","response":"<HMAC-SHA256(secret,challenge)>"} ──▶│
 │                                  │
 │◀── {"type":"auth_ok"} ──────────│  (handshake done, start sync)
 │     or
 │◀── {"type":"auth_fail"} ────────│  (close connection)
```

### Configuration (clipsync.toml)

```toml
[connection]
port = 5287

[auth]
secret = "<pre-shared key>"

[clipboard]
debounce_ms = 300
```

No manual `phone` IP field; mDNS handles discovery.

## Android Daemon Design

### Tech Stack

| Component | Library |
|-----------|---------|
| Binder IPC | `libbinder_ndk.so` (Android 10+ NDK API) |
| WebSocket server | mongoose (single-file embedded C) |
| mDNS publish | mongoose built-in mDNS |
| Event loop | `epoll` |

### Module Architecture

```
clipsyncd (single-threaded epoll event loop)
│
├── binder_clip — Binder IPC wrapper
│   ├── init()         Get IClipboard service + register listener callback
│   ├── get_text()     AIBinder_transact → getPrimaryClip → parse ClipData → extract text
│   ├── set_text()     AIBinder_transact → setPrimaryClip(ClipData.newPlainText(null, text))
│   └── on_change()    Callback fires → read content → dedup → push via WS
│
├── ws_server — mongoose
│   ├── mg_mgr_poll()  Drive mongoose event loop
│   ├── ev_handler()   New connection → HMAC handshake → mark authenticated
│   └── on PC push     → binder_clip.set_text()
│
└── mdns_publish — mongoose mDNS
    └── Register _clipsync._tcp service, port 5287
```

### Main Loop

```c
for (;;) {
    // 1. Wait for Binder callback (fd readable, timeout=1s)
    // 2. mg_mgr_poll(mgr, 1) — drive WS + mDNS
    // 3. If debounce timer expired, flush pending text to WS
}
```

### Binder Clipboard Operations (no shell)

- **Read:** `transact getPrimaryClip` → return Parcel contains `ClipData` → extract `CharSequence` text from `ClipData.Item`
- **Write:** Construct `ClipData.newPlainText(null, text)` → `transact setPrimaryClip`

### Daemon Configuration

Embedded or at `/data/adb/modules/clipsync/clipsync.toml`:

```toml
[server]
port = 5287

[auth]
secret = "<pre-shared key>"

[clipboard]
debounce_ms = 300
```

### KernelSU Module Packaging

```
clipsync-module/
├── module.prop
├── post-fs-data.sh      # Launch clipsyncd on boot
├── system/bin/clipsyncd # ARM64 statically compiled binary
├── config/clipsync.toml
└── webroot/             # Reserved: future web management UI
```

**module.prop:**
```
id=clipsync
name=ClipSync Clipboard Sync
version=v1.0
versionCode=1
author=...
description=System-level clipboard sync daemon with Binder event listener (LAN)
```

## Wire Protocol (JSON over WebSocket)

### Message Types

```json
// Phone pushes text to PC
PC ◀── phone: {"type": "clipboard_push", "text": "...", "ts": 1719859200}

// PC writes text to phone clipboard
PC ──▶ phone: {"type": "clipboard_set",  "text": "...", "ts": 1719859200}

// Heartbeat
PC ◀─▶ phone: {"type": "ping"}
PC ◀─▶ phone: {"type": "pong"}

// Authentication handshake
PC ◀── phone: {"type": "hello", "challenge": "<32-byte-hex>"}
PC ──▶ phone: {"type": "auth", "response": "<HMAC-SHA256-hex>"}
PC ◀── phone: {"type": "auth_ok"}
PC ◀── phone: {"type": "auth_fail"}
```

### Conflict Resolution

Compare `ts` timestamps; keep the newer one. If both sides copy within the same millisecond, phone wins (phone arbitrates).

## Implementation Phases

### Phase 1 — PC Client Core
- [ ] Project scaffolding: Cargo.toml, config parsing, main entry + tokio runtime
- [ ] Win32 clipboard module: read/write/listen with echo prevention
- [ ] Protocol types and JSON ser/de
- [ ] Dedup engine: hash comparison + debounce timer

### Phase 2 — PC Client Networking
- [ ] WebSocket client: connect, reconnect with exponential backoff, HMAC handshake
- [ ] mDNS discovery
- [ ] Pending buffer for offline clipboard changes
- [ ] Sync engine: full state machine (connected → offline → reconnect → flush)

### Phase 3 — PC Client Tray
- [ ] System tray icon with state colors (green/yellow/grey)
- [ ] Right-click menu: reconnect, pause/resume, quit
- [ ] winit event loop integration with tokio mpsc bridge

### Phase 4 — Android Daemon
- [ ] Binder IPC wrapper: init, get_text, set_text, on_change callback
- [ ] WebSocket server with mongoose: connection handling, HMAC auth
- [ ] mDNS service publish
- [ ] Main epoll event loop integrating all components
- [ ] KernelSU module packaging

### Phase 5 — Polish
- [ ] Conflict arbitration (timestamp-based, phone-wins tiebreaker)
- [ ] Clipboard history (future)
- [ ] Image sync (future)
- [ ] End-to-end encryption (future)
