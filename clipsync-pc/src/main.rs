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

use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tokio::time::{Instant, Sleep};
use tray::{ConnState, Tray, TrayAction};
use winit::application::ApplicationHandler;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy};

#[derive(Debug)]
pub(crate) enum UiEvent {
    StateChanged(ConnState),
    TrayAction(TrayAction),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum EndpointOrigin {
    Direct,
    Discovered,
}

fn should_retry_failed_endpoint(origin: EndpointOrigin) -> bool {
    origin == EndpointOrigin::Direct
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
    if proxy.send_event(UiEvent::StateChanged(state)).is_err() {
        log::debug!("Failed to send UI state update: {:?}", state);
    }
}

fn preserve_text_after_send_failure(engine: &mut SyncEngine, text: String) {
    engine.store_pending(text);
}

fn reconnect_interrupts_wait(action: TrayAction) -> bool {
    matches!(action, TrayAction::Reconnect)
}

fn state_after_connected_loop_exit() -> ConnState {
    ConnState::Disconnected
}

fn handshake_timeout(cfg: &config::ClipSyncConfig) -> Duration {
    Duration::from_millis(cfg.connection.heartbeat_timeout_ms)
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
    let heartbeat_interval = Duration::from_millis(cfg.connection.heartbeat_interval_ms);
    let heartbeat_timeout = Duration::from_millis(cfg.connection.heartbeat_timeout_ms);
    let mut backoff_secs = 1u64;
    let max_backoff = 60u64;
    let mut actions_closed = false;

    send_state(&proxy, ConnState::Disconnected);

    'sync: loop {
        let endpoint = if let Some(uri) = cfg.connection.direct_ws_uri() {
            log::info!("State: Disconnected — using configured endpoint {}", uri);
            Some((uri, EndpointOrigin::Direct))
        } else {
            log::info!("State: Disconnected — starting mDNS discovery");

            let Ok(mut mdns_rx) = mdns::discover(cfg.connection.port) else {
                log::error!("mDNS discovery failed, retrying in 5s");
                tokio::select! {
                    _ = tokio::time::sleep(Duration::from_secs(5)) => {}
                    action = action_rx.recv(), if !actions_closed => {
                        match action {
                            Some(action) if reconnect_interrupts_wait(action) => {
                                log::info!("Reconnect requested while mDNS setup was backing off");
                            }
                            Some(_) => {}
                            None => actions_closed = true,
                        }
                    }
                }
                continue;
            };

            let mut discovered = None;
            while discovered.is_none() {
                tokio::select! {
                    uri = mdns_rx.recv() => {
                        match uri {
                            Some(uri) => discovered = Some((uri, EndpointOrigin::Discovered)),
                            None => {
                                log::warn!("mDNS discovery channel closed; restarting discovery");
                                break;
                            }
                        }
                    }
                    Some(text) = clip_rx.recv() => {
                        if !paused.load(Ordering::Relaxed) {
                            log::info!("Queued pending PC clipboard while disconnected: {} chars", text.chars().count());
                            engine.store_pending(text);
                        }
                    }
                    action = action_rx.recv(), if !actions_closed => {
                        match action {
                            Some(action) if reconnect_interrupts_wait(action) => {
                                log::info!("Reconnect requested while discovering; restarting discovery");
                                continue 'sync;
                            }
                            Some(_) => {}
                            None => actions_closed = true,
                        }
                    }
                    _ = tokio::time::sleep(Duration::from_secs(5)) => {}
                }
            }
            discovered
        };

        let Some((uri, origin)) = endpoint else {
            send_state(&proxy, ConnState::Disconnected);
            continue;
        };

        send_state(&proxy, ConnState::Connecting);

        let ws = loop {
            log::info!("Connecting to {}...", uri);
            match ws::connect_and_auth(&uri, &cfg.auth.secret, handshake_timeout(&cfg)).await {
                Ok(ws) => {
                    log::info!("Connected and authenticated!");
                    break Some(ws);
                }
                Err(e) => {
                    log::error!("Connection failed: {}", e);
                    if !should_retry_failed_endpoint(origin) {
                        log::info!("Discarding discovered endpoint and resuming discovery");
                        break None;
                    }
                    log::info!("Retrying in {}s...", backoff_secs);
                    let retry_after = backoff_secs;
                    backoff_secs = (backoff_secs * 2).min(max_backoff);
                    tokio::select! {
                        _ = tokio::time::sleep(Duration::from_secs(retry_after)) => {}
                        action = action_rx.recv(), if !actions_closed => {
                            match action {
                                Some(action) if reconnect_interrupts_wait(action) => {
                                    log::info!("Reconnect requested during direct endpoint backoff");
                                    backoff_secs = 1;
                                }
                                Some(_) => {}
                                None => actions_closed = true,
                            }
                        }
                    }
                }
            }
        };
        let Some(mut ws) = ws else {
            send_state(&proxy, ConnState::Disconnected);
            backoff_secs = 1;
            continue;
        };
        backoff_secs = 1;

        send_state(&proxy, ConnState::Connected);

        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text.clone());
            match ws::send(&mut ws, &msg).await {
                Ok(()) => log::info!("Sent pending clipboard_set"),
                Err(e) => {
                    log::error!("Failed to send pending clipboard_set: {}", e);
                    preserve_text_after_send_failure(&mut engine, pending_text);
                    send_state(&proxy, state_after_connected_loop_exit());
                    continue;
                }
            }
        }

        let mut debounce_text: Option<String> = None;
        let mut debounce_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(Duration::MAX));
        let mut heartbeat_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(heartbeat_interval));
        let mut last_ws_activity = Instant::now();

        'connected: loop {
            tokio::select! {
                Some(text) = clip_rx.recv() => {
                    if !paused.load(Ordering::Relaxed) {
                        log::info!("Queued PC clipboard for debounce: {} chars", text.chars().count());
                        debounce_text = Some(text);
                        debounce_sleep.as_mut().reset(tokio::time::Instant::now() + debounce);
                    }
                }
                _ = &mut debounce_sleep, if debounce_text.is_some() => {
                    let Some(text) = debounce_text.take() else {
                        continue;
                    };
                    if !paused.load(Ordering::Relaxed) && engine.should_send(&text) {
                        let msg = protocol::ClipMessage::set(text.clone());
                        if let Err(e) = ws::send(&mut ws, &msg).await {
                            log::error!("Failed to send clipboard_set: {}", e);
                            preserve_text_after_send_failure(&mut engine, text);
                            break 'connected;
                        }
                        log::info!("Sent clipboard_set: {} chars", text.chars().count());
                        engine.mark_sent(&text);
                    }
                }
                _ = &mut heartbeat_sleep => {
                    let idle_for = last_ws_activity.elapsed();
                    if heartbeat_is_stale(idle_for, heartbeat_timeout) {
                        log::warn!("WebSocket heartbeat timed out after {}ms idle", idle_for.as_millis());
                        break 'connected;
                    }
                    if let Err(e) = ws::send(&mut ws, &protocol::ClipMessage::Ping).await {
                        log::error!("Failed to send heartbeat ping: {}", e);
                        break 'connected;
                    }
                    heartbeat_sleep.as_mut().reset(Instant::now() + heartbeat_interval);
                }
                result = ws::recv(&mut ws) => {
                    match result {
                        Ok(protocol::ClipMessage::Push { text, .. }) => {
                            last_ws_activity = Instant::now();
                            if !paused.load(Ordering::Relaxed) {
                                let ok = clip::write(&text);
                                log::info!("Received clipboard_push: {} chars; PC clipboard write={}", text.chars().count(), ok);
                                if ok {
                                    engine.mark_sent(&text);
                                }
                            }
                        }
                        Ok(protocol::ClipMessage::Ping) => {
                            last_ws_activity = Instant::now();
                            let _ = ws::send(&mut ws, &protocol::ClipMessage::Pong).await;
                        }
                        Ok(protocol::ClipMessage::Pong) => {
                            last_ws_activity = Instant::now();
                        }
                        Err(_) => break 'connected,
                        _ => {}
                    }
                }
                action = action_rx.recv(), if !actions_closed => {
                    match action {
                        Some(action) if reconnect_interrupts_wait(action) => {
                            log::info!("Reconnect requested; closing current connection");
                            if let Some(text) = debounce_text.take() {
                                preserve_text_after_send_failure(&mut engine, text);
                            }
                            break 'connected;
                        }
                        Some(_) => {}
                        None => actions_closed = true,
                    }
                }
            }
        }

        log::info!("State: Disconnected");
        send_state(&proxy, state_after_connected_loop_exit());
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

    let mut app = App::new(proxy, action_tx, paused, app_config, config_path, tokio_handle);
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
    fn discovered_endpoint_failure_returns_to_discovery() {
        assert!(!should_retry_failed_endpoint(EndpointOrigin::Discovered));
        assert!(should_retry_failed_endpoint(EndpointOrigin::Direct));
    }

    #[test]
    fn tray_app_waits_for_events_instead_of_polling() {
        assert_eq!(tray_event_loop_control_flow(), ControlFlow::Wait);
    }

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
    fn failed_send_text_is_preserved_as_pending() {
        let mut engine = SyncEngine::new();
        preserve_text_after_send_failure(&mut engine, "offline".into());
        assert_eq!(engine.take_pending(), Some("offline".into()));
    }

    #[test]
    fn reconnect_action_interrupts_waits() {
        assert!(reconnect_interrupts_wait(TrayAction::Reconnect));
        assert!(!reconnect_interrupts_wait(TrayAction::TogglePause));
    }

    #[test]
    fn connected_loop_exit_reports_disconnected() {
        assert_eq!(state_after_connected_loop_exit(), ConnState::Disconnected);
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
