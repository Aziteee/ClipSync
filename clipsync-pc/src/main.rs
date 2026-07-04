#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod clip;
mod config;
mod mdns;
mod protocol;
mod startup;
mod sync;
mod tray;
mod update;
mod ws;

use std::collections::{HashMap, HashSet};
use std::future::pending;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tokio::time::{Instant, Sleep};
use tray::{ConnState, DeviceSummary, DeviceSummaryState, Tray, TrayAction};
use winit::application::ApplicationHandler;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};

#[derive(Debug)]
pub(crate) enum UiEvent {
    StateChanged(ConnState),
    TrayAction(TrayAction),
}

type DeviceId = String;

const PC_ORIGIN: &str = "pc";
static EVENT_COUNTER: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DeviceOrigin {
    Static,
    Discovered,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct DeviceTarget {
    id: DeviceId,
    name: String,
    uri: String,
    origin: DeviceOrigin,
    enabled: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DeviceConnectionState {
    Disconnected,
    Connecting,
    Connected,
}

#[derive(Debug)]
enum DeviceCommand {
    SendClipboard { text: String, event_id: String },
    Reconnect,
}

#[derive(Debug)]
enum DeviceEvent {
    StateChanged {
        id: DeviceId,
        state: DeviceConnectionState,
        last_error: Option<String>,
    },
    Push {
        id: DeviceId,
        text: String,
    },
    SendFailed {
        id: DeviceId,
        text: String,
        event_id: String,
        error: String,
    },
}

struct DeviceHandle {
    target: DeviceTarget,
    tx: mpsc::UnboundedSender<DeviceCommand>,
    state: DeviceConnectionState,
    last_error: Option<String>,
}

fn should_retry_failed_endpoint(_origin: DeviceOrigin) -> bool {
    true
}

fn heartbeat_is_stale(idle_for: Duration, timeout: Duration) -> bool {
    idle_for >= timeout
}

struct App {
    tray: Option<Tray>,
    proxy: EventLoopProxy<UiEvent>,
    action_tx: mpsc::UnboundedSender<TrayAction>,
    paused: Arc<AtomicBool>,
    config: config::ClipSyncConfig,
    config_path: std::path::PathBuf,
    tokio_handle: tokio::runtime::Handle,
}

impl App {
    fn new(
        proxy: EventLoopProxy<UiEvent>,
        action_tx: mpsc::UnboundedSender<TrayAction>,
        paused: Arc<AtomicBool>,
        config: config::ClipSyncConfig,
        config_path: std::path::PathBuf,
        tokio_handle: tokio::runtime::Handle,
    ) -> Self {
        Self {
            tray: None,
            proxy,
            action_tx,
            paused,
            config,
            config_path,
            tokio_handle,
        }
    }
}

impl ApplicationHandler<UiEvent> for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.tray.is_none() {
            match Tray::new(
                event_loop,
                self.proxy.clone(),
                self.config.general.start_with_windows,
            ) {
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
    ) {
    }

    fn user_event(&mut self, _event_loop: &ActiveEventLoop, event: UiEvent) {
        match event {
            UiEvent::StateChanged(state) => {
                if let Some(ref mut tray) = self.tray {
                    tray.update_state(state);
                }
            }
            UiEvent::TrayAction(action) => self.handle_tray_action(action, _event_loop),
        }
    }
}

impl App {
    fn handle_tray_action(&mut self, action: TrayAction, event_loop: &ActiveEventLoop) {
        if matches!(action, TrayAction::Quit) {
            event_loop.exit();
            return;
        }
        if matches!(action, TrayAction::TogglePause) {
            let was_paused = self.paused.fetch_xor(true, Ordering::SeqCst);
            let now_paused = !was_paused;
            if let Some(ref mut tray) = self.tray {
                tray.set_paused(now_paused);
            }
            return;
        }
        if matches!(action, TrayAction::ToggleStartWithWindows) {
            self.config.general.start_with_windows = !self.config.general.start_with_windows;
            let enabled = self.config.general.start_with_windows;
            if let Err(e) = startup::set_autostart(enabled) {
                log::error!("Failed to update autostart registry: {}", e);
            }
            if let Err(e) = self.config.save(&self.config_path) {
                log::error!("Failed to save config: {}", e);
            }
            if let Some(ref mut tray) = self.tray {
                tray.set_start_with_windows(enabled);
            }
            return;
        }
        if matches!(action, TrayAction::CheckForUpdates) {
            self.tokio_handle.spawn(async move {
                match update::check_for_updates().await {
                    update::CheckResult::UpdateAvailable => {
                        log::info!("Update available, opening browser");
                        update::open_releases_page();
                    }
                    update::CheckResult::UpToDate => {
                        log::info!("Already up to date");
                    }
                    update::CheckResult::Error => {
                        log::warn!("Update check failed");
                    }
                }
            });
            return;
        }
        let _ = self.action_tx.send(action);
    }
}

fn send_state(proxy: &EventLoopProxy<UiEvent>, state: ConnState) {
    if proxy
        .send_event(UiEvent::StateChanged(state.clone()))
        .is_err()
    {
        log::debug!("Failed to send UI state update: {:?}", state);
    }
}

fn preserve_text_after_send_failure(engine: &mut SyncEngine, device_id: &str, text: String) {
    engine.store_pending_for(device_id, text);
}

fn reconnect_interrupts_wait(action: TrayAction) -> bool {
    matches!(action, TrayAction::Reconnect)
}

fn handshake_timeout(cfg: &config::ClipSyncConfig) -> Duration {
    Duration::from_millis(cfg.connection.heartbeat_timeout_ms)
}

fn next_event_id() -> String {
    let seq = EVENT_COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("pc-{}-{}", protocol::now_millis(), seq)
}

fn device_id_for_uri(uri: &str) -> DeviceId {
    let hex = blake3::hash(uri.as_bytes()).to_hex();
    hex[..16].to_string()
}

fn target_from_uri(uri: String, name: Option<String>, origin: DeviceOrigin) -> DeviceTarget {
    let id = device_id_for_uri(&uri);
    let name = name
        .and_then(|s| {
            let trimmed = s.trim().to_string();
            if trimmed.is_empty() {
                None
            } else {
                Some(trimmed)
            }
        })
        .unwrap_or_default();
    DeviceTarget {
        id,
        name,
        uri,
        origin,
        enabled: true,
    }
}

fn configured_device_targets(cfg: &config::ClipSyncConfig) -> Vec<DeviceTarget> {
    let mut targets = Vec::new();
    let mut seen = HashSet::new();

    for device in cfg.devices.iter().filter(|device| device.enabled) {
        let uri = device.uri.trim();
        if uri.is_empty() || !seen.insert(uri.to_string()) {
            continue;
        }
        targets.push(target_from_uri(
            uri.to_string(),
            device.name.clone(),
            DeviceOrigin::Static,
        ));
    }

    if let Some(uri) = cfg.connection.direct_ws_uri() {
        if seen.insert(uri.clone()) {
            targets.push(target_from_uri(uri, None, DeviceOrigin::Static));
        }
    }

    targets
}

fn aggregate_conn_state(handles: &HashMap<DeviceId, DeviceHandle>) -> ConnState {
    let devices = device_summaries(handles);
    let connected = devices
        .iter()
        .filter(|device| device.state == DeviceSummaryState::Connected)
        .count();
    if connected > 0 {
        return ConnState::Connected { devices };
    }
    if devices
        .iter()
        .any(|device| device.state == DeviceSummaryState::Connecting)
    {
        return ConnState::Connecting { devices };
    }
    ConnState::Disconnected
}

fn device_summaries(handles: &HashMap<DeviceId, DeviceHandle>) -> Vec<DeviceSummary> {
    let mut devices: Vec<DeviceSummary> = handles
        .values()
        .filter(|handle| handle.target.enabled)
        .map(|handle| DeviceSummary {
            name: non_empty_name(&handle.target.name),
            ip: display_host_for_uri(&handle.target.uri),
            state: summary_state_for(handle.state),
        })
        .collect();
    devices.sort_by(|a, b| {
        a.ip.cmp(&b.ip)
            .then_with(|| a.name.as_deref().cmp(&b.name.as_deref()))
    });
    devices
}

fn non_empty_name(name: &str) -> Option<String> {
    let trimmed = name.trim();
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    }
}

fn summary_state_for(state: DeviceConnectionState) -> DeviceSummaryState {
    match state {
        DeviceConnectionState::Disconnected => DeviceSummaryState::Disconnected,
        DeviceConnectionState::Connecting => DeviceSummaryState::Connecting,
        DeviceConnectionState::Connected => DeviceSummaryState::Connected,
    }
}

fn display_host_for_uri(uri: &str) -> String {
    url::Url::parse(uri)
        .ok()
        .and_then(|url| {
            url.host_str()
                .map(|host| host.trim_matches(&['[', ']'][..]).to_string())
        })
        .unwrap_or_else(|| uri.to_string())
}

fn send_aggregate_state(
    proxy: &EventLoopProxy<UiEvent>,
    handles: &HashMap<DeviceId, DeviceHandle>,
) {
    send_state(proxy, aggregate_conn_state(handles));
}

fn start_discovery(port: u16) -> Option<mpsc::UnboundedReceiver<String>> {
    match mdns::discover(port) {
        Ok(rx) => Some(rx),
        Err(e) => {
            log::error!("mDNS discovery failed: {}", e);
            None
        }
    }
}

async fn recv_discovery(rx: &mut Option<mpsc::UnboundedReceiver<String>>) -> Option<String> {
    match rx {
        Some(rx) => rx.recv().await,
        None => pending().await,
    }
}

fn register_device(
    target: DeviceTarget,
    handles: &mut HashMap<DeviceId, DeviceHandle>,
    cfg: Arc<config::ClipSyncConfig>,
    device_event_tx: mpsc::UnboundedSender<DeviceEvent>,
    engine: &mut SyncEngine,
    latest_text: Option<&str>,
) -> bool {
    if handles.contains_key(&target.id) {
        return false;
    }

    if let Some(text) = latest_text {
        engine.store_pending_for(target.id.clone(), text.to_string());
    }

    log::info!(
        "Registering device {} ({}) from {:?}",
        target.name,
        target.uri,
        target.origin
    );
    let (tx, rx) = mpsc::unbounded_channel();
    tokio::spawn(run_device_task(target.clone(), cfg, rx, device_event_tx));
    handles.insert(
        target.id.clone(),
        DeviceHandle {
            target,
            tx,
            state: DeviceConnectionState::Connecting,
            last_error: None,
        },
    );
    true
}

fn send_clipboard_to_device(
    handle: &DeviceHandle,
    text: String,
    event_id: String,
) -> Result<(), mpsc::error::SendError<DeviceCommand>> {
    handle
        .tx
        .send(DeviceCommand::SendClipboard { text, event_id })
}

fn broadcast_clipboard(
    handles: &HashMap<DeviceId, DeviceHandle>,
    engine: &mut SyncEngine,
    text: String,
    event_id: String,
) {
    for (id, handle) in handles {
        if !handle.target.enabled {
            continue;
        }
        if handle.state == DeviceConnectionState::Connected {
            if let Err(e) = send_clipboard_to_device(handle, text.clone(), event_id.clone()) {
                log::warn!(
                    "Failed to queue clipboard for {}: {}",
                    handle.target.name,
                    e
                );
                engine.store_pending_for(id.clone(), text.clone());
            }
        } else {
            engine.store_pending_for(id.clone(), text.clone());
        }
    }
}

fn flush_pending_to_device(engine: &mut SyncEngine, handle: &DeviceHandle) {
    if let Some(text) = engine.take_pending_for(&handle.target.id) {
        let event_id = next_event_id();
        if let Err(e) = send_clipboard_to_device(handle, text.clone(), event_id) {
            log::warn!(
                "Failed to queue pending clipboard for {}: {}",
                handle.target.name,
                e
            );
            engine.store_pending_for(handle.target.id.clone(), text);
        }
    }
}

async fn run_device_task(
    target: DeviceTarget,
    cfg: Arc<config::ClipSyncConfig>,
    mut command_rx: mpsc::UnboundedReceiver<DeviceCommand>,
    event_tx: mpsc::UnboundedSender<DeviceEvent>,
) {
    let heartbeat_interval = Duration::from_millis(cfg.connection.heartbeat_interval_ms);
    let heartbeat_timeout = Duration::from_millis(cfg.connection.heartbeat_timeout_ms);
    let mut backoff_secs = 1u64;
    let max_backoff = 60u64;

    loop {
        let _ = event_tx.send(DeviceEvent::StateChanged {
            id: target.id.clone(),
            state: DeviceConnectionState::Connecting,
            last_error: None,
        });

        log::info!("Connecting to {} ({})...", target.name, target.uri);
        match ws::connect_and_auth(&target.uri, &cfg.auth.secret, handshake_timeout(&cfg)).await {
            Ok(mut ws) => {
                backoff_secs = 1;
                log::info!(
                    "Connected and authenticated: {} ({})",
                    target.name,
                    target.uri
                );
                let _ = event_tx.send(DeviceEvent::StateChanged {
                    id: target.id.clone(),
                    state: DeviceConnectionState::Connected,
                    last_error: None,
                });

                let reason = run_connected_device_loop(
                    &target,
                    &mut ws,
                    &mut command_rx,
                    &event_tx,
                    heartbeat_interval,
                    heartbeat_timeout,
                )
                .await;
                let _ = event_tx.send(DeviceEvent::StateChanged {
                    id: target.id.clone(),
                    state: DeviceConnectionState::Disconnected,
                    last_error: reason,
                });
            }
            Err(e) => {
                let error = e.to_string();
                log::error!("Connection failed for {}: {}", target.name, error);
                let _ = event_tx.send(DeviceEvent::StateChanged {
                    id: target.id.clone(),
                    state: DeviceConnectionState::Disconnected,
                    last_error: Some(error),
                });
            }
        }

        if !should_retry_failed_endpoint(target.origin) {
            break;
        }

        let retry_after = backoff_secs;
        backoff_secs = (backoff_secs * 2).min(max_backoff);
        let mut retry_sleep = Box::pin(tokio::time::sleep(Duration::from_secs(retry_after)));
        loop {
            tokio::select! {
                _ = &mut retry_sleep => break,
                command = command_rx.recv() => {
                    match command {
                        Some(DeviceCommand::Reconnect) => {
                            backoff_secs = 1;
                            break;
                        }
                        Some(DeviceCommand::SendClipboard { text, event_id }) => {
                            let _ = event_tx.send(DeviceEvent::SendFailed {
                                id: target.id.clone(),
                                text,
                                event_id,
                                error: "device is not connected".to_string(),
                            });
                        }
                        None => return,
                    }
                }
            }
        }
    }
}

async fn run_connected_device_loop(
    target: &DeviceTarget,
    ws: &mut ws::WebSocketStream,
    command_rx: &mut mpsc::UnboundedReceiver<DeviceCommand>,
    event_tx: &mpsc::UnboundedSender<DeviceEvent>,
    heartbeat_interval: Duration,
    heartbeat_timeout: Duration,
) -> Option<String> {
    let mut heartbeat_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(heartbeat_interval));
    let mut last_ws_activity = Instant::now();

    loop {
        tokio::select! {
            command = command_rx.recv() => {
                match command {
                    Some(DeviceCommand::SendClipboard { text, event_id }) => {
                        let msg = protocol::ClipMessage::set_with_meta(
                            text.clone(),
                            Some(event_id.clone()),
                            Some(PC_ORIGIN.to_string()),
                            Some(target.id.clone()),
                        );
                        if let Err(e) = ws::send(ws, &msg).await {
                            let error = e.to_string();
                            let _ = event_tx.send(DeviceEvent::SendFailed {
                                id: target.id.clone(),
                                text,
                                event_id,
                                error: error.clone(),
                            });
                            return Some(error);
                        }
                        log::info!(
                            "Sent clipboard_set to {}: {} chars",
                            target.name,
                            text.chars().count()
                        );
                    }
                    Some(DeviceCommand::Reconnect) => {
                        return Some("reconnect requested".to_string());
                    }
                    None => return Some("device command channel closed".to_string()),
                }
            }
            _ = &mut heartbeat_sleep => {
                let idle_for = last_ws_activity.elapsed();
                if heartbeat_is_stale(idle_for, heartbeat_timeout) {
                    return Some(format!(
                        "heartbeat timed out after {}ms idle",
                        idle_for.as_millis()
                    ));
                }
                if let Err(e) = ws::send(ws, &protocol::ClipMessage::Ping).await {
                    return Some(format!("heartbeat ping failed: {}", e));
                }
                heartbeat_sleep.as_mut().reset(Instant::now() + heartbeat_interval);
            }
            result = ws::recv(ws) => {
                match result {
                    Ok(protocol::ClipMessage::Push { text, .. }) => {
                        last_ws_activity = Instant::now();
                        let _ = event_tx.send(DeviceEvent::Push {
                            id: target.id.clone(),
                            text,
                        });
                    }
                    Ok(protocol::ClipMessage::Ping) => {
                        last_ws_activity = Instant::now();
                        let _ = ws::send(ws, &protocol::ClipMessage::Pong).await;
                    }
                    Ok(protocol::ClipMessage::Pong) => {
                        last_ws_activity = Instant::now();
                    }
                    Ok(_) => {}
                    Err(e) => return Some(e.to_string()),
                }
            }
        }
    }
}

fn config_path_for(exe_path: Option<&Path>, cwd: Option<&Path>) -> PathBuf {
    let exe_config = exe_path
        .and_then(Path::parent)
        .map(|parent| parent.join("clipsync.toml"));

    if let Some(path) = exe_config.as_ref().filter(|path| path.exists()) {
        return path.clone();
    }

    if let Some(path) = cwd
        .map(|cwd| cwd.join("clipsync.toml"))
        .filter(|path| path.exists())
    {
        return path;
    }

    exe_config.unwrap_or_else(|| PathBuf::from("clipsync.toml"))
}

fn resolve_config_path() -> PathBuf {
    let exe_path = std::env::current_exe().ok();
    let cwd = std::env::current_dir().ok();
    config_path_for(exe_path.as_deref(), cwd.as_deref())
}

async fn run_sync_loop(
    cfg: Arc<config::ClipSyncConfig>,
    proxy: EventLoopProxy<UiEvent>,
    mut action_rx: mpsc::UnboundedReceiver<TrayAction>,
    paused: Arc<AtomicBool>,
) {
    let (clip_tx, mut clip_rx) = mpsc::unbounded_channel::<String>();
    let listener = clip::ClipListener::new(clip_tx);
    listener.spawn();

    let mut engine = SyncEngine::new();
    let debounce = Duration::from_millis(cfg.clipboard.debounce_ms);
    let mut actions_closed = false;
    let (device_event_tx, mut device_event_rx) = mpsc::unbounded_channel::<DeviceEvent>();
    let mut handles: HashMap<DeviceId, DeviceHandle> = HashMap::new();
    let mut latest_text: Option<String> = None;
    let mut debounce_text: Option<String> = None;
    let mut debounce_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(Duration::MAX));

    let static_targets = configured_device_targets(&cfg);
    let auto_discovery = static_targets.is_empty();
    let mut discovery_rx = if auto_discovery {
        log::info!("No static devices configured; starting mDNS/LAN discovery");
        start_discovery(cfg.connection.port)
    } else {
        None
    };
    let mut discovery_retry_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(Duration::MAX));
    if auto_discovery && discovery_rx.is_none() {
        discovery_retry_sleep
            .as_mut()
            .reset(Instant::now() + Duration::from_secs(5));
    }

    for target in static_targets {
        register_device(
            target,
            &mut handles,
            cfg.clone(),
            device_event_tx.clone(),
            &mut engine,
            latest_text.as_deref(),
        );
    }

    send_aggregate_state(&proxy, &handles);

    loop {
        tokio::select! {
            uri = recv_discovery(&mut discovery_rx), if auto_discovery && discovery_rx.is_some() => {
                match uri {
                    Some(uri) => {
                        let target = target_from_uri(uri, None, DeviceOrigin::Discovered);
                        if register_device(
                            target,
                            &mut handles,
                            cfg.clone(),
                            device_event_tx.clone(),
                            &mut engine,
                            latest_text.as_deref(),
                        ) {
                            send_aggregate_state(&proxy, &handles);
                        }
                    }
                    None => {
                        log::warn!("Discovery channel closed; retrying discovery in 5s");
                        discovery_rx = None;
                        discovery_retry_sleep
                            .as_mut()
                            .reset(Instant::now() + Duration::from_secs(5));
                    }
                }
            }
            _ = &mut discovery_retry_sleep, if auto_discovery && discovery_rx.is_none() => {
                discovery_rx = start_discovery(cfg.connection.port);
                if discovery_rx.is_none() {
                    discovery_retry_sleep
                        .as_mut()
                        .reset(Instant::now() + Duration::from_secs(5));
                }
            }
            Some(text) = clip_rx.recv() => {
                if !paused.load(Ordering::Relaxed) {
                    log::info!("Queued PC clipboard for debounce: {} chars", text.chars().count());
                    debounce_text = Some(text);
                    debounce_sleep.as_mut().reset(Instant::now() + debounce);
                }
            }
            _ = &mut debounce_sleep, if debounce_text.is_some() => {
                let Some(text) = debounce_text.take() else {
                    continue;
                };
                if !paused.load(Ordering::Relaxed) && engine.should_broadcast(&text) {
                    let event_id = next_event_id();
                    latest_text = Some(text.clone());
                    broadcast_clipboard(&handles, &mut engine, text.clone(), event_id);
                    engine.mark_broadcast(&text);
                    log::info!("Broadcast PC clipboard: {} chars to {} devices", text.chars().count(), handles.len());
                }
            }
            Some(event) = device_event_rx.recv() => {
                match event {
                    DeviceEvent::StateChanged { id, state, last_error } => {
                        if let Some(handle) = handles.get_mut(&id) {
                            handle.state = state;
                            handle.last_error = last_error.clone();
                            match state {
                                DeviceConnectionState::Connected => {
                                    log::info!("Device connected: {}", handle.target.name);
                                    flush_pending_to_device(&mut engine, handle);
                                }
                                DeviceConnectionState::Disconnected => {
                                    if let Some(error) = last_error {
                                        log::warn!("Device disconnected: {} ({})", handle.target.name, error);
                                    } else {
                                        log::warn!("Device disconnected: {}", handle.target.name);
                                    }
                                }
                                DeviceConnectionState::Connecting => {
                                    log::info!("Device connecting: {}", handle.target.name);
                                }
                            }
                        }
                        send_aggregate_state(&proxy, &handles);
                    }
                    DeviceEvent::Push { id, text } => {
                        if !paused.load(Ordering::Relaxed) {
                            let ok = clip::write(&text);
                            let name = handles
                                .get(&id)
                                .map(|handle| handle.target.name.as_str())
                                .unwrap_or("unknown device");
                            log::info!(
                                "Received clipboard_push from {}: {} chars; PC clipboard write={}",
                                name,
                                text.chars().count(),
                                ok
                            );
                            if ok {
                                engine.mark_broadcast(&text);
                                latest_text = Some(text);
                            }
                        }
                    }
                    DeviceEvent::SendFailed { id, text, event_id, error } => {
                        log::warn!("Device send failed: id={} event={} error={}", id, event_id, error);
                        preserve_text_after_send_failure(&mut engine, &id, text);
                    }
                }
            }
            action = action_rx.recv(), if !actions_closed => {
                match action {
                    Some(action) if reconnect_interrupts_wait(action) => {
                        log::info!("Reconnect requested; reconnecting all devices");
                        for handle in handles.values() {
                            let _ = handle.tx.send(DeviceCommand::Reconnect);
                        }
                        if auto_discovery {
                            discovery_rx = start_discovery(cfg.connection.port);
                        }
                    }
                    Some(_) => {}
                    None => actions_closed = true,
                }
            }
        }
    }
}

fn main() -> anyhow::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let config_path = resolve_config_path();

    let app_config = config::ClipSyncConfig::load(&config_path)?;
    if app_config.has_empty_secret() {
        log::warn!(
            "auth.secret is empty in {}; HMAC auth will use an empty shared secret",
            config_path.display()
        );
    }
    log::info!("ClipSync PC starting... config={}", config_path.display());

    if let Err(e) = startup::set_autostart(app_config.general.start_with_windows) {
        log::error!("Failed to sync autostart registry on startup: {}", e);
    }

    let cfg = Arc::new(app_config.clone());
    let event_loop = EventLoop::<UiEvent>::with_user_event().build()?;
    let proxy = event_loop.create_proxy();

    let (action_tx, action_rx) = mpsc::unbounded_channel::<TrayAction>();

    let paused = Arc::new(AtomicBool::new(false));

    let rt = tokio::runtime::Runtime::new().unwrap();
    let tokio_handle = rt.handle().clone();

    let cfg_clone = cfg.clone();
    let proxy_clone = proxy.clone();
    let paused_clone = paused.clone();
    std::thread::spawn(move || {
        rt.block_on(run_sync_loop(
            cfg_clone,
            proxy_clone,
            action_rx,
            paused_clone,
        ));
    });

    let mut app = App::new(
        proxy,
        action_tx,
        paused,
        app_config,
        config_path,
        tokio_handle,
    );
    event_loop.set_control_flow(tray_event_loop_control_flow());
    event_loop.run_app(&mut app)?;

    Ok(())
}

fn tray_event_loop_control_flow() -> ControlFlow {
    ControlFlow::Wait
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn heartbeat_times_out_after_idle_deadline() {
        assert!(!heartbeat_is_stale(
            Duration::from_millis(14_999),
            Duration::from_millis(15_000)
        ));
        assert!(heartbeat_is_stale(
            Duration::from_millis(15_000),
            Duration::from_millis(15_000)
        ));
    }

    #[test]
    fn reconnect_action_interrupts_waits() {
        assert!(reconnect_interrupts_wait(TrayAction::Reconnect));
        assert!(!reconnect_interrupts_wait(TrayAction::TogglePause));
    }

    #[test]
    fn aggregate_state_reports_connected_device_count() {
        let mut handles = HashMap::new();
        let phone = target_from_uri(
            "ws://192.168.0.10:5287/ws".to_string(),
            Some("Phone".to_string()),
            DeviceOrigin::Static,
        );
        let tablet = target_from_uri(
            "ws://192.168.0.11:5287/ws".to_string(),
            Some("Tablet".to_string()),
            DeviceOrigin::Static,
        );
        let (phone_tx, _phone_rx) = mpsc::unbounded_channel();
        let (tablet_tx, _tablet_rx) = mpsc::unbounded_channel();
        handles.insert(
            phone.id.clone(),
            DeviceHandle {
                target: phone,
                tx: phone_tx,
                state: DeviceConnectionState::Connected,
                last_error: None,
            },
        );
        handles.insert(
            tablet.id.clone(),
            DeviceHandle {
                target: tablet,
                tx: tablet_tx,
                state: DeviceConnectionState::Connected,
                last_error: None,
            },
        );

        let state = aggregate_conn_state(&handles);
        match state {
            ConnState::Connected { devices } => {
                assert_eq!(devices.len(), 2);
                assert_eq!(devices[0].ip, "192.168.0.10");
                assert_eq!(devices[0].name.as_deref(), Some("Phone"));
                assert_eq!(devices[0].state, DeviceSummaryState::Connected);
                assert_eq!(devices[1].ip, "192.168.0.11");
            }
            _ => panic!("wrong aggregate state: {:?}", state),
        }
    }

    #[test]
    fn configured_targets_keep_legacy_direct_uri() {
        let mut cfg = config::ClipSyncConfig::default();
        cfg.connection.uri = Some("ws://192.168.0.10:5287/ws".into());

        let targets = configured_device_targets(&cfg);

        assert_eq!(targets.len(), 1);
        assert_eq!(targets[0].uri, "ws://192.168.0.10:5287/ws");
        assert_eq!(targets[0].name, "");
        assert_eq!(targets[0].origin, DeviceOrigin::Static);
    }

    #[test]
    fn configured_targets_include_enabled_devices_and_skip_duplicates() {
        let toml_str = r#"
[[devices]]
name = "Phone"
uri = "ws://192.168.0.10:5287/ws"

[[devices]]
name = "Duplicate Phone"
uri = "ws://192.168.0.10:5287/ws"

[[devices]]
name = "Disabled Tablet"
uri = "ws://192.168.0.11:5287/ws"
enabled = false
"#;
        let cfg: config::ClipSyncConfig = toml::from_str(toml_str).unwrap();

        let targets = configured_device_targets(&cfg);

        assert_eq!(targets.len(), 1);
        assert_eq!(targets[0].name, "Phone");
        assert_eq!(targets[0].uri, "ws://192.168.0.10:5287/ws");
    }

    #[test]
    fn broadcast_queues_pending_only_for_disconnected_devices() {
        let connected = target_from_uri(
            "ws://192.168.0.10:5287/ws".to_string(),
            Some("Connected".to_string()),
            DeviceOrigin::Static,
        );
        let disconnected = target_from_uri(
            "ws://192.168.0.11:5287/ws".to_string(),
            Some("Disconnected".to_string()),
            DeviceOrigin::Static,
        );
        let disconnected_id = disconnected.id.clone();
        let mut handles = HashMap::new();
        let (connected_tx, mut connected_rx) = mpsc::unbounded_channel();
        let (disconnected_tx, _disconnected_rx) = mpsc::unbounded_channel();
        handles.insert(
            connected.id.clone(),
            DeviceHandle {
                target: connected,
                tx: connected_tx,
                state: DeviceConnectionState::Connected,
                last_error: None,
            },
        );
        handles.insert(
            disconnected.id.clone(),
            DeviceHandle {
                target: disconnected,
                tx: disconnected_tx,
                state: DeviceConnectionState::Disconnected,
                last_error: None,
            },
        );
        let mut engine = SyncEngine::new();

        broadcast_clipboard(&handles, &mut engine, "hello".into(), "evt-1".into());

        assert!(matches!(
            connected_rx.try_recv(),
            Ok(DeviceCommand::SendClipboard { text, event_id })
                if text == "hello" && event_id == "evt-1"
        ));
        assert_eq!(
            engine.take_pending_for(&disconnected_id),
            Some("hello".into())
        );
    }

    #[test]
    fn handshake_timeout_uses_heartbeat_timeout() {
        let mut cfg = config::ClipSyncConfig::default();
        cfg.connection.heartbeat_timeout_ms = 12_345;
        assert_eq!(handshake_timeout(&cfg), Duration::from_millis(12_345));
    }

    #[test]
    fn config_path_prefers_existing_cwd_config_when_exe_config_is_missing() {
        let cwd = std::env::current_dir().unwrap();
        let exe = cwd.join("target").join("debug").join("clipsync-pc.exe");
        assert_eq!(
            config_path_for(Some(&exe), Some(&cwd)),
            cwd.join("clipsync.toml")
        );
    }
}
