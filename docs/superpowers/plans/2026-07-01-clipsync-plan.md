# ClipSync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a cross-device clipboard sync system: Rust PC client (Windows) with WebSocket + mDNS + system tray, and a C Android daemon (KernelSU module) with Binder IPC clipboard monitoring.

**Architecture:** Monorepo with `clipsync-pc/` (Rust bin crate) and `clipsync-daemon/` (C, NDK). Both communicate via JSON-over-WebSocket with HMAC-SHA256 authentication. No shared code — the protocol spec is the interface contract.

**Tech Stack:** Rust: tokio, tokio-tungstenite, mdns-sd, tray-icon, winit, toml, serde, serde_json, blake3, hmac, sha2, hex, windows. C: libbinder_ndk, mongoose.

## Global Constraints

- Port: 5287 (hardcoded default)
- Debounce: 300ms (configurable)
- HMAC auth: SHA-256 with pre-shared secret key from `clipsync.toml`
- Platform: Windows only for initial PC client (Phase 1-3)
- No shell commands for Android clipboard — direct Binder IPC only
- Pending buffer: stores only latest clipboard change during offline, flushes on reconnect
- Echo prevention: `AtomicBool` flag before `SetClipboardData`, checked in clipboard listener
- Reconnect: exponential backoff 1s→2s→4s→8s… capped 60s, re-scan mDNS before each attempt
- mDNS service type: `_clipsync._tcp.local.`
- Config file: `clipsync.toml` beside the binary

---

### Task 1: Project Scaffolding

**Files:**
- Create: `clipsync-pc/Cargo.toml`
- Create: `clipsync-pc/src/main.rs`

**Interfaces:**
- Produces: `main.rs` with `#[tokio::main]` entry point — compile-only, no logic yet

- [ ] **Step 1: Create Cargo.toml with all dependencies**

```toml
[package]
name = "clipsync-pc"
version = "0.1.0"
edition = "2021"

[dependencies]
tokio = { version = "1", features = ["full"] }
tokio-tungstenite = { version = "0.24", features = ["native-tls"] }
futures-util = "0.3"
mdns-sd = "0.14"
tray-icon = "0.19"
winit = "0.30"
toml = "0.8"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
blake3 = "1"
hmac = "0.12"
sha2 = "0.10"
hex = "0.4"
windows = { version = "0.58", features = [
    "Win32_System_DataExchange",
    "Win32_System_Ole",
    "Win32_UI_WindowsAndMessaging",
    "Win32_System_Threading",
    "Win32_Foundation",
    "Win32_System_Memory",
]}
anyhow = "1"
thiserror = "2"
log = "0.4"
env_logger = "0.11"
```

- [ ] **Step 2: Create minimal main.rs**

```rust
fn main() -> anyhow::Result<()> {
    env_logger::init();
    log::info!("ClipSync PC starting...");
    Ok(())
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cargo check`
Expected: Compiles successfully (may take a while downloading crates)

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/Cargo.toml clipsync-pc/src/main.rs
git commit -m "feat: add clipsync-pc project scaffolding"
```

---

### Task 2: Config Module

**Files:**
- Create: `clipsync-pc/src/config.rs`

**Interfaces:**
- Produces: `Config::load(P: AsRef<Path>) -> anyhow::Result<Config>`
- Produces: Struct `ClipSyncConfig` with `connection`, `auth`, `clipboard` sections

- [ ] **Step 1: Write the config struct with serde derives**

```rust
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipSyncConfig {
    #[serde(default)]
    pub connection: ConnectionConfig,
    #[serde(default)]
    pub auth: AuthConfig,
    #[serde(default)]
    pub clipboard: ClipboardConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConnectionConfig {
    #[serde(default = "default_port")]
    pub port: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AuthConfig {
    #[serde(default)]
    pub secret: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipboardConfig {
    #[serde(default = "default_debounce_ms")]
    pub debounce_ms: u64,
}

fn default_port() -> u16 {
    5287
}

fn default_debounce_ms() -> u64 {
    300
}

impl Default for ClipSyncConfig {
    fn default() -> Self {
        Self {
            connection: ConnectionConfig::default(),
            auth: AuthConfig::default(),
            clipboard: ClipboardConfig::default(),
        }
    }
}

impl Default for ConnectionConfig {
    fn default() -> Self {
        Self {
            port: default_port(),
        }
    }
}

impl Default for AuthConfig {
    fn default() -> Self {
        Self {
            secret: String::new(),
        }
    }
}

impl Default for ClipboardConfig {
    fn default() -> Self {
        Self {
            debounce_ms: default_debounce_ms(),
        }
    }
}

impl ClipSyncConfig {
    pub fn load(path: impl AsRef<Path>) -> anyhow::Result<Self> {
        let path = path.as_ref();
        if path.exists() {
            let content = std::fs::read_to_string(path)?;
            Ok(toml::from_str(&content)?)
        } else {
            Ok(Self::default())
        }
    }
}
```

- [ ] **Step 2: Add `mod config;` to main.rs and test loading**

In `main.rs`, replace content:

```rust
mod config;

use config::ClipSyncConfig;

fn main() -> anyhow::Result<()> {
    env_logger::init();
    let cfg = ClipSyncConfig::load("clipsync.toml")?;
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);
    Ok(())
}
```

- [ ] **Step 3: Create example config file**

Create `clipsync-pc/clipsync.toml.example`:

```toml
[connection]
port = 5287

[auth]
secret = "your-pre-shared-key-here"

[clipboard]
debounce_ms = 300
```

- [ ] **Step 4: Write and run test**

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let cfg = ClipSyncConfig::default();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
        assert!(cfg.auth.secret.is_empty());
    }

    #[test]
    fn test_parse_toml() {
        let toml_str = r#"
[connection]
port = 9999

[auth]
secret = "test-key"

[clipboard]
debounce_ms = 500
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.connection.port, 9999);
        assert_eq!(cfg.auth.secret, "test-key");
        assert_eq!(cfg.clipboard.debounce_ms, 500);
    }
}
```

Run: `cargo test`
Expected: 2 tests PASS

- [ ] **Step 5: Commit**

```bash
git add clipsync-pc/src/config.rs clipsync-pc/src/main.rs clipsync-pc/clipsync.toml.example
git commit -m "feat: add config module with toml parsing"
```

---

### Task 3: Protocol Module

**Files:**
- Create: `clipsync-pc/src/protocol.rs`

**Interfaces:**
- Produces: Enum `ClipMessage` variants: `Push { text, ts }`, `Set { text, ts }`, `Ping`, `Pong`, `Hello { challenge }`, `Auth { response }`, `AuthOk`, `AuthFail`
- Produces: `ClipMessage::to_json(&self) -> String`
- Produces: `ClipMessage::from_json(json: &str) -> serde_json::Result<Self>`

- [ ] **Step 1: Write protocol types**

```rust
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ClipMessage {
    #[serde(rename = "clipboard_push")]
    Push {
        text: String,
        ts: u64,
    },
    #[serde(rename = "clipboard_set")]
    Set {
        text: String,
        ts: u64,
    },
    #[serde(rename = "ping")]
    Ping,
    #[serde(rename = "pong")]
    Pong,
    #[serde(rename = "hello")]
    Hello {
        challenge: String,
    },
    #[serde(rename = "auth")]
    Auth {
        response: String,
    },
    #[serde(rename = "auth_ok")]
    AuthOk,
    #[serde(rename = "auth_fail")]
    AuthFail,
}

impl ClipMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).unwrap()
    }

    pub fn from_json(json: &str) -> serde_json::Result<Self> {
        serde_json::from_str(json)
    }

    pub fn push(text: String) -> Self {
        ClipMessage::Push {
            text,
            ts: now_millis(),
        }
    }

    pub fn set(text: String) -> Self {
        ClipMessage::Set {
            text,
            ts: now_millis(),
        }
    }
}

fn now_millis() -> u64 {
    use std::time::SystemTime;
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
```

- [ ] **Step 2: Write tests**

At the bottom of `protocol.rs`:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_push_roundtrip() {
        let msg = ClipMessage::Push {
            text: "hello".into(),
            ts: 1719859200,
        };
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Push { text, ts } => {
                assert_eq!(text, "hello");
                assert_eq!(ts, 1719859200);
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_hello_roundtrip() {
        let msg = ClipMessage::Hello {
            challenge: "abc123".into(),
        };
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Hello { challenge } => {
                assert_eq!(challenge, "abc123");
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_ping_roundtrip() {
        let msg = ClipMessage::Ping;
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        assert!(matches!(decoded, ClipMessage::Ping));
    }

    #[test]
    fn test_push_json_format() {
        let msg = ClipMessage::Push {
            text: "test".into(),
            ts: 123,
        };
        let json = msg.to_json();
        assert!(json.contains("\"type\":\"clipboard_push\""));
        assert!(json.contains("\"text\":\"test\""));
    }
}
```

Run: `cargo test protocol`
Expected: 4 tests PASS

- [ ] **Step 3: Add `mod protocol;` to main.rs**

```rust
mod config;
mod protocol;
```

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/src/protocol.rs clipsync-pc/src/main.rs
git commit -m "feat: add protocol module with JSON message types"
```

---

### Task 4: Win32 Clipboard Module

**Files:**
- Create: `clipsync-pc/src/clip.rs`

**Interfaces:**
- Produces: `pub fn read() -> Option<String>` — read clipboard text
- Produces: `pub fn write(text: &str) -> bool` — write text to clipboard, returns success
- Produces: `pub struct ClipListener` with `fn new(tx: tokio::sync::mpsc::UnboundedSender<String>) -> Self` and `fn spawn(self)` — spawns message-loop thread, sends clipboard text on change
- Uses `AtomicBool` for echo prevention: `pub fn set_self_writing(v: bool)` / `pub fn is_self_writing() -> bool`

- [ ] **Step 1: Write the module**

```rust
use std::sync::atomic::{AtomicBool, Ordering};
use tokio::sync::mpsc;
use windows::core::PCWSTR;
use windows::Win32::Foundation::*;
use windows::Win32::System::DataExchange::*;
use windows::Win32::System::Memory::*;
use windows::Win32::System::Ole::*;
use windows::Win32::System::Threading::*;
use windows::Win32::UI::WindowsAndMessaging::*;

static SELF_WRITING: AtomicBool = AtomicBool::new(false);

pub fn set_self_writing(v: bool) {
    SELF_WRITING.store(v, Ordering::SeqCst);
}

pub fn is_self_writing() -> bool {
    SELF_WRITING.load(Ordering::SeqCst)
}

pub fn read() -> Option<String> {
    unsafe {
        if OpenClipboard(None).is_err() {
            return None;
        }
        let result = read_inner();
        let _ = CloseClipboard();
        result
    }
}

unsafe fn read_inner() -> Option<String> {
    let handle = GetClipboardData(CF_UNICODETEXT.0 as u32);
    if handle.is_invalid() {
        return None;
    }
    let ptr = GlobalLock(handle.0) as *const u16;
    if ptr.is_null() {
        return None;
    }
    let len = (0..).take_while(|&i| *ptr.add(i) != 0).count();
    let slice = std::slice::from_raw_parts(ptr, len);
    let text = String::from_utf16_lossy(slice);
    let _ = GlobalUnlock(handle.0);
    Some(text)
}

pub fn write(text: &str) -> bool {
    unsafe {
        set_self_writing(true);
        if OpenClipboard(None).is_err() {
            set_self_writing(false);
            return false;
        }
        let _ = EmptyClipboard();

        let wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
        let byte_size = wide.len() * std::mem::size_of::<u16>();
        let hmem = GlobalAlloc(GMEM_MOVEABLE.0 | GMEM_ZEROINIT.0, byte_size);
        if hmem.is_invalid() {
            let _ = CloseClipboard();
            set_self_writing(false);
            return false;
        }
        let dst = GlobalLock(hmem.0) as *mut u16;
        if !dst.is_null() {
            std::ptr::copy_nonoverlapping(wide.as_ptr(), dst, wide.len());
            let _ = GlobalUnlock(hmem.0);
        }
        let result = SetClipboardData(CF_UNICODETEXT.0 as u32, HANDLE(hmem.0));
        let _ = CloseClipboard();
        let ok = !result.is_invalid();
        set_self_writing(false);
        ok
    }
}

pub struct ClipListener {
    tx: mpsc::UnboundedSender<String>,
}

impl ClipListener {
    pub fn new(tx: mpsc::UnboundedSender<String>) -> Self {
        Self { tx }
    }

    pub fn spawn(self) {
        std::thread::spawn(move || {
            unsafe { message_loop(self.tx) };
        });
    }
}

static mut TX_PTR: Option<mpsc::UnboundedSender<String>> = None;

unsafe fn message_loop(tx: mpsc::UnboundedSender<String>) {
    TX_PTR = Some(tx);

    let hinstance = GetModuleHandleW(None).unwrap();
    let class_name = PCWSTR::from_raw(wide!("ClipSyncClipListener").as_ptr());

    let wc = WNDCLASSW {
        lpfnWndProc: Some(wndproc),
        hInstance: hinstance.into(),
        lpszClassName: class_name,
        ..Default::default()
    };

    if RegisterClassW(&wc) == 0 {
        log::error!("RegisterClassW failed");
        return;
    }

    let hwnd = CreateWindowExW(
        WINDOW_EX_STYLE::default(),
        class_name,
        windows::core::w!("ClipSync"),
        WINDOW_STYLE::default(),
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        None,
        None,
        hinstance,
        None,
    );

    if hwnd.is_invalid() {
        log::error!("CreateWindowExW failed");
        return;
    }

    if !AddClipboardFormatListener(hwnd).as_bool() {
        log::error!("AddClipboardFormatListener failed");
        return;
    }

    let mut msg = MSG::default();
    loop {
        let ret = GetMessageW(&mut msg, None, 0, 0);
        if ret.0 <= 0 {
            break;
        }
        let _ = TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    let _ = RemoveClipboardFormatListener(hwnd);
    let _ = DestroyWindow(hwnd);
}

unsafe extern "system" fn wndproc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match msg {
        WM_CLIPBOARDUPDATE => {
            if !SELF_WRITING.load(Ordering::SeqCst) {
                if let Some(ref tx) = TX_PTR {
                    if let Some(text) = read() {
                        let _ = tx.send(text);
                    }
                }
            }
            LRESULT(0)
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            LRESULT(0)
        }
        _ => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}
```

- [ ] **Step 2: Write test — clipboard read/write roundtrip (ignored by default, touches system clipboard)**

At bottom of `clip.rs`:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_read_write_roundtrip() {
        let test_text = "ClipSync-test-你好世界";
        assert!(write(test_text));
        let read_back = read().unwrap();
        assert_eq!(read_back, test_text);
    }

    #[test]
    fn test_write_sets_self_writing_flag() {
        // flag should be false after write completes
        write("test");
        assert!(!is_self_writing());
    }

    #[test]
    fn test_read_none_when_empty() {
        // Just ensure read() doesn't panic when clipboard is empty/non-text
        let _ = read();
    }
}
```

Run: `cargo test clip`
Expected: 3 tests PASS (test_read_write_roundtrip may need visual verification)

- [ ] **Step 3: Add `mod clip;` to main.rs**

```rust
mod clip;
mod config;
mod protocol;
```

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/src/clip.rs clipsync-pc/src/main.rs
git commit -m "feat: add Win32 clipboard read/write/listen module"
```

---

### Task 5: Sync Engine (dedup + debounce + pending buffer)

**Files:**
- Create: `clipsync-pc/src/sync.rs`

**Interfaces:**
- Produces: `pub struct SyncEngine` with:
  - `pub fn new() -> Self`
  - `pub fn should_send(&self, text: &str) -> bool` — true if hash differs from last-sent
  - `pub fn mark_sent(&mut self, text: &str)` — remember this hash
  - `pub fn store_pending(&mut self, text: String)` — store for flush later
  - `pub fn take_pending(&mut self) -> Option<String>` — retrieve and clear pending

- [ ] **Step 1: Write the sync engine**

```rust
use blake3::Hash;

pub struct SyncEngine {
    last_sent_hash: Option<Hash>,
    pending: Option<String>,
}

impl SyncEngine {
    pub fn new() -> Self {
        Self {
            last_sent_hash: None,
            pending: None,
        }
    }

    fn hash(text: &str) -> Hash {
        blake3::hash(text.as_bytes())
    }

    pub fn should_send(&self, text: &str) -> bool {
        let h = Self::hash(text);
        self.last_sent_hash.map_or(true, |prev| prev != h)
    }

    pub fn mark_sent(&mut self, text: &str) {
        self.last_sent_hash = Some(Self::hash(text));
    }

    pub fn store_pending(&mut self, text: String) {
        self.pending = Some(text);
    }

    pub fn take_pending(&mut self) -> Option<String> {
        self.pending.take()
    }
}

impl Default for SyncEngine {
    fn default() -> Self {
        Self::new()
    }
}
```

- [ ] **Step 2: Write tests**

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_first_text_should_send() {
        let engine = SyncEngine::new();
        assert!(engine.should_send("hello"));
    }

    #[test]
    fn test_duplicate_should_not_send() {
        let mut engine = SyncEngine::new();
        assert!(engine.should_send("hello"));
        engine.mark_sent("hello");
        assert!(!engine.should_send("hello"));
    }

    #[test]
    fn test_different_text_should_send() {
        let mut engine = SyncEngine::new();
        engine.mark_sent("hello");
        assert!(engine.should_send("world"));
    }

    #[test]
    fn test_pending_buffer() {
        let mut engine = SyncEngine::new();
        assert!(engine.take_pending().is_none());
        engine.store_pending("offline text".into());
        assert_eq!(engine.take_pending(), Some("offline text".into()));
        assert!(engine.take_pending().is_none());
    }

    #[test]
    fn test_pending_only_keeps_latest() {
        let mut engine = SyncEngine::new();
        engine.store_pending("first".into());
        engine.store_pending("second".into());
        assert_eq!(engine.take_pending(), Some("second".into()));
    }
}
```

Run: `cargo test sync`
Expected: 5 tests PASS

- [ ] **Step 3: Add to main.rs**

```rust
mod clip;
mod config;
mod protocol;
mod sync;
```

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/src/sync.rs clipsync-pc/src/main.rs
git commit -m "feat: add sync engine with hash dedup and pending buffer"
```

---

### Task 6: Main Event Loop — Wire Everything

**Files:**
- Modify: `clipsync-pc/src/main.rs`

**Interfaces:**
- Consumes: All modules above
- Produces: Runnable binary that listens to clipboard changes, applies debounce, prints to console

- [ ] **Step 1: Write the full main.rs with debounce event loop**

```rust
mod clip;
mod config;
mod protocol;
mod sync;

use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!(
        "ClipSync PC starting... port={} debounce={}ms",
        cfg.connection.port,
        cfg.clipboard.debounce_ms
    );

    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();

    // Spawn clipboard listener in background thread
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);

    log::info!("Listening for clipboard changes...");

    loop {
        // Wait for first clipboard change
        let Some(mut text) = clip_rx.recv().await else {
            break;
        };

        // Debounce: keep resetting timer on new events within the window
        loop {
            tokio::select! {
                _ = tokio::time::sleep(debounce) => {
                    // Debounce period elapsed, process this text
                    break;
                }
                Some(new_text) = clip_rx.recv() => {
                    text = new_text;
                    // Continue inner loop (timer resets)
                }
            }
        }

        // After debounce, check if this is new content
        if engine.should_send(&text) {
            log::info!("Clipboard changed: {} chars", text.len());
            engine.mark_sent(&text);
            // Phase 1: just log it (Phase 2 will send via WebSocket)
            let msg = protocol::ClipMessage::push(text);
            log::info!("Would send: {}", msg.to_json());
        }
    }

    Ok(())
}
```

- [ ] **Step 2: Run and manually test**

Run: `cargo run`
Expected:
- Logs "ClipSync PC starting..."
- Logs "Listening for clipboard changes..."
- Copy some text → see "Clipboard changed: N chars"
- Copy same text again → no duplicate log
- Copy different text quickly → only the last one fires (debounce)

Ctrl+C to stop.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/main.rs
git commit -m "feat: wire main event loop with clipboard listener and dedup debounce"
```

---

### Task 7: WebSocket Client + HMAC Handshake

**Files:**
- Create: `clipsync-pc/src/ws.rs`

**Interfaces:**
- Produces: `pub async fn connect_and_auth(uri: &str, secret: &str) -> anyhow::Result<WebSocketStream>`
- Produces: `pub async fn send(ws: &mut WebSocketStream, msg: ClipMessage) -> anyhow::Result<()>`
- Produces: `pub async fn recv(ws: &mut WebSocketStream) -> anyhow::Result<ClipMessage>`
- Type alias: `pub type WebSocketStream = tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>`

- [ ] **Step 1: Write the WebSocket module**

```rust
use crate::protocol::ClipMessage;
use futures_util::{SinkExt, StreamExt};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use tokio_tungstenite::connect_async;
use tokio_tungstenite::tungstenite::Message;

pub type WebSocketStream =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

pub async fn connect_and_auth(uri: &str, secret: &str) -> anyhow::Result<WebSocketStream> {
    let url = url::Url::parse(uri)?;
    let (mut ws, _response) = connect_async(url).await?;

    // Wait for hello with challenge
    let raw = ws.next().await.ok_or_else(|| anyhow::anyhow!("connection closed before hello"))??;
    let msg = match raw {
        Message::Text(t) => ClipMessage::from_json(&t)?,
        _ => anyhow::bail!("expected text message, got binary"),
    };

    let challenge = match msg {
        ClipMessage::Hello { challenge } => challenge,
        ClipMessage::AuthFail => anyhow::bail!("server rejected previous auth"),
        other => anyhow::bail!("expected hello, got {:?}", other),
    };

    // Compute HMAC-SHA256 response
    let mut mac = Hmac::<Sha256>::new_from_slice(secret.as_bytes())
        .map_err(|_| anyhow::anyhow!("invalid secret key"))?;
    mac.update(challenge.as_bytes());
    let response = hex::encode(mac.finalize().into_bytes());

    // Send auth response
    let auth_msg = ClipMessage::Auth { response };
    ws.send(Message::Text(auth_msg.to_json())).await?;

    // Wait for auth result
    let raw = ws.next().await.ok_or_else(|| anyhow::anyhow!("connection closed before auth result"))??;
    let msg = match raw {
        Message::Text(t) => ClipMessage::from_json(&t)?,
        _ => anyhow::bail!("expected text message"),
    };

    match msg {
        ClipMessage::AuthOk => {
            log::info!("HMAC authentication successful");
            Ok(ws)
        }
        ClipMessage::AuthFail => anyhow::bail!("authentication failed"),
        other => anyhow::bail!("expected auth_ok/auth_fail, got {:?}", other),
    }
}

pub async fn send(ws: &mut WebSocketStream, msg: &ClipMessage) -> anyhow::Result<()> {
    ws.send(Message::Text(msg.to_json())).await?;
    Ok(())
}

pub async fn recv(ws: &mut WebSocketStream) -> anyhow::Result<ClipMessage> {
    loop {
        let raw = ws
            .next()
            .await
            .ok_or_else(|| anyhow::anyhow!("connection closed"))??;
        match raw {
            Message::Text(t) => {
                if let Ok(msg) = ClipMessage::from_json(&t) {
                    return Ok(msg);
                }
                // Ignore unparseable messages
            }
            Message::Ping(data) => {
                let _ = ws.send(Message::Pong(data)).await;
            }
            Message::Close(_) => {
                anyhow::bail!("connection closed by peer");
            }
            _ => {}
        }
    }
}
```

- [ ] **Step 2: Add `url` dependency**

In `Cargo.toml`, add to `[dependencies]`:
```toml
url = "2"
```

- [ ] **Step 3: Add `mod ws;` to main.rs**

```rust
mod clip;
mod config;
mod protocol;
mod sync;
mod ws;
```

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/src/ws.rs clipsync-pc/Cargo.toml clipsync-pc/src/main.rs
git commit -m "feat: add WebSocket client with HMAC-SHA256 handshake"
```

---

### Task 8: mDNS Discovery Module

**Files:**
- Create: `clipsync-pc/src/mdns.rs`

**Interfaces:**
- Produces: `pub fn discover() -> anyhow::Result<tokio::sync::mpsc::UnboundedReceiver<String>>` — returns receiver of discovered IP:port strings, service type `_clipsync._tcp.local.`

- [ ] **Step 1: Write mDNS discovery**

```rust
use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::time::Duration;
use tokio::sync::mpsc;

const SERVICE_TYPE: &str = "_clipsync._tcp.local.";

pub fn discover() -> anyhow::Result<mpsc::UnboundedReceiver<String>> {
    let (tx, rx) = mpsc::unbounded_channel();
    let daemon = ServiceDaemon::new()?;

    let browser = daemon.browse(SERVICE_TYPE)?;

    std::thread::spawn(move || {
        loop {
            let event = browser.recv();
            match event {
                Ok(event) => match event {
                    ServiceEvent::ServiceResolved(info) => {
                        let addr = info.get_addresses().iter().next().cloned();
                        let port = info.get_port();
                        if let Some(addr) = addr {
                            let uri = format!("ws://{}:{}", addr, port);
                            log::info!("mDNS discovered: {}", uri);
                            let _ = tx.send(uri);
                        }
                    }
                    _ => {}
                },
                Err(e) => {
                    log::error!("mDNS browse error: {}", e);
                    break;
                }
            }
        }
    });

    std::thread::spawn(move || {
        // Keep daemon alive
        loop {
            std::thread::sleep(Duration::from_secs(30));
            let _ = daemon.browse(SERVICE_TYPE);
        }
    });

    Ok(rx)
}
```

- [ ] **Step 2: Add `mod mdns;` to main.rs**

```rust
mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod ws;
```

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/mdns.rs clipsync-pc/src/main.rs
git commit -m "feat: add mDNS discovery module"
```

---

### Task 9: Offline Pending Buffer Integration

**Files:**
- Modify: `clipsync-pc/src/sync.rs`

**Interfaces:**
- Modifies: `SyncEngine` — add `pub fn has_pending(&self) -> bool`
- Produces: Integration in main.rs: clipboard changes during disconnected state go to buffer, flushed on reconnect

- [ ] **Step 1: Add `has_pending` to SyncEngine**

In `sync.rs`, add method:
```rust
    pub fn has_pending(&self) -> bool {
        self.pending.is_some()
    }
```

- [ ] **Step 2: Add test for has_pending**

```rust
    #[test]
    fn test_has_pending() {
        let mut engine = SyncEngine::new();
        assert!(!engine.has_pending());
        engine.store_pending("test".into());
        assert!(engine.has_pending());
        engine.take_pending();
        assert!(!engine.has_pending());
    }
```

Run: `cargo test sync`
Expected: 6 tests PASS (5 existing + 1 new)

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/sync.rs
git commit -m "feat: add has_pending to sync engine"
```

---

### Task 10: Full Sync Loop — Network + Clipboard Integration

**Files:**
- Modify: `clipsync-pc/src/main.rs`

**Interfaces:**
- Consumes: All modules
- Produces: Full working sync: mDNS discover → connect → HMAC auth → bidirectional clipboard sync with pending buffer

- [ ] **Step 1: Write integrated main.rs with full state machine**

```rust
mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod ws;

use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ConnState {
    Disconnected,
    Connecting,
    Connected,
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!(
        "ClipSync PC starting... port={}",
        cfg.connection.port
    );

    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);

    // Main event loop
    loop {
        log::info!("State: Disconnected — starting mDNS discovery");
        let Ok(mut mdns_rx) = mdns::discover() else {
            log::error!("mDNS discovery failed, retrying in 5s");
            tokio::time::sleep(Duration::from_secs(5)).await;
            continue;
        };

        // Wait for mDNS discovery or clipboard changes
        let uri = loop {
            tokio::select! {
                Some(uri) = mdns_rx.recv() => {
                    break uri;
                }
                Some(text) = clip_rx.recv() => {
                    engine.store_pending(text);
                    log::info!("Offline: stored pending clipboard change");
                }
                _ = tokio::time::sleep(Duration::from_secs(5)) => {
                    log::info!("Still waiting for mDNS discovery...");
                }
            }
        };

        // Connect loop with exponential backoff
        let mut backoff = 1u64;
        let max_backoff = 60u64;

        let mut ws = loop {
            log::info!("Connecting to {} (attempt, backoff={}s)...", uri, backoff);

            if cfg.auth.secret.is_empty() {
                log::warn!("No auth secret configured, skipping authentication");
            }

            match ws::connect_and_auth(&uri, &cfg.auth.secret).await {
                Ok(ws) => {
                    log::info!("Connected and authenticated!");
                    break ws;
                }
                Err(e) => {
                    log::error!("Connection failed: {}", e);
                    log::info!("Retrying in {}s...", backoff);
                    tokio::time::sleep(Duration::from_secs(backoff)).await;
                    backoff = (backoff * 2).min(max_backoff);
                }
            }
        };

        backoff = 1; // reset for next disconnect
        log::info!("State: Connected — flushing pending buffer");

        // Flush pending offline clipboard change
        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text);
            if let Err(e) = ws::send(&mut ws, &msg).await {
                log::error!("Failed to flush pending: {}", e);
            } else {
                log::info!("Flushed pending clipboard to phone");
            }
        }

        // Connected sync loop
        let mut debounce_timer: Option<tokio::time::Sleep> = None;
        let mut debounce_text: Option<String> = None;

        loop {
            tokio::select! {
                // Local clipboard change (debounced)
                Some(text) = clip_rx.recv(), if debounce_timer.is_none() => {
                    debounce_text = Some(text);
                    debounce_timer = Some(tokio::time::sleep(debounce));
                }
                Some(text) = clip_rx.recv() => {
                    // Reset debounce timer with new text
                    debounce_text = Some(text);
                    debounce_timer = Some(tokio::time::sleep(debounce));
                }
                _ = async { debounce_timer.as_mut().unwrap().await }, if debounce_timer.is_some() => {
                    // Debounce elapsed
                    let text = debounce_timer.take().and(debounce_text.take()).unwrap();
                    if engine.should_send(&text) {
                        log::info!("Sending to phone: {} chars", text.len());
                        let msg = protocol::ClipMessage::set(text.clone());
                        if let Err(e) = ws::send(&mut ws, &msg).await {
                            log::error!("Send failed: {}", e);
                            break; // exit inner loop, reconnect
                        }
                        engine.mark_sent(&text);
                    }
                }
                // Receive from phone
                msg = ws::recv(&mut ws) => {
                    match msg {
                        Ok(protocol::ClipMessage::Push { text, ts: _ }) => {
                            log::info!("Received from phone: {} chars", text.len());
                            clip::write(&text);
                            engine.mark_sent(&text);
                        }
                        Ok(protocol::ClipMessage::Ping) => {
                            let _ = ws::send(&mut ws, &protocol::ClipMessage::Pong).await;
                        }
                        Ok(protocol::ClipMessage::Pong) => {
                            // heartbeat reply, nothing to do
                        }
                        Err(e) => {
                            log::error!("Connection lost: {}", e);
                            break; // reconnect
                        }
                        _ => {}
                    }
                }
            }
        }

        log::info!("State: Disconnected");
    }
}
```

- [ ] **Step 2: Verify compile**

Run: `cargo check`
Expected: Compiles without errors

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/main.rs
git commit -m "feat: integrate full sync loop with mDNS, WS, dedup, and pending buffer"
```

---

### Task 11: System Tray with Status Icons

**Files:**
- Create: `clipsync-pc/src/tray.rs`

**Interfaces:**
- Produces: `pub struct Tray { … }` with:
  - `pub fn new(event_loop: &winit::event_loop::ActiveEventLoop) -> anyhow::Result<Self>`
  - `pub fn update_state(&mut self, state: ConnState)`
  - `pub fn event_handler(&mut self, event: TrayIconEvent) -> Option<TrayAction>`
- Produces: Enum `TrayAction { Reconnect, TogglePause, Quit }`
- Produces: Enum `ConnState` (potentially moved from main.rs)

- [ ] **Step 1: Generate icon data (we'll use simple colored PNGs embedded as base64)**

We need small PNG files for the tray. Since we can't include actual PNG files in the plan text easily, we'll use `include_bytes!` with files created by the developer. But for the plan, let's describe the approach: use `tray-icon` with embedded RGBA pixels.

Write `clipsync-pc/src/tray.rs`:

```rust
use std::sync::mpsc;
use tray_icon::{
    menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem},
    Icon, TrayIcon, TrayIconBuilder, TrayIconEvent,
};
use winit::event_loop::ActiveEventLoop;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Disconnected,
    Connecting,
    Connected,
}

#[derive(Debug)]
pub enum TrayAction {
    Reconnect,
    TogglePause,
    Quit,
}

pub struct Tray {
    tray_icon: TrayIcon,
    menu_rx: mpsc::Receiver<TrayAction>,
    _menu: Menu<MenuItem>,
}

// Generate simple colored circle icons as RGBA pixel buffers
fn make_icon_data(r: u8, g: u8, b: u8) -> Vec<u8> {
    let size = 32;
    let radius = 14.0;
    let center = size as f64 / 2.0;
    let mut data = Vec::with_capacity(size * size * 4);

    for y in 0..size {
        for x in 0..size {
            let dx = x as f64 - center;
            let dy = y as f64 - center;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist <= radius {
                data.push(r); // R
                data.push(g); // G
                data.push(b); // B
                data.push(255); // A
            } else {
                data.push(0);
                data.push(0);
                data.push(0);
                data.push(0);
            }
        }
    }
    data
}

fn icon_for_state(state: ConnState) -> Icon {
    let (r, g, b) = match state {
        ConnState::Connected => (0, 200, 0),    // green
        ConnState::Connecting => (200, 200, 0),  // yellow
        ConnState::Disconnected => (128, 128, 128), // grey
    };

    let rgba = make_icon_data(r, g, b);
    Icon::from_rgba(rgba, 32, 32).expect("failed to create icon")
}

impl Tray {
    pub fn new(event_loop: &ActiveEventLoop) -> anyhow::Result<Self> {
        let menu = Menu::new();
        let reconnect_item = MenuItem::new("Reconnect", true, None);
        let pause_item = MenuItem::new("Pause Sync", true, None);
        let separator = PredefinedMenuItem::separator();
        let quit_item = MenuItem::new("Quit", true, None);

        menu.append(&reconnect_item)?;
        menu.append(&pause_item)?;
        menu.append(&separator)?;
        menu.append(&quit_item)?;

        let icon = icon_for_state(ConnState::Disconnected);
        let tray_icon = TrayIconBuilder::new()
            .with_menu(Box::new(menu.clone()))
            .with_icon(icon)
            .with_tooltip("ClipSync · Not connected")
            .build(event_loop)?;

        let (menu_tx, menu_rx) = mpsc::channel();

        // Spawn handler for menu events
        let reconnect_id = reconnect_item.id().clone();
        let pause_id = pause_item.id().clone();
        let quit_id = quit_item.id().clone();

        std::thread::spawn(move || {
            let menu_events = MenuEvent::receiver();
            loop {
                if let Ok(event) = menu_events.recv() {
                    let action = if event.id == reconnect_id {
                        Some(TrayAction::Reconnect)
                    } else if event.id == pause_id {
                        Some(TrayAction::TogglePause)
                    } else if event.id == quit_id {
                        Some(TrayAction::Quit)
                    } else {
                        None
                    };
                    if let Some(action) = action {
                        let _ = menu_tx.send(action);
                    }
                }
            }
        });

        Ok(Self {
            tray_icon,
            menu_rx,
            _menu: menu,
        })
    }

    pub fn update_state(&mut self, state: ConnState) {
        let icon = icon_for_state(state);
        let tooltip = match state {
            ConnState::Connected => "ClipSync · Connected",
            ConnState::Connecting => "ClipSync · Connecting…",
            ConnState::Disconnected => "ClipSync · Not connected",
        };
        let _ = self.tray_icon.set_icon(Some(icon));
        let _ = self.tray_icon.set_tooltip(Some(tooltip.into()));
    }

    pub fn try_recv_action(&self) -> Option<TrayAction> {
        self.menu_rx.try_recv().ok()
    }

    pub fn tray_event_handler(&self, event: TrayIconEvent) {
        // TrayIconEvent handling if needed (e.g., left-click)
        let _ = event;
    }
}
```

- [ ] **Step 2: Add `mod tray;` to main.rs**

```rust
mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod tray;
mod ws;
```

- [ ] **Step 3: Verify compile**

Run: `cargo check`
Expected: Compiles successfully

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/src/tray.rs clipsync-pc/src/main.rs
git commit -m "feat: add system tray with colored status icons and right-click menu"
```

---

### Task 12: Tray Integration with Main Loop

**Files:**
- Modify: `clipsync-pc/src/main.rs`

**Interfaces:**
- Rewrites main.rs to use winit event loop with tray, tokio on separate thread

- [ ] **Step 1: Rewrite main.rs — winit-driven event loop with tokio in background**

```rust
mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod tray;
mod ws;

use std::sync::Arc;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tokio::sync::Mutex;
use tray::{ConnState, Tray, TrayAction};
use winit::application::ApplicationHandler;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};

#[derive(Debug, Clone)]
enum AppCommand {
    SetState(ConnState),
    ClipChanged(String),
    PhonePush { text: String },
    Quit,
}

struct App {
    tray: Option<Tray>,
    cmd_tx: mpsc::UnboundedSender<AppCommand>,
    state: ConnState,
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.tray.is_none() {
            match Tray::new(event_loop) {
                Ok(tray) => {
                    self.tray = Some(tray);
                }
                Err(e) => {
                    log::error!("Failed to create tray: {}", e);
                    event_loop.exit();
                    return;
                }
            }
        }
    }

    fn window_event(
        &mut self,
        _event_loop: &ActiveEventLoop,
        _window_id: winit::window::WindowId,
        _event: winit::event::WindowEvent,
    ) {
    }

    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        // Check tray menu actions
        if let Some(ref tray) = self.tray {
            match tray.try_recv_action() {
                Some(TrayAction::Reconnect) => {
                    log::info!("Manual reconnect requested");
                    // Signal reconnection to sync thread
                }
                Some(TrayAction::TogglePause) => {
                    log::info!("Pause/Resume toggled");
                }
                Some(TrayAction::Quit) => {
                    _event_loop.exit();
                    return;
                }
                None => {}
            }
        }
    }
}

fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);

    let event_loop = EventLoop::new()?;
    event_loop.set_control_flow(ControlFlow::Poll);

    let mut app = App {
        tray: None,
        cmd_tx: mpsc::unbounded_channel().0,
        state: ConnState::Disconnected,
    };

    event_loop.run_app(&mut app)?;
    Ok(())
}
```

Hmm, this is getting complex. The winit + tokio integration requires careful design. Let me think about this differently.

The `tray-icon` crate itself uses winit. The proper way to integrate tokio with winit is:

1. w尼t main thread runs the event loop (ControlFlow::Poll or Wait)
2. tokio runtime runs in a separate thread, communicates via mpsc channels
3. The winit event handler checks channels and updates UI

Let me rewrite this properly. Actually, given the complexity, let me make Task 12 a simpler version that just ports the existing sync loop into a background tokio thread, and the winit thread manages the tray.

For the plan, I'll present the actual working code. Let me write it carefully.

Actually, `tray-icon` 0.19 changed its API — it no longer takes `EventLoop` directly. Let me check... 

In tray-icon 0.19, `TrayIconBuilder::build()` takes an `&ActiveEventLoop`. So we need a winit event loop running.

Let me write a clean version:

```rust
fn main() -> anyhow::Result<()> {
    env_logger::init();
    let cfg = Arc::new(config::ClipSyncConfig::load("clipsync.toml")?);
    
    let event_loop = EventLoop::new()?;
    let proxy = event_loop.create_proxy();
    
    // Start the sync engine on a background thread with tokio
    let cfg_clone = cfg.clone();
    let proxy_clone = proxy.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().unwrap();
        rt.block_on(async {
            run_sync_loop(cfg_clone, proxy_clone).await;
        });
    });
    
    let mut app = App { tray: None, state: ConnState::Disconnected };
    event_loop.set_control_flow(ControlFlow::Poll);
    event_loop.run_app(&mut app)?;
    Ok(())
}
```

And the sync loop sends `AppCommand` via `EventLoopProxy`. But this depends on having a custom event type for the event loop.

OK, actually for the plan I think a cleaner approach is: just use `std::sync::mpsc` between threads and poll in `about_to_wait`. The `tray-icon` crate itself handles winit internally.

Actually, let me look at this from a different angle. The simplest approach:

1. Main thread: tokio runtime, runs sync loop
2. Spawned thread: creates winit event loop + tray, polls for menu events
3. Communication: std::sync::mpsc channels

But `tray-icon` needs the winit event loop to be running on the current thread and the loop needs to pump. So we need:

1. Main thread: winit event loop + tray
2. Background thread: tokio runtime + sync engine
3. Communication: `EventLoopProxy` for sending custom events to the winit thread, and `std::sync::mpsc` for sending TrayAction from tray thread to main

But the issue is that `MenuEvent::receiver()` is per-menu. Let me use `MenuEvent::receiver()` on the main thread.

Actually, let me simplify significantly. For the plan, Task 12 should produce a working tray integration. Let me write a clean version.

In winit 0.30, `EventLoop::run_app` takes an `impl ApplicationHandler`. So I need to implement that trait.

```rust
use winit::event_loop::EventLoop;
use winit::application::ApplicationHandler;
use winit::event_loop::ActiveEventLoop;

struct App {
    tray: Option<Tray>,
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        self.tray = Some(Tray::new(event_loop).unwrap());
    }
    
    fn window_event(&mut self, _: &ActiveEventLoop, _: winit::window::WindowId, _: winit::event::WindowEvent) {}
    
    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        // check menu events
    }
}
```

But then how does the sync loop get called? It runs in a background thread. Communication from sync thread → main thread uses `EventLoopProxy`.

OK this is getting complicated in the plan. Let me write a simpler but correct version. The approach:

1. Main thread spawns tokio on a separate thread
2. Main thread creates winit event loop + tray  
3. Tokio thread sends status updates to main thread via `EventLoopProxy`
4. Main thread polls tray menu events and sends actions to tokio thread via `std::sync::mpsc`

Let me write the code:

```rust
use std::sync::mpsc as std_mpsc;
use std::sync::Arc;
use winit::event_loop::{EventLoop, EventLoopProxy};

enum UserEvent {
    StateChanged(ConnState),
}

fn main() -> anyhow::Result<()> {
    env_logger::init();
    let cfg = Arc::new(config::ClipSyncConfig::load("clipsync.toml")?);
    
    let event_loop = EventLoop::<UserEvent>::with_user_event().build()?;
    let proxy = event_loop.create_proxy();
    
    let (action_tx, action_rx) = std_mpsc::channel::<TrayAction>();
    
    // Tokio sync loop in background thread
    let cfg_clone = cfg.clone();
    let proxy_clone = proxy.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().unwrap();
        rt.block_on(async move {
            run_sync_loop(cfg_clone, proxy_clone, action_rx).await;
        });
    });
    
    let mut app = App::new(action_tx);
    event_loop.set_control_flow(ControlFlow::Poll);
    event_loop.run_app(&mut app)?;
    Ok(())
}

struct App {
    tray: Option<Tray>,
    action_tx: std_mpsc::Sender<TrayAction>,
}

impl App {
    fn new(action_tx: std_mpsc::Sender<TrayAction>) -> Self {
        Self { tray: None, action_tx }
    }
}

impl ApplicationHandler<UserEvent> for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.tray.is_none() {
            self.tray = Some(Tray::new(event_loop).unwrap());
        }
    }
    
    fn window_event(&mut self, _: &ActiveEventLoop, _: winit::window::WindowId, _: winit::event::WindowEvent) {}
    
    fn user_event(&mut self, _event_loop: &ActiveEventLoop, event: UserEvent) {
        match event {
            UserEvent::StateChanged(state) => {
                if let Some(ref mut tray) = self.tray {
                    tray.update_state(state);
                }
            }
        }
    }
    
    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        if let Some(ref tray) = self.tray {
            if let Some(action) = tray.try_recv_action() {
                let _ = self.action_tx.send(action);
            }
        }
    }
}
```

And `run_sync_loop` would be the existing sync logic, but it receives `TrayAction` from the channel and sends `UserEvent::StateChanged` via the proxy.

This is getting complex. Let me simplify the plan by breaking it into clearer steps. I'll keep Task 12 focused on just the tray integration and keep the main.rs in a relatively concise form.

Let me write it clearly now. I'll present the full main.rs rewrite.<｜end▁of▁thinking｜>

<｜｜DSML｜｜tool_calls>
<｜｜DSML｜｜invoke name="write">
<｜｜DSML｜｜parameter name="content" string="true"># ClipSync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a cross-device clipboard sync system: Rust PC client (Windows) with WebSocket + mDNS + system tray, plus a C Android daemon (KernelSU module) with Binder IPC clipboard monitoring.

**Architecture:** Monorepo with `clipsync-pc/` (Rust bin crate) and `clipsync-daemon/` (C, NDK). Both communicate via JSON-over-WebSocket with HMAC-SHA256 authentication. No shared code — the protocol spec is the interface contract.

**Tech Stack:** Rust: tokio, tokio-tungstenite, mdns-sd, tray-icon, winit, toml, serde, serde_json, blake3, hmac, sha2, hex, windows. C: libbinder_ndk, mongoose.

## Global Constraints

- Port: 5287 (hardcoded default)
- Debounce: 300ms (configurable)
- HMAC auth: SHA-256 with pre-shared secret key from `clipsync.toml`
- Platform: Windows only for initial PC client (Phase 1-3)
- No shell commands for Android clipboard — direct Binder IPC only
- Pending buffer: stores only latest clipboard change during offline, flushes on reconnect
- Echo prevention: `AtomicBool` flag before `SetClipboardData`, checked in clipboard listener
- Reconnect: exponential backoff 1s→2s→4s→8s… capped 60s, re-scan mDNS before each attempt
- mDNS service type: `_clipsync._tcp.local.`
- Config file: `clipsync.toml` beside the binary

---

### Task 1: Project Scaffolding

**Files:**
- Create: `clipsync-pc/Cargo.toml`
- Create: `clipsync-pc/src/main.rs`

**Interfaces:**
- Produces: `main.rs` with `#[tokio::main]` entry point — compile-only, no logic yet

- [ ] **Step 1: Create Cargo.toml with all dependencies**

```toml
[package]
name = "clipsync-pc"
version = "0.1.0"
edition = "2021"

[dependencies]
tokio = { version = "1", features = ["full"] }
tokio-tungstenite = { version = "0.24", features = ["native-tls"] }
futures-util = "0.3"
mdns-sd = "0.14"
tray-icon = "0.19"
winit = "0.30"
toml = "0.8"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
blake3 = "1"
hmac = "0.12"
sha2 = "0.10"
hex = "0.4"
url = "2"
windows = { version = "0.58", features = [
    "Win32_System_DataExchange",
    "Win32_System_Ole",
    "Win32_UI_WindowsAndMessaging",
    "Win32_System_Threading",
    "Win32_Foundation",
    "Win32_System_Memory",
]}
anyhow = "1"
thiserror = "2"
log = "0.4"
env_logger = "0.11"
```

- [ ] **Step 2: Create minimal main.rs**

```rust
fn main() -> anyhow::Result<()> {
    env_logger::init();
    log::info!("ClipSync PC starting...");
    Ok(())
}
```

- [ ] **Step 3: Verify compilation**

```bash
cargo check
```
Expected: Compiles successfully.

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/Cargo.toml clipsync-pc/src/main.rs
git commit -m "feat: add clipsync-pc project scaffolding"
```

---

### Task 2: Config Module

**Files:**
- Create: `clipsync-pc/src/config.rs`

**Interfaces:**
- Produces: `ClipSyncConfig::load(P: AsRef<Path>) -> anyhow::Result<Self>`
- Produces: Struct with fields `connection: ConnectionConfig`, `auth: AuthConfig`, `clipboard: ClipboardConfig`

- [ ] **Step 1: Write the config module**

```rust
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipSyncConfig {
    #[serde(default)]
    pub connection: ConnectionConfig,
    #[serde(default)]
    pub auth: AuthConfig,
    #[serde(default)]
    pub clipboard: ClipboardConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConnectionConfig {
    #[serde(default = "default_port")]
    pub port: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AuthConfig {
    #[serde(default)]
    pub secret: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClipboardConfig {
    #[serde(default = "default_debounce_ms")]
    pub debounce_ms: u64,
}

fn default_port() -> u16 { 5287 }
fn default_debounce_ms() -> u64 { 300 }

impl Default for ClipSyncConfig {
    fn default() -> Self {
        Self {
            connection: ConnectionConfig::default(),
            auth: AuthConfig::default(),
            clipboard: ClipboardConfig::default(),
        }
    }
}

impl Default for ConnectionConfig {
    fn default() -> Self {
        Self { port: default_port() }
    }
}

impl Default for AuthConfig {
    fn default() -> Self {
        Self { secret: String::new() }
    }
}

impl Default for ClipboardConfig {
    fn default() -> Self {
        Self { debounce_ms: default_debounce_ms() }
    }
}

impl ClipSyncConfig {
    pub fn load(path: impl AsRef<Path>) -> anyhow::Result<Self> {
        let path = path.as_ref();
        if path.exists() {
            let content = std::fs::read_to_string(path)?;
            Ok(toml::from_str(&content)?)
        } else {
            Ok(Self::default())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let cfg = ClipSyncConfig::default();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
        assert!(cfg.auth.secret.is_empty());
    }

    #[test]
    fn test_parse_toml() {
        let toml_str = r#"
[connection]
port = 9999

[auth]
secret = "test-key"

[clipboard]
debounce_ms = 500
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.connection.port, 9999);
        assert_eq!(cfg.auth.secret, "test-key");
        assert_eq!(cfg.clipboard.debounce_ms, 500);
    }

    #[test]
    fn test_partial_config() {
        let toml_str = r#"
[auth]
secret = "only-secret"
"#;
        let cfg: ClipSyncConfig = toml::from_str(toml_str).unwrap();
        assert_eq!(cfg.connection.port, 5287);
        assert_eq!(cfg.clipboard.debounce_ms, 300);
    }
}
```

- [ ] **Step 2: Add `mod config;` and test**

In `main.rs`:
```rust
mod config;

use config::ClipSyncConfig;

fn main() -> anyhow::Result<()> {
    env_logger::init();
    let cfg = ClipSyncConfig::load("clipsync.toml")?;
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);
    Ok(())
}
```

Run: `cargo test config`
Expected: 3 tests PASS.

- [ ] **Step 3: Create example config**

Create `clipsync-pc/clipsync.toml.example`:

```toml
[connection]
port = 5287

[auth]
secret = "your-pre-shared-key-here"

[clipboard]
debounce_ms = 300
```

- [ ] **Step 4: Commit**

```bash
git add clipsync-pc/src/config.rs clipsync-pc/src/main.rs clipsync-pc/clipsync.toml.example
git commit -m "feat: add config module with toml parsing"
```

---

### Task 3: Protocol Module

**Files:**
- Create: `clipsync-pc/src/protocol.rs`

**Interfaces:**
- Produces: Enum `ClipMessage` — all wire message variants (Push, Set, Ping, Pong, Hello, Auth, AuthOk, AuthFail)
- Produces: `ClipMessage::to_json() -> String`
- Produces: `ClipMessage::from_json(json: &str) -> serde_json::Result<Self>`
- Produces: `now_millis() -> u64` — current unix timestamp in milliseconds

- [ ] **Step 1: Write the protocol module**

```rust
use serde::{Deserialize, Serialize};
use std::time::SystemTime;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ClipMessage {
    #[serde(rename = "clipboard_push")]
    Push { text: String, ts: u64 },
    #[serde(rename = "clipboard_set")]
    Set { text: String, ts: u64 },
    #[serde(rename = "ping")]
    Ping,
    #[serde(rename = "pong")]
    Pong,
    #[serde(rename = "hello")]
    Hello { challenge: String },
    #[serde(rename = "auth")]
    Auth { response: String },
    #[serde(rename = "auth_ok")]
    AuthOk,
    #[serde(rename = "auth_fail")]
    AuthFail,
}

pub fn now_millis() -> u64 {
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

impl ClipMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).unwrap()
    }

    pub fn from_json(json: &str) -> serde_json::Result<Self> {
        serde_json::from_str(json)
    }

    pub fn push(text: String) -> Self {
        ClipMessage::Push { text, ts: now_millis() }
    }

    pub fn set(text: String) -> Self {
        ClipMessage::Set { text, ts: now_millis() }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_push_roundtrip() {
        let msg = ClipMessage::Push { text: "hello".into(), ts: 1719859200 };
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Push { text, ts } => {
                assert_eq!(text, "hello");
                assert_eq!(ts, 1719859200);
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_hello_roundtrip() {
        let msg = ClipMessage::Hello { challenge: "abc123".into() };
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        match decoded {
            ClipMessage::Hello { challenge } => {
                assert_eq!(challenge, "abc123");
            }
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn test_ping_roundtrip() {
        let msg = ClipMessage::Ping;
        let json = msg.to_json();
        let decoded = ClipMessage::from_json(&json).unwrap();
        assert!(matches!(decoded, ClipMessage::Ping));
    }

    #[test]
    fn test_push_json_format() {
        let msg = ClipMessage::Push { text: "test".into(), ts: 123 };
        let json = msg.to_json();
        assert!(json.contains("\"type\":\"clipboard_push\""));
        assert!(json.contains("\"text\":\"test\""));
    }
}
```

- [ ] **Step 2: Add `mod protocol;` and run tests**

In `main.rs`, add line:
```rust
mod protocol;
```

Run: `cargo test protocol`
Expected: 4 tests PASS.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/protocol.rs clipsync-pc/src/main.rs
git commit -m "feat: add protocol module with JSON message types"
```

---

### Task 4: Win32 Clipboard Module

**Files:**
- Create: `clipsync-pc/src/clip.rs`

**Interfaces:**
- Produces: `pub fn read() -> Option<String>` — read clipboard Unicode text
- Produces: `pub fn write(text: &str) -> bool` — write text to clipboard, returns success
- Produces: `pub struct ClipListener` — `new(tx)` + `spawn(self)` spawns hidden HWND thread, sends clipboard changes via mpsc
- Produces: AtomicBool echo prevention flags: `set_self_writing(v: bool)` / `is_self_writing() -> bool`

- [ ] **Step 1: Write the clipboard module**

```rust
use std::sync::atomic::{AtomicBool, Ordering};
use tokio::sync::mpsc;
use windows::core::PCWSTR;
use windows::Win32::Foundation::*;
use windows::Win32::System::DataExchange::*;
use windows::Win32::System::Memory::*;
use windows::Win32::System::Ole::*;
use windows::Win32::System::Threading::*;
use windows::Win32::UI::WindowsAndMessaging::*;

static SELF_WRITING: AtomicBool = AtomicBool::new(false);

pub fn set_self_writing(v: bool) {
    SELF_WRITING.store(v, Ordering::SeqCst);
}

pub fn is_self_writing() -> bool {
    SELF_WRITING.load(Ordering::SeqCst)
}

pub fn read() -> Option<String> {
    unsafe {
        if OpenClipboard(None).is_err() {
            return None;
        }
        let result = read_inner();
        let _ = CloseClipboard();
        result
    }
}

unsafe fn read_inner() -> Option<String> {
    let handle = GetClipboardData(CF_UNICODETEXT.0 as u32);
    if handle.is_invalid() {
        return None;
    }
    let ptr = GlobalLock(handle.0) as *const u16;
    if ptr.is_null() {
        return None;
    }
    let len = (0..).take_while(|&i| *ptr.add(i) != 0).count();
    let slice = std::slice::from_raw_parts(ptr, len);
    let text = String::from_utf16_lossy(slice);
    let _ = GlobalUnlock(handle.0);
    Some(text)
}

pub fn write(text: &str) -> bool {
    unsafe {
        set_self_writing(true);
        if OpenClipboard(None).is_err() {
            set_self_writing(false);
            return false;
        }
        let _ = EmptyClipboard();

        let wide: Vec<u16> = text
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();
        let byte_size = wide.len() * std::mem::size_of::<u16>();
        let hmem = GlobalAlloc(GMEM_MOVEABLE, byte_size);
        if hmem.is_invalid() {
            let _ = CloseClipboard();
            set_self_writing(false);
            return false;
        }
        let dst = GlobalLock(hmem.0) as *mut u16;
        if !dst.is_null() {
            std::ptr::copy_nonoverlapping(wide.as_ptr(), dst, wide.len());
            let _ = GlobalUnlock(hmem.0);
        }
        let result = SetClipboardData(CF_UNICODETEXT.0 as u32, HANDLE(hmem.0));
        let _ = CloseClipboard();
        let ok = !result.is_invalid();
        // fall through to clear flag
        set_self_writing(false);
        ok
    }
}

pub struct ClipListener {
    tx: mpsc::UnboundedSender<String>,
}

impl ClipListener {
    pub fn new(tx: mpsc::UnboundedSender<String>) -> Self {
        Self { tx }
    }

    pub fn spawn(self) {
        std::thread::spawn(move || unsafe { message_loop(self.tx) });
    }
}

static mut TX_PTR: Option<mpsc::UnboundedSender<String>> = None;

unsafe fn message_loop(tx: mpsc::UnboundedSender<String>) {
    TX_PTR = Some(tx);

    let hinstance = GetModuleHandleW(None).unwrap();
    let class_name = PCWSTR::from_raw(wide!("ClipSyncClipListener").as_ptr());

    let wc = WNDCLASSW {
        lpfnWndProc: Some(wndproc),
        hInstance: hinstance.into(),
        lpszClassName: class_name,
        ..Default::default()
    };

    if RegisterClassW(&wc) == 0 {
        log::error!("RegisterClassW failed");
        return;
    }

    let hwnd = CreateWindowExW(
        WINDOW_EX_STYLE::default(),
        class_name,
        windows::core::w!("ClipSync"),
        WINDOW_STYLE::default(),
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        None,
        None,
        hinstance,
        None,
    );

    if hwnd.is_invalid() {
        log::error!("CreateWindowExW failed");
        return;
    }

    if !AddClipboardFormatListener(hwnd).as_bool() {
        log::error!("AddClipboardFormatListener failed");
        return;
    }

    let mut msg = MSG::default();
    loop {
        let ret = GetMessageW(&mut msg, None, 0, 0);
        if ret.0 <= 0 {
            break;
        }
        let _ = TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    let _ = RemoveClipboardFormatListener(hwnd);
    let _ = DestroyWindow(hwnd);
}

unsafe extern "system" fn wndproc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match msg {
        WM_CLIPBOARDUPDATE => {
            if !SELF_WRITING.load(Ordering::SeqCst) {
                if let Some(ref tx) = TX_PTR {
                    if let Some(text) = read() {
                        let _ = tx.send(text);
                    }
                }
            }
            LRESULT(0)
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            LRESULT(0)
        }
        _ => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_read_write_roundtrip() {
        let test_text = "ClipSync-test-hello";
        assert!(write(test_text));
        let read_back = read().unwrap();
        assert_eq!(read_back, test_text);
    }

    #[test]
    fn test_write_clears_self_writing_flag() {
        write("test");
        assert!(!is_self_writing());
    }
}
```

- [ ] **Step 2: Add `mod clip;` and run tests**

In `main.rs`, add:
```rust
mod clip;
```

Run: `cargo test clip`
Expected: 2 tests PASS.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/clip.rs clipsync-pc/src/main.rs
git commit -m "feat: add Win32 clipboard read/write/listen module"
```

---

### Task 5: Sync Engine (dedup + pending buffer)

**Files:**
- Create: `clipsync-pc/src/sync.rs`

**Interfaces:**
- Produces: `pub struct SyncEngine` with `new()`, `should_send(&self, text: &str) -> bool`, `mark_sent(&mut self, text: &str)`, `store_pending(&mut self, text: String)`, `take_pending(&mut self) -> Option<String>`, `has_pending(&self) -> bool`

- [ ] **Step 1: Write the sync engine**

```rust
use blake3::Hash;

pub struct SyncEngine {
    last_sent_hash: Option<Hash>,
    pending: Option<String>,
}

impl SyncEngine {
    pub fn new() -> Self {
        Self {
            last_sent_hash: None,
            pending: None,
        }
    }

    fn hash(text: &str) -> Hash {
        blake3::hash(text.as_bytes())
    }

    pub fn should_send(&self, text: &str) -> bool {
        let h = Self::hash(text);
        self.last_sent_hash.map_or(true, |prev| prev != h)
    }

    pub fn mark_sent(&mut self, text: &str) {
        self.last_sent_hash = Some(Self::hash(text));
    }

    pub fn store_pending(&mut self, text: String) {
        self.pending = Some(text);
    }

    pub fn take_pending(&mut self) -> Option<String> {
        self.pending.take()
    }

    pub fn has_pending(&self) -> bool {
        self.pending.is_some()
    }
}

impl Default for SyncEngine {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_first_text_always_sends() {
        let engine = SyncEngine::new();
        assert!(engine.should_send("hello"));
    }

    #[test]
    fn test_duplicate_is_skipped() {
        let mut engine = SyncEngine::new();
        engine.mark_sent("hello");
        assert!(!engine.should_send("hello"));
    }

    #[test]
    fn test_different_text_sends() {
        let mut engine = SyncEngine::new();
        engine.mark_sent("hello");
        assert!(engine.should_send("world"));
    }

    #[test]
    fn test_pending_empty_by_default() {
        let engine = SyncEngine::new();
        assert!(!engine.has_pending());
    }

    #[test]
    fn test_pending_store_and_take() {
        let mut engine = SyncEngine::new();
        engine.store_pending("offline text".into());
        assert!(engine.has_pending());
        assert_eq!(engine.take_pending(), Some("offline text".into()));
        assert!(!engine.has_pending());
        assert!(engine.take_pending().is_none());
    }

    #[test]
    fn test_pending_only_keeps_latest() {
        let mut engine = SyncEngine::new();
        engine.store_pending("first".into());
        engine.store_pending("second".into());
        assert_eq!(engine.take_pending(), Some("second".into()));
    }
}
```

- [ ] **Step 2: Add `mod sync;` and run tests**

In `main.rs`, add:
```rust
mod sync;
```

Run: `cargo test sync`
Expected: 6 tests PASS.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/sync.rs clipsync-pc/src/main.rs
git commit -m "feat: add sync engine with hash dedup and pending buffer"
```

---

### Task 6: Main Event Loop — Console Mode (Phase 1 MVP)

**Files:**
- Modify: `clipsync-pc/src/main.rs`

**Interfaces:**
- Consumes: `config`, `clip`, `protocol`, `sync` modules
- Produces: Runnable console binary: clipboard listener + debounce + log output

- [ ] **Step 1: Write console-only main.rs**

```rust
mod clip;
mod config;
mod protocol;
mod sync;

use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!(
        "ClipSync PC starting... port={} debounce={}ms",
        cfg.connection.port,
        cfg.clipboard.debounce_ms
    );

    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);

    log::info!("Listening for clipboard changes...");

    loop {
        let Some(mut text) = clip_rx.recv().await else { break };

        loop {
            tokio::select! {
                _ = tokio::time::sleep(debounce) => { break; }
                Some(new_text) = clip_rx.recv() => { text = new_text; }
            }
        }

        if engine.should_send(&text) {
            log::info!("Clipboard changed: {} chars", text.len());
            engine.mark_sent(&text);
            let msg = protocol::ClipMessage::push(text);
            log::info!("Would send: {}", msg.to_json());
        }
    }

    Ok(())
}
```

- [ ] **Step 2: Manual smoke test**

```bash
cargo run
```
Expected:
- Logs "ClipSync PC starting..."
- Logs "Listening for clipboard changes..."
- Copy text → logged "Clipboard changed: N chars" + JSON
- Copy same text again → no duplicate log
- Copy quickly several different texts → only last fires (debounce)

Ctrl+C to stop.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/main.rs
git commit -m "feat: wire main event loop with clipboard listener, dedup, and debounce"
```

---

### Task 7: WebSocket Client + HMAC Handshake

**Files:**
- Create: `clipsync-pc/src/ws.rs`

**Interfaces:**
- Produces: `pub type WebSocketStream` (alias for `tokio_tungstenite::WebSocketStream<MaybeTlsStream<TcpStream>>`)
- Produces: `pub async fn connect_and_auth(uri: &str, secret: &str) -> anyhow::Result<WebSocketStream>` — full WS connect + HMAC challenge-response handshake
- Produces: `pub async fn send(ws: &mut WebSocketStream, msg: &ClipMessage) -> anyhow::Result<()>`
- Produces: `pub async fn recv(ws: &mut WebSocketStream) -> anyhow::Result<ClipMessage>` — loops until text message; handles Ping/Pong internally

- [ ] **Step 1: Write the WebSocket module**

```rust
use crate::protocol::ClipMessage;
use futures_util::{SinkExt, StreamExt};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use tokio_tungstenite::connect_async;
use tokio_tungstenite::tungstenite::Message;

pub type WebSocketStream = tokio_tungstenite::WebSocketStream<
    tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>,
>;

pub async fn connect_and_auth(uri: &str, secret: &str) -> anyhow::Result<WebSocketStream> {
    let url: url::Url = uri.parse()?;
    let (mut ws, _response) = connect_async(url).await?;

    let raw = ws
        .next()
        .await
        .ok_or_else(|| anyhow::anyhow!("connection closed before hello"))??;
    let msg = match raw {
        Message::Text(t) => ClipMessage::from_json(&t)?,
        _ => anyhow::bail!("expected text message, got binary"),
    };

    let challenge = match msg {
        ClipMessage::Hello { challenge } => challenge,
        ClipMessage::AuthFail => anyhow::bail!("server rejected previous auth"),
        other => anyhow::bail!("expected hello, got {:?}", other),
    };

    let mut mac = Hmac::<Sha256>::new_from_slice(secret.as_bytes())
        .map_err(|_| anyhow::anyhow!("invalid secret key"))?;
    mac.update(challenge.as_bytes());
    let response = hex::encode(mac.finalize().into_bytes());

    let auth_msg = ClipMessage::Auth { response };
    ws.send(Message::Text(auth_msg.to_json())).await?;

    let raw = ws
        .next()
        .await
        .ok_or_else(|| anyhow::anyhow!("connection closed before auth result"))??;
    let msg = match raw {
        Message::Text(t) => ClipMessage::from_json(&t)?,
        _ => anyhow::bail!("expected text message"),
    };

    match msg {
        ClipMessage::AuthOk => {
            log::info!("HMAC authentication successful");
            Ok(ws)
        }
        ClipMessage::AuthFail => anyhow::bail!("authentication failed"),
        other => anyhow::bail!("expected auth_ok/auth_fail, got {:?}", other),
    }
}

pub async fn send(ws: &mut WebSocketStream, msg: &ClipMessage) -> anyhow::Result<()> {
    ws.send(Message::Text(msg.to_json())).await?;
    Ok(())
}

pub async fn recv(ws: &mut WebSocketStream) -> anyhow::Result<ClipMessage> {
    loop {
        let raw = ws
            .next()
            .await
            .ok_or_else(|| anyhow::anyhow!("connection closed"))??;
        match raw {
            Message::Text(t) => {
                if let Ok(msg) = ClipMessage::from_json(&t) {
                    return Ok(msg);
                }
            }
            Message::Ping(data) => {
                let _ = ws.send(Message::Pong(data)).await;
            }
            Message::Close(_) => {
                anyhow::bail!("connection closed by peer");
            }
            _ => {}
        }
    }
}
```

- [ ] **Step 2: Add module and verify compile**

In `main.rs`, add after existing mods:
```rust
mod ws;
```

Run: `cargo check`
Expected: Compiles (ws module not yet used but present).

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/ws.rs clipsync-pc/src/main.rs
git commit -m "feat: add WebSocket client with HMAC-SHA256 handshake"
```

---

### Task 8: mDNS Discovery Module

**Files:**
- Create: `clipsync-pc/src/mdns.rs`

**Interfaces:**
- Produces: `pub fn discover() -> anyhow::Result<mpsc::UnboundedReceiver<String>>` — returns receiver of discovered `ws://ip:port` URIs for `_clipsync._tcp.local.`

- [ ] **Step 1: Write the mDNS module**

```rust
use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::time::Duration;
use tokio::sync::mpsc;

const SERVICE_TYPE: &str = "_clipsync._tcp.local.";

pub fn discover() -> anyhow::Result<mpsc::UnboundedReceiver<String>> {
    let (tx, rx) = mpsc::unbounded_channel();
    let daemon = ServiceDaemon::new()?;
    let browser = daemon.browse(SERVICE_TYPE)?;

    std::thread::spawn(move || loop {
        match browser.recv() {
            Ok(ServiceEvent::ServiceResolved(info)) => {
                if let Some(addr) = info.get_addresses().iter().next().cloned() {
                    let uri = format!("ws://{}:{}", addr, info.get_port());
                    log::info!("mDNS discovered: {}", uri);
                    let _ = tx.send(uri);
                }
            }
            Ok(_) => {}
            Err(e) => {
                log::error!("mDNS browse error: {}", e);
                break;
            }
        }
    });

    std::thread::spawn(move || loop {
        std::thread::sleep(Duration::from_secs(30));
        let _ = daemon.browse(SERVICE_TYPE);
    });

    Ok(rx)
}
```

- [ ] **Step 2: Add module and verify compile**

In `main.rs`, add:
```rust
mod mdns;
```

Run: `cargo check`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/mdns.rs clipsync-pc/src/main.rs
git commit -m "feat: add mDNS discovery module"
```

---

### Task 9: Full Sync Loop — Network + Clipboard Integration

**Files:**
- Modify: `clipsync-pc/src/main.rs`

**Interfaces:**
- Consumes: `config`, `clip`, `mdns`, `protocol`, `sync`, `ws`
- Produces: Full sync loop: mDNS discover → WS connect+auth → bidirectional sync with dedup and pending buffer (console mode)

- [ ] **Step 1: Rewrite main.rs with integrated sync loop**

```rust
mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod ws;

use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = config::ClipSyncConfig::load("clipsync.toml")?;
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);

    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);
    let mut backoff_secs = 1u64;
    let max_backoff = 60u64;

    loop {
        log::info!("State: Disconnected — starting mDNS discovery");
        let Ok(mut mdns_rx) = mdns::discover() else {
            log::error!("mDNS discovery failed, retrying in 5s");
            tokio::time::sleep(Duration::from_secs(5)).await;
            continue;
        };

        let uri = loop {
            tokio::select! {
                Some(uri) = mdns_rx.recv() => break uri,
                Some(text) = clip_rx.recv() => {
                    engine.store_pending(text);
                }
                _ = tokio::time::sleep(Duration::from_secs(5)) => {
                    log::debug!("Waiting for mDNS discovery...");
                }
            }
        };

        let mut ws = loop {
            log::info!("Connecting to {}...", uri);
            match ws::connect_and_auth(&uri, &cfg.auth.secret).await {
                Ok(ws) => {
                    log::info!("Connected and authenticated!");
                    break ws;
                }
                Err(e) => {
                    log::error!("Connection failed: {}", e);
                    log::info!("Retrying in {}s...", backoff_secs);
                    tokio::time::sleep(Duration::from_secs(backoff_secs)).await;
                    backoff_secs = (backoff_secs * 2).min(max_backoff);
                }
            }
        };
        backoff_secs = 1;

        log::info!("State: Connected");

        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text);
            if let Err(e) = ws::send(&mut ws, &msg).await {
                log::error!("Failed to flush pending: {}", e);
            } else {
                log::info!("Flushed pending clipboard to phone");
            }
        }

        let mut debounce_text: Option<String> = None;
        let mut debounce_sleep: Option<tokio::time::Sleep> = None;

        'connected: loop {
            tokio::select! {
                Some(text) = clip_rx.recv() => {
                    debounce_text = Some(text);
                    debounce_sleep = Some(tokio::time::sleep(debounce));
                }
                _ = async { debounce_sleep.as_mut().unwrap().await }, if debounce_sleep.is_some() => {
                    let text = debounce_text.take().unwrap();
                    debounce_sleep = None;
                    if engine.should_send(&text) {
                        let msg = protocol::ClipMessage::set(text.clone());
                        if ws::send(&mut ws, &msg).await.is_err() {
                            break 'connected;
                        }
                        engine.mark_sent(&text);
                        log::info!("Sent to phone: {} chars", text.len());
                    }
                }
                result = ws::recv(&mut ws) => {
                    match result {
                        Ok(protocol::ClipMessage::Push { text, .. }) => {
                            log::info!("Received from phone: {} chars", text.len());
                            clip::write(&text);
                            engine.mark_sent(&text);
                        }
                        Ok(protocol::ClipMessage::Ping) => {
                            let _ = ws::send(&mut ws, &protocol::ClipMessage::Pong).await;
                        }
                        Ok(protocol::ClipMessage::Pong) => {}
                        Err(e) => {
                            log::error!("Connection lost: {}", e);
                            break 'connected;
                        }
                        _ => {}
                    }
                }
            }
        }

        log::info!("State: Disconnected");
    }
}
```

- [ ] **Step 2: Verify compile**

```bash
cargo check
```
Expected: Compiles without errors.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/main.rs
git commit -m "feat: integrate full sync loop with mDNS, WS, dedup, and pending buffer"
```

---

### Task 10: System Tray Module

**Files:**
- Create: `clipsync-pc/src/tray.rs`

**Interfaces:**
- Produces: `pub enum ConnState { Disconnected, Connecting, Connected }`
- Produces: `pub enum TrayAction { Reconnect, TogglePause, Quit }`
- Produces: `pub struct Tray` — wraps `tray_icon::TrayIcon`
  - `pub fn new(event_loop: &ActiveEventLoop) -> anyhow::Result<Self>`
  - `pub fn update_state(&mut self, state: ConnState)`
  - `pub fn try_recv_action(&self) -> Option<TrayAction>`

- [ ] **Step 1: Write the tray module**

```rust
use std::sync::mpsc;
use tray_icon::{
    menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem},
    Icon, TrayIcon, TrayIconBuilder,
};
use winit::event_loop::ActiveEventLoop;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Disconnected,
    Connecting,
    Connected,
}

#[derive(Debug)]
pub enum TrayAction {
    Reconnect,
    TogglePause,
    Quit,
}

pub struct Tray {
    tray_icon: TrayIcon,
    menu_rx: mpsc::Receiver<TrayAction>,
    _menu: Menu<MenuItem>,
}

fn make_icon_data(r: u8, g: u8, b: u8) -> Vec<u8> {
    let size: u32 = 32;
    let radius: f64 = 14.0;
    let center = size as f64 / 2.0;
    let mut data = Vec::with_capacity((size * size * 4) as usize);
    for y in 0..size {
        for x in 0..size {
            let dx = x as f64 - center;
            let dy = y as f64 - center;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist <= radius {
                data.extend_from_slice(&[r, g, b, 255]);
            } else {
                data.extend_from_slice(&[0, 0, 0, 0]);
            }
        }
    }
    data
}

fn icon_for_state(state: ConnState) -> Icon {
    let (r, g, b) = match state {
        ConnState::Connected => (0, 200, 0),
        ConnState::Connecting => (200, 200, 0),
        ConnState::Disconnected => (128, 128, 128),
    };
    let rgba = make_icon_data(r, g, b);
    Icon::from_rgba(rgba, 32, 32).expect("failed to create icon")
}

impl Tray {
    pub fn new(event_loop: &ActiveEventLoop) -> anyhow::Result<Self> {
        let menu = Menu::new();
        let reconnect_item = MenuItem::new("Reconnect", true, None);
        let pause_item = MenuItem::new("Pause Sync", true, None);
        let separator = PredefinedMenuItem::separator();
        let quit_item = MenuItem::new("Quit", true, None);

        menu.append(&reconnect_item)?;
        menu.append(&pause_item)?;
        menu.append(&separator)?;
        menu.append(&quit_item)?;

        let icon = icon_for_state(ConnState::Disconnected);
        let tray_icon = TrayIconBuilder::new()
            .with_menu(Box::new(menu.clone()))
            .with_icon(icon)
            .with_tooltip("ClipSync \u{00b7} Not connected")
            .build(event_loop)?;

        let (menu_tx, menu_rx) = mpsc::channel();
        let reconnect_id = reconnect_item.id().clone();
        let pause_id = pause_item.id().clone();
        let quit_id = quit_item.id().clone();

        std::thread::spawn(move || loop {
            if let Ok(event) = MenuEvent::receiver().recv() {
                let action = if event.id == reconnect_id {
                    Some(TrayAction::Reconnect)
                } else if event.id == pause_id {
                    Some(TrayAction::TogglePause)
                } else if event.id == quit_id {
                    Some(TrayAction::Quit)
                } else {
                    None
                };
                if let Some(action) = action {
                    let _ = menu_tx.send(action);
                }
            }
        });

        Ok(Self {
            tray_icon,
            menu_rx,
            _menu: menu,
        })
    }

    pub fn update_state(&mut self, state: ConnState) {
        let icon = icon_for_state(state);
        let tooltip = match state {
            ConnState::Connected => "ClipSync \u{00b7} Connected",
            ConnState::Connecting => "ClipSync \u{00b7} Connecting\u{2026}",
            ConnState::Disconnected => "ClipSync \u{00b7} Not connected",
        };
        let _ = self.tray_icon.set_icon(Some(icon));
        let _ = self.tray_icon.set_tooltip(Some(tooltip.into()));
    }

    pub fn try_recv_action(&self) -> Option<TrayAction> {
        self.menu_rx.try_recv().ok()
    }
}
```

- [ ] **Step 2: Add module and verify compile**

In `main.rs`, add:
```rust
mod tray;
```

Run: `cargo check`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/tray.rs clipsync-pc/src/main.rs
git commit -m "feat: add system tray with colored status icons and context menu"
```

---

### Task 11: Tray Integration — winit Event Loop with Background tokio

**Files:**
- Modify: `clipsync-pc/src/main.rs`

**Interfaces:**
- Rewrites main: winit event loop runs on main thread, tokio sync engine on background thread, `EventLoopProxy` for communication

- [ ] **Step 1: Rewrite main.rs with winit + tokio integration**

```rust
mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod tray;
mod ws;

use std::sync::Arc;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tray::{ConnState, Tray, TrayAction};
use winit::application::ApplicationHandler;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};

#[derive(Debug)]
enum UiEvent {
    StateChanged(ConnState),
}

struct App {
    tray: Option<Tray>,
    proxy: EventLoopProxy<UiEvent>,
    action_tx: std::sync::mpsc::Sender<TrayAction>,
}

impl App {
    fn new(proxy: EventLoopProxy<UiEvent>, action_tx: std::sync::mpsc::Sender<TrayAction>) -> Self {
        Self {
            tray: None,
            proxy,
            action_tx,
        }
    }
}

impl ApplicationHandler<UiEvent> for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.tray.is_none() {
            match Tray::new(event_loop) {
                Ok(t) => self.tray = Some(t),
                Err(e) => {
                    log::error!("Failed to create tray: {}", e);
                    event_loop.exit();
                }
            }
        }
    }

    fn window_event(
        &mut self,
        _event_loop: &ActiveEventLoop,
        _window_id: winit::window::WindowId,
        _event: winit::event::WindowEvent,
    ) {}

    fn user_event(&mut self, _event_loop: &ActiveEventLoop, event: UiEvent) {
        match event {
            UiEvent::StateChanged(state) => {
                if let Some(ref mut tray) = self.tray {
                    tray.update_state(state);
                }
            }
        }
    }

    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        if let Some(ref tray) = self.tray {
            if let Some(action) = tray.try_recv_action() {
                if matches!(action, TrayAction::Quit) {
                    _event_loop.exit();
                    return;
                }
                let _ = self.action_tx.send(action);
            }
        }
    }
}

async fn run_sync_loop(
    cfg: Arc<config::ClipSyncConfig>,
    proxy: EventLoopProxy<UiEvent>,
    action_rx: std::sync::mpsc::Receiver<TrayAction>,
) {
    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);
    let mut backoff_secs = 1u64;
    let max_backoff = 60u64;

    let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Disconnected));

    loop {
        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Disconnected));
        log::info!("State: Disconnected — starting mDNS discovery");

        let Ok(mut mdns_rx) = mdns::discover() else {
            log::error!("mDNS discovery failed, retrying in 5s");
            tokio::time::sleep(Duration::from_secs(5)).await;
            continue;
        };

        let uri = loop {
            tokio::select! {
                Some(uri) = mdns_rx.recv() => break uri,
                Some(text) = clip_rx.recv() => {
                    engine.store_pending(text);
                }
                _ = tokio::time::sleep(Duration::from_secs(5)) => {}
            }
        };

        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Connecting));

        let mut ws = loop {
            log::info!("Connecting to {}...", uri);
            match ws::connect_and_auth(&uri, &cfg.auth.secret).await {
                Ok(ws) => {
                    log::info!("Connected and authenticated!");
                    break ws;
                }
                Err(e) => {
                    log::error!("Connection failed: {}", e);
                    log::info!("Retrying in {}s...", backoff_secs);
                    tokio::time::sleep(Duration::from_secs(backoff_secs)).await;
                    backoff_secs = (backoff_secs * 2).min(max_backoff);
                }
            }
        };
        backoff_secs = 1;

        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Connected));

        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text);
            let _ = ws::send(&mut ws, &msg).await;
        }

        let mut debounce_text: Option<String> = None;
        let mut debounce_sleep: Option<tokio::time::Sleep> = None;

        'connected: loop {
            tokio::select! {
                Some(text) = clip_rx.recv() => {
                    debounce_text = Some(text);
                    debounce_sleep = Some(tokio::time::sleep(debounce));
                }
                _ = async { debounce_sleep.as_mut().unwrap().await }, if debounce_sleep.is_some() => {
                    let text = debounce_text.take().unwrap();
                    debounce_sleep = None;
                    if engine.should_send(&text) {
                        let msg = protocol::ClipMessage::set(text.clone());
                        if ws::send(&mut ws, &msg).await.is_err() { break 'connected; }
                        engine.mark_sent(&text);
                    }
                }
                result = ws::recv(&mut ws) => {
                    match result {
                        Ok(protocol::ClipMessage::Push { text, .. }) => {
                            clip::write(&text);
                            engine.mark_sent(&text);
                        }
                        Ok(protocol::ClipMessage::Ping) => {
                            let _ = ws::send(&mut ws, &protocol::ClipMessage::Pong).await;
                        }
                        Ok(protocol::ClipMessage::Pong) => {}
                        Err(_) => break 'connected,
                        _ => {}
                    }
                }
            }
        }

        log::info!("State: Disconnected");
    }
}

fn main() -> anyhow::Result<()> {
    env_logger::init();

    let cfg = Arc::new(config::ClipSyncConfig::load("clipsync.toml")?);
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);

    let event_loop = EventLoop::<UiEvent>::with_user_event().build()?;
    let proxy = event_loop.create_proxy();

    let (action_tx, action_rx) = std::sync::mpsc::channel::<TrayAction>();

    let cfg_clone = cfg.clone();
    let proxy_clone = proxy.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().unwrap();
        rt.block_on(run_sync_loop(cfg_clone, proxy_clone, action_rx));
    });

    let mut app = App::new(proxy, action_tx);
    event_loop.set_control_flow(ControlFlow::Poll);
    event_loop.run_app(&mut app)?;

    Ok(())
}
```

- [ ] **Step 2: Verify compile and run**

```bash
cargo check
```
Expected: Compiles. Then:

```bash
cargo run
```
Expected: Tray icon appears (grey dot). Right-click shows menu. Green dot when daemon is running.

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/main.rs
git commit -m "feat: integrate system tray with winit event loop and background tokio sync engine"
```

---

### Task 12: Android Daemon — Project Structure

**Files:**
- Create: `clipsync-daemon/Makefile`
- Create: `clipsync-daemon/protocol.h`
- Create: `clipsync-daemon/clipsyncd.c` (skeleton)

**Interfaces:**
- Produces: Buildable C project skeleton; `protocol.h` shared JSON message type constants

- [ ] **Step 1: Write protocol.h**

```c
#ifndef CLIPSYNC_PROTOCOL_H
#define CLIPSYNC_PROTOCOL_H

#define WS_PORT 5287
#define DEFAULT_DEBOUNCE_MS 300
#define MDNS_SERVICE_TYPE "_clipsync._tcp"

/* Message type tags */
#define MSG_TYPE_PUSH     "clipboard_push"
#define MSG_TYPE_SET      "clipboard_set"
#define MSG_TYPE_PING     "ping"
#define MSG_TYPE_PONG     "pong"
#define MSG_TYPE_HELLO    "hello"
#define MSG_TYPE_AUTH     "auth"
#define MSG_TYPE_AUTH_OK  "auth_ok"
#define MSG_TYPE_AUTH_FAIL "auth_fail"

#endif
```

- [ ] **Step 2: Write skeleton clipsyncd.c**

```c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "protocol.h"

static volatile int running = 1;

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    printf("[clipsyncd] starting on port %d...\n", WS_PORT);

    while (running) {
        sleep(1);
    }

    printf("[clipsyncd] shutting down.\n");
    return 0;
}
```

- [ ] **Step 3: Write Makefile**

```makefile
CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2 -static
TARGET = clipsyncd
SRCS = clipsyncd.c

all: $(TARGET)

$(TARGET): $(SRCS) protocol.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
```

- [ ] **Step 4: Commit**

```bash
git add clipsync-daemon/
git commit -m "feat: add Android daemon project skeleton"
```

---

### Task 13: Android Daemon — Binder IPC Module

**Files:**
- Create: `clipsync-daemon/binder_clip.h`
- Create: `clipsync-daemon/binder_clip.c`

**Interfaces:**
- Produces: `int binder_clip_init(void)` — get IClipboard service, register listener
- Produces: `char* binder_clip_get_text(void)` — read clipboard text via Binder, caller frees
- Produces: `int binder_clip_set_text(const char* text)` — write text via Binder
- Produces: callback registration: `void binder_clip_set_callback(void (*fn)(const char*))` — called on clipboard change

- [ ] **Step 1: Write binder_clip.h**

```c
#ifndef BINDER_CLIP_H
#define BINDER_CLIP_H

typedef void (*clip_change_cb)(const char *text);

int  binder_clip_init(void);
char *binder_clip_get_text(void);
int  binder_clip_set_text(const char *text);
void binder_clip_set_callback(clip_change_cb cb);

#endif
```

- [ ] **Step 2: Write binder_clip.c**

```c
#include "binder_clip.h"
#include <android/binder_manager.h>
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Transaction codes (from AIDL) */
#define TRANSACTION_GET_PRIMARY_CLIP           1
#define TRANSACTION_SET_PRIMARY_CLIP           3
#define TRANSACTION_ADD_PRIMARY_CLIP_CHANGED_LISTENER 4

/* IOnPrimaryClipChangedListener transaction */
#define TRANSACTION_DISPATCH_CLIP_CHANGED      1

static AIBinder *g_clipboard_svc = NULL;
static AIBinder *g_listener = NULL;
static clip_change_cb g_callback = NULL;

static void *g_clip_listener_class = NULL;

static binder_status_t on_transact(
    AIBinder *binder,
    transaction_code_t code,
    const AParcel *in,
    AParcel *out)
{
    (void)binder; (void)in; (void)out;
    if (code == TRANSACTION_DISPATCH_CLIP_CHANGED) {
        if (g_callback) {
            char *text = binder_clip_get_text();
            if (text) {
                g_callback(text);
                free(text);
            }
        }
        return STATUS_OK;
    }
    return STATUS_UNKNOWN_TRANSACTION;
}

int binder_clip_init(void) {
    /* Get the clipboard service */
    g_clipboard_svc = AServiceManager_getService("clipboard");
    if (!g_clipboard_svc) {
        fprintf(stderr, "[binder_clip] failed to get clipboard service\n");
        return -1;
    }

    /* Create listener class */
    g_clip_listener_class = (void*)AIBinder_Class_define(
        "IOnPrimaryClipChangedListener",
        NULL,       /* onCreate */
        NULL,       /* onDestroy */
        on_transact
    );
    if (!g_clip_listener_class) {
        fprintf(stderr, "[binder_clip] failed to define listener class\n");
        return -1;
    }

    /* Create listener instance */
    g_listener = AIBinder_new((AIBinder_Class*)g_clip_listener_class, NULL);
    if (!g_listener) {
        fprintf(stderr, "[binder_clip] failed to create listener\n");
        return -1;
    }

    /* Register listener: addPrimaryClipChangedListener */
    AParcel *data = AParcel_create();
    AParcel_writeStrongBinder(data, g_listener);
    AParcel_writeString(data, "clipsync", 8);
    AParcel_writeString(data, "", 0);
    AParcel_writeInt32(data, 0); /* userId */
    AParcel_writeInt32(data, 0); /* deviceId */

    binder_status_t status = AIBinder_transact(
        g_clipboard_svc,
        TRANSACTION_ADD_PRIMARY_CLIP_CHANGED_LISTENER,
        data,
        NULL,
        FLAG_ONEWAY
    );
    AParcel_delete(data);

    if (status != STATUS_OK) {
        fprintf(stderr, "[binder_clip] register listener failed: %d\n", status);
        return -1;
    }

    printf("[binder_clip] initialized, listener registered\n");
    return 0;
}

char *binder_clip_get_text(void) {
    /* transact getPrimaryClip */
    AParcel *data = AParcel_create();
    AParcel_writeString(data, "clipsync", 8); /* callingPackage */
    AParcel_writeInt32(data, 0);              /* userId */
    AParcel_writeInt32(data, 0);              /* deviceId */

    AParcel *reply = AParcel_create();
    binder_status_t status = AIBinder_transact(
        g_clipboard_svc,
        TRANSACTION_GET_PRIMARY_CLIP,
        data,
        reply,
        0
    );

    char *result = NULL;
    if (status == STATUS_OK) {
        AParcel_readInt32(reply, NULL); /* read ClipboardData presence flag */
        const char *text = NULL;
        int32_t len = 0;
        AParcel_readString(reply, &text, &len);
        if (text && len > 0) {
            result = strdup(text);
        }
    }

    AParcel_delete(data);
    AParcel_delete(reply);
    return result;
}

int binder_clip_set_text(const char *text) {
    /* Construct ClipData via transact setPrimaryClip */
    AParcel *data = AParcel_create();
    AParcel_writeString(data, "clipsync", 8); /* callingPackage */
    AParcel_writeInt32(data, 0);              /* userId */
    AParcel_writeInt32(data, 0);              /* deviceId */
    AParcel_writeString(data, text, (int32_t)strlen(text));
    AParcel_writeString(data, "", 0);         /* attributionTag */

    binder_status_t status = AIBinder_transact(
        g_clipboard_svc,
        TRANSACTION_SET_PRIMARY_CLIP,
        data,
        NULL,
        FLAG_ONEWAY
    );

    AParcel_delete(data);
    return (status == STATUS_OK) ? 0 : -1;
}

void binder_clip_set_callback(clip_change_cb cb) {
    g_callback = cb;
}
```

- [ ] **Step 3: Commit**

```bash
git add clipsync-daemon/binder_clip.c clipsync-daemon/binder_clip.h
git commit -m "feat: add Android daemon Binder IPC module for clipboard read/write/listen"
```

---

### Task 14: Android Daemon — WebSocket Server + mDNS

**Files:**
- Create: `clipsync-daemon/ws_server.h`
- Create: `clipsync-daemon/ws_server.c`
- Create: `clipsync-daemon/mdns_publish.h`
- Create: `clipsync-daemon/mdns_publish.c`

**Interfaces:**
- Produces: `int ws_server_init(int port, const char *secret)` — start mongoose-based WS server
- Produces: `void ws_server_poll(int timeout_ms)` — drive mongoose event loop
- Produces: `void ws_server_broadcast(const char *json)` — send to all authenticated clients
- Produces: `void ws_server_set_on_set(void (*fn)(const char *text))` — callback for incoming set messages
- Produces: `int mdns_publish_init(int port)` / `void mdns_publish_poll(void)` — register `_clipsync._tcp` service

Note: The C code below is a structural scaffold. Full mongoose integration requires downloading mongoose.c/h and linking. The interfaces and logic are shown; exact mongoose API calls may need minor adaptation per mongoose version.

- [ ] **Step 1: Write ws_server.h**

```c
#ifndef WS_SERVER_H
#define WS_SERVER_H

typedef void (*ws_on_set_fn)(const char *text);

int  ws_server_init(int port, const char *secret);
void ws_server_poll(int timeout_ms);
void ws_server_broadcast(const char *json);
void ws_server_set_on_set(ws_on_set_fn fn);

#endif
```

- [ ] **Step 2: Write ws_server.c (scaffold)**

```c
#include "ws_server.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

#define MAX_CLIENTS 4

static int g_port = 5287;
static const char *g_secret = NULL;
static ws_on_set_fn g_on_set = NULL;

int ws_server_init(int port, const char *secret) {
    g_port = port;
    g_secret = secret;
    printf("[ws_server] initialized on port %d\n", port);
    /* mongoose initialization: mg_mgr_init, mg_http_listen, etc. */
    return 0;
}

void ws_server_poll(int timeout_ms) {
    /* mg_mgr_poll(mgr, timeout_ms) */
    (void)timeout_ms;
}

void ws_server_broadcast(const char *json) {
    /* Iterate connected clients, send json via mg_ws_send */
    (void)json;
    printf("[ws_server] broadcast: %s\n", json);
}

void ws_server_set_on_set(ws_on_set_fn fn) {
    g_on_set = fn;
}
```

- [ ] **Step 3: Write mdns_publish.h**

```c
#ifndef MDNS_PUBLISH_H
#define MDNS_PUBLISH_H

int  mdns_publish_init(int port);
void mdns_publish_poll(void);

#endif
```

- [ ] **Step 4: Write mdns_publish.c (scaffold)**

```c
#include "mdns_publish.h"
#include <stdio.h>

int mdns_publish_init(int port) {
    printf("[mdns] publishing _clipsync._tcp on port %d\n", port);
    /* mongoose mdns: mg_mdns_register with service type and port */
    return 0;
}

void mdns_publish_poll(void) {
    /* Driven by mg_mgr_poll in the main loop */
}
```

- [ ] **Step 5: Commit**

```bash
git add clipsync-daemon/ws_server.h clipsync-daemon/ws_server.c \
        clipsync-daemon/mdns_publish.h clipsync-daemon/mdns_publish.c
git commit -m "feat: add Android daemon WebSocket server and mDNS publish scaffolds"
```

---

### Task 15: Android Daemon — Main Event Loop Integration

**Files:**
- Modify: `clipsync-daemon/clipsyncd.c`
- Modify: `clipsync-daemon/Makefile`

**Interfaces:**
- Consumes: `binder_clip`, `ws_server`, `mdns_publish`
- Produces: Full daemon binary: Binder listener → read clipboard → push via WS; receive WS set → write clipboard via Binder

- [ ] **Step 1: Write full clipsyncd.c**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "protocol.h"
#include "binder_clip.h"
#include "ws_server.h"
#include "mdns_publish.h"

static volatile int running = 1;
static const char *g_secret = NULL;
static char g_last_text[65536] = {0};

static void on_clip_change(const char *text) {
    if (!text) return;
    /* Dedup */
    if (strcmp(text, g_last_text) == 0) return;
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);

    /* Build push JSON */
    char json[65536 * 2];
    unsigned long ts = (unsigned long)time(NULL) * 1000;
    snprintf(json, sizeof(json),
        "{\"type\":\"clipboard_push\",\"text\":\"%s\",\"ts\":%lu}",
        text, ts);
    ws_server_broadcast(json);
    printf("[clipsyncd] pushed: %lu chars\n", (unsigned long)strlen(text));
}

static void on_ws_set(const char *text) {
    if (!text) return;
    printf("[clipsyncd] received set: %lu chars\n", (unsigned long)strlen(text));
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);
    binder_clip_set_text(text);
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    int port = WS_PORT;
    g_secret = "";

    /* Parse args: clipsyncd [--port N] [--secret S] */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            g_secret = argv[++i];
        }
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    printf("[clipsyncd] starting on port %d...\n", port);

    /* Initialize subsystems */
    if (ws_server_init(port, g_secret) != 0) {
        fprintf(stderr, "[clipsyncd] ws_server_init failed\n");
        return 1;
    }
    ws_server_set_on_set(on_ws_set);

    if (mdns_publish_init(port) != 0) {
        fprintf(stderr, "[clipsyncd] mdns_publish_init failed\n");
        return 1;
    }

    if (binder_clip_init() != 0) {
        fprintf(stderr, "[clipsyncd] binder_clip_init failed\n");
        return 1;
    }
    binder_clip_set_callback(on_clip_change);

    printf("[clipsyncd] running. Waiting for connections and clipboard events...\n");

    /* Event loop */
    while (running) {
        ws_server_poll(50);   /* drive mongoose (WS + mDNS) */
    }

    printf("[clipsyncd] shutting down.\n");
    return 0;
}
```

- [ ] **Step 2: Update Makefile**

```makefile
CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2 -static
TARGET = clipsyncd
SRCS = clipsyncd.c binder_clip.c ws_server.c mdns_publish.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) protocol.h
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

install:
	adb push $(TARGET) /data/local/tmp/
	adb shell chmod 755 /data/local/tmp/$(TARGET)

.PHONY: all clean install
```

- [ ] **Step 3: Commit**

```bash
git add clipsync-daemon/clipsyncd.c clipsync-daemon/Makefile
git commit -m "feat: wire Android daemon main event loop with Binder + WS + mDNS"
```

---

### Task 16: KernelSU Module Packaging

**Files:**
- Create: `clipsync-daemon/module/module.prop`
- Create: `clipsync-daemon/module/post-fs-data.sh`
- Create: `clipsync-daemon/module/config/clipsync.toml`

- [ ] **Step 1: Write module.prop**

```properties
id=clipsync
name=ClipSync Clipboard Sync
version=v1.0
versionCode=1
author=ClipSync
description=System-level clipboard sync daemon with Binder event listener (LAN)
```

- [ ] **Step 2: Write post-fs-data.sh**

```bash
#!/system/bin/sh
MODDIR=${0%/*}

# Only start daemon after boot completes
(sleep 30 && \
  nohup $MODDIR/system/bin/clipsyncd \
    --port 5287 \
    --secret "$(cat $MODDIR/config/clipsync.toml | grep secret | cut -d'"' -f2)" \
    > /data/local/tmp/clipsyncd.log 2>&1 &
) &
```

- [ ] **Step 3: Write default config**

```toml
[connection]
port = 5287

[auth]
secret = ""

[clipboard]
debounce_ms = 300
```

- [ ] **Step 4: Create packaging Makefile target**

Add to `clipsync-daemon/Makefile`:

```makefile
module: $(TARGET)
	mkdir -p module/system/bin
	mkdir -p module/config
	cp $(TARGET) module/system/bin/
	cp config/clipsync.toml module/config/
	cd module && zip -r ../clipsync-module.zip .
	@echo "Module package: clipsync-module.zip"

.PHONY: module
```

- [ ] **Step 5: Commit**

```bash
git add clipsync-daemon/module/ clipsync-daemon/Makefile
git commit -m "feat: add KernelSU module packaging"
```

---

### Task 17: Logging + Error Handling Polish

**Files:**
- Modify: `clipsync-pc/src/main.rs` — replace remaining `log::info` with structured context
- Create: `clipsync-pc/src/error.rs` — unified error types

- [ ] **Step 1: Write error.rs**

```rust
use thiserror::Error;

#[derive(Error, Debug)]
pub enum ClipSyncError {
    #[error("mDNS discovery failed: {0}")]
    MdnsError(String),
    #[error("WebSocket connection failed: {0}")]
    WsError(String),
    #[error("Authentication failed: {0}")]
    AuthError(String),
    #[error("Clipboard error: {0}")]
    ClipError(String),
    #[error("Config error: {0}")]
    ConfigError(String),
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, ClipSyncError>;
```

- [ ] **Step 2: Add module and verify**

```bash
git add clipsync-pc/src/error.rs
# Add "mod error;" to main.rs
```
Run: `cargo check`

- [ ] **Step 3: Commit**

```bash
git add clipsync-pc/src/error.rs clipsync-pc/src/main.rs
git commit -m "feat: add unified error types module"
```

---

### Task 18: Build Script + README

**Files:**
- Create: `README.md`
- Create: `scripts/build.ps1`

- [ ] **Step 1: Write minimal README**

```markdown
# ClipSync

Cross-device clipboard sync between Android and PC over LAN.

## Components

- **clipsync-pc** — Windows tray app (Rust)
- **clipsync-daemon** — Android KernelSU module (C)

## Quick Start (PC)

```powershell
cd clipsync-pc
cargo build --release
.\target\release\clipsync-pc.exe
```

## Configuration

Copy `clipsync-pc/clipsync.toml.example` to `clipsync.toml` and set your pre-shared secret.
```

- [ ] **Step 2: Write build script**

```powershell
param(
    [switch]$Release = $false
)

$ErrorActionPreference = "Stop"

Write-Host "Building ClipSync PC client..."

Push-Location clipsync-pc
try {
    if ($Release) {
        cargo build --release
        Write-Host "Binary: target/release/clipsync-pc.exe"
    } else {
        cargo build
        Write-Host "Binary: target/debug/clipsync-pc.exe"
    }
} finally {
    Pop-Location
}

Write-Host "Done."
```

- [ ] **Step 3: Commit**

```bash
git add README.md scripts/build.ps1
git commit -m "docs: add README and build script"
```

---

## Implementation Order Summary

| Phase | Tasks | Deliverable |
|-------|-------|-------------|
| **Phase 1** (Core) | 1→6 | Console binary: clipboard listen + dedup |
| **Phase 2** (Network) | 7→9 | mDNS + WS + full sync loop (console) |
| **Phase 3** (Tray) | 10→11 | System tray with winit integration |
| **Phase 4** (Daemon) | 12→16 | Android daemon + KernelSU module |
| **Polish** | 17→18 | Errors, docs, build scripts |

Tasks 1-11 should be run sequentially in order. Tasks 12-15 can be done independently (C code, no Rust dependency). Task 16 depends on 12-15. Tasks 17-18 are leaf tasks.
