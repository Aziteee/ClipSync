#![windows_subsystem = "windows"]

mod clip;
mod config;
mod mdns;
mod protocol;
mod sync;
mod tray;
mod ws;

use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use sync::SyncEngine;
use tokio::sync::mpsc;
use tokio::time::Sleep;
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

struct App {
    tray: Option<Tray>,
    proxy: EventLoopProxy<UiEvent>,
    action_tx: std::sync::mpsc::Sender<TrayAction>,
    paused: Arc<AtomicBool>,
}

impl App {
    fn new(
        proxy: EventLoopProxy<UiEvent>,
        action_tx: std::sync::mpsc::Sender<TrayAction>,
        paused: Arc<AtomicBool>,
    ) -> Self {
        Self {
            tray: None,
            proxy,
            action_tx,
            paused,
        }
    }
}

impl ApplicationHandler<UiEvent> for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.tray.is_none() {
            match Tray::new(event_loop, self.proxy.clone()) {
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
        let _ = self.action_tx.send(action);
    }
}

async fn run_sync_loop(
    cfg: Arc<config::ClipSyncConfig>,
    proxy: EventLoopProxy<UiEvent>,
    _action_rx: std::sync::mpsc::Receiver<TrayAction>,
    paused: Arc<AtomicBool>,
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
        let (uri, origin) = if let Some(uri) = cfg.connection.direct_ws_uri() {
            log::info!("State: Disconnected — using configured endpoint {}", uri);
            (uri, EndpointOrigin::Direct)
        } else {
            log::info!("State: Disconnected — starting mDNS discovery");

            let Ok(mut mdns_rx) = mdns::discover(cfg.connection.port) else {
                log::error!("mDNS discovery failed, retrying in 5s");
                tokio::time::sleep(Duration::from_secs(5)).await;
                continue;
            };

            loop {
                tokio::select! {
                    Some(uri) = mdns_rx.recv() => break (uri, EndpointOrigin::Discovered),
                Some(text) = clip_rx.recv() => {
                    if !paused.load(Ordering::Relaxed) {
                        log::info!("Queued pending PC clipboard while disconnected: {} chars", text.chars().count());
                        engine.store_pending(text);
                    }
                }
                    _ = tokio::time::sleep(Duration::from_secs(5)) => {}
                }
            }
        };

        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Connecting));

        let ws = loop {
            log::info!("Connecting to {}...", uri);
            match ws::connect_and_auth(&uri, &cfg.auth.secret).await {
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
                    tokio::time::sleep(Duration::from_secs(backoff_secs)).await;
                    backoff_secs = (backoff_secs * 2).min(max_backoff);
                }
            }
        };
        let Some(mut ws) = ws else {
            let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Disconnected));
            backoff_secs = 1;
            continue;
        };
        backoff_secs = 1;

        let _ = proxy.send_event(UiEvent::StateChanged(ConnState::Connected));

        if let Some(pending_text) = engine.take_pending() {
            let msg = protocol::ClipMessage::set(pending_text);
            match ws::send(&mut ws, &msg).await {
                Ok(()) => log::info!("Sent pending clipboard_set"),
                Err(e) => log::error!("Failed to send pending clipboard_set: {}", e),
            }
        }

        let mut debounce_text: Option<String> = None;
        let mut debounce_sleep: Pin<Box<Sleep>> = Box::pin(tokio::time::sleep(Duration::MAX));

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
                    let text = debounce_text.take().unwrap();
                    if !paused.load(Ordering::Relaxed) && engine.should_send(&text) {
                        let msg = protocol::ClipMessage::set(text.clone());
                        if let Err(e) = ws::send(&mut ws, &msg).await {
                            log::error!("Failed to send clipboard_set: {}", e);
                            break 'connected;
                        }
                        log::info!("Sent clipboard_set: {} chars", text.chars().count());
                        engine.mark_sent(&text);
                    }
                }
                result = ws::recv(&mut ws) => {
                    match result {
                        Ok(protocol::ClipMessage::Push { text, .. }) => {
                            if !paused.load(Ordering::Relaxed) {
                                let ok = clip::write(&text);
                                log::info!("Received clipboard_push: {} chars; PC clipboard write={}", text.chars().count(), ok);
                                engine.mark_sent(&text);
                            }
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
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let cfg = Arc::new(config::ClipSyncConfig::load("clipsync.toml")?);
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);

    let event_loop = EventLoop::<UiEvent>::with_user_event().build()?;
    let proxy = event_loop.create_proxy();

    let (action_tx, action_rx) = std::sync::mpsc::channel::<TrayAction>();

    let paused = Arc::new(AtomicBool::new(false));

    let cfg_clone = cfg.clone();
    let proxy_clone = proxy.clone();
    let paused_clone = paused.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().unwrap();
        rt.block_on(run_sync_loop(
            cfg_clone,
            proxy_clone,
            action_rx,
            paused_clone,
        ));
    });

    let mut app = App::new(proxy, action_tx, paused);
    event_loop.set_control_flow(tray_event_loop_control_flow());
    event_loop.run_app(&mut app)?;

    Ok(())
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
}

fn tray_event_loop_control_flow() -> ControlFlow {
    ControlFlow::Wait
}
